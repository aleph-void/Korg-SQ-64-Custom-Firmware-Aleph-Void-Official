/* Aleph Void aggregate firmware for the SQ-64: boot mode selection and
 * normal mode.
 *
 * Each of the four tracks runs one of two engines, toggled per track with
 * SHIFT + its track button:
 *
 *  - MODE_SEQ: a Euclidean rhythm E(pulses, length) with rotation, up to 64
 *    steps (four bars of 16). Tracks A/B/C drive their gate jack, pitch CV,
 *    mod CV and MIDI notes on channels 1..3; track D drives trigger jack 1
 *    and MIDI channel 10.
 *  - MODE_LFO: a tempo-synced LFO with waveform, rate division, depth,
 *    phase offset and a MIDI CC streamed on channels 1..4. LFOs A/B/C can
 *    drive their mod CV, pitch CV and gate jacks; LFO D pulses trigger
 *    jack 1 once per cycle.
 *
 * Both engines hang off one shared 24 PPQN transport position, advanced by
 * the internal tempo clock or following incoming MIDI clock automatically,
 * so sequencer steps and LFO phases stay locked to the same beat. Pads and
 * the param encoders edit whichever engine the selected track runs, and the
 * display shows that engine — the active program — alone; tracks running the
 * other engine are only tagged, never drawn in their own idiom alongside it.
 * GLOBAL commits settings to SPI flash, and the power button saves and shuts
 * down cleanly.
 *
 * The spec's panel-key requirements (TRACK/MODE/LOOP/BAR-001): the PITCH,
 * GATE, MOD and CONTROL keys each toggle an edit mode that scopes the pads
 * and param encoders to that mode's documented parameters on the selected
 * track — on a sequencer lane one pad-edited parameter (note, gate length,
 * mod CV level, velocity), on an LFO lane that destination's routing plus
 * its companion parameter (phase, step gate, depth, MIDI CC). Keys 1..4
 * select which 16-step bar of the 64-step pattern space the pads, pad LEDs
 * and display lanes address; LOOP loops step playback over the selected bar
 * of the selected sequencer lane's pattern. Mode entry/exit, bar selection
 * and loop toggles alone never change stored data.
 *
 * PROJECT toggles project mode, where the 64 pads manage the 64 saved
 * project slots of the board contract's catalog: pad = load, SHIFT+pad =
 * save, RECORD+pad = delete. SHIFT+PROJECT starts a new project (workspace
 * back to the declared defaults) without touching any saved slot
 * (PROJECT-001..004). A project carries both engine modes: each track's
 * engine selection and both its sequencer and LFO configurations.
 */
#include <stdbool.h>

#include "board.h"
#include "display.h"
#include "midi.h"
#include "outputs.h"
#include "panel.h"
#include "storage.h"
#include "updater.h"

volatile uint32_t g_ms;

void tick_irq_handler(void)
{
    REG32(TICK_TIM + 0x10) = 0; /* clear update flag */
    g_ms++;
}

/* Synced LFO rate divisions in 24 PPQN clock ticks, slowest first. */
static const uint16_t DIV_TICKS[DIV_COUNT] = {
    768, 384, 192, 96, 72, 48, 36, 24, 18, 16, 12, 9, 8, 6, 4, 3,
};
static const char *const DIV_NAMES[DIV_COUNT] = {
    "8BAR", "4BAR", "2BAR", "1BAR", "1/2D", "1/2", "1/4D", "1/4",
    "1/8D", "1/4T", "1/8", "1/16D", "1/8T", "1/16", "1/16T", "1/32",
};
static const char *const WAVE_NAMES[WAVE_COUNT] = {
    "SINE", "TRI", "RISE", "FALL", "SQR", "S+H",
};
static const char *const NOTE_NAMES[12] = {
    "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B",
};

#define STEP_T256 1536u /* one sixteenth = 6 ticks in 1/256-tick units */

static settings_t cfg;
static bool playing;

/* Project manager (PROJECT button): the 64 pads select the 64 saved project
 * slots, bank A pad 1 = slot 0 .. bank D pad 16 = slot 63. */
#define SLOT_NONE 0xff
static bool project_mode;
static uint8_t cur_slot = SLOT_NONE;     /* last loaded/saved slot */
static uint8_t slot_states[PROJECT_SLOTS];
static const char *proj_msg;
static int proj_msg_slot = -1;           /* -1: message without a slot name */
static uint32_t proj_msg_ms;

/* Shared transport position: integer 24 PPQN ticks plus a 1/65536-tick
 * fraction. Sequencer steps and LFO phases are both derived from it, never
 * free-running, so a position reset or an external-clock snap re-syncs
 * every lane at once. */
static uint32_t pos_tick;
static uint32_t pos_frac;     /* 0..65535 */
static uint32_t last_ms;

/* External MIDI clock: measured tick spacing and arrival time. The clock
 * source switches to external automatically while ticks keep arriving. */
static uint32_t ext_last_ms;
static uint32_t ext_gap_ms;   /* 0 = no usable spacing measured yet */

/* Sequencer engine state. */
static uint32_t global_step;
static uint32_t next_step256;   /* 1/256-tick timestamps, wrap-safe compares */
static uint8_t active_note[4] = {0xff, 0xff, 0xff, 0xff};
static uint32_t gate_off_ms[4];

/* LFO engine state. */
static uint16_t lfo_phase_raw[4]; /* pre-offset phase, for wrap detection */
static uint16_t lfo_rand[4] = {0x5a21, 0xc3f1, 0x90b7, 0x1e65};
static uint16_t lfo_out[4];       /* post-depth level, for LEDs and UI */
static uint8_t cc_last[4] = {0xff, 0xff, 0xff, 0xff};
static uint32_t cc_sent_ms[4];

static uint32_t trigger_off_ms;
static uint32_t tap_last_ms;
static volatile bool rt_start, rt_stop;

/* Edit modes (MODE-001): the PITCH/GATE/MOD/CONTROL keys scope what the
 * pads and param encoders edit on the selected track; pressing the active
 * mode's key again leaves it. Entry and exit change no data. */
enum { EDIT_NONE, EDIT_PITCH, EDIT_GATE, EDIT_MOD, EDIT_CONTROL };
static uint8_t edit_mode;

/* Bar selection (BAR-001): keys 1..4 pick which 16-step bar of the 64-step
 * pattern space the pads, pad LEDs and display lanes address. */
static uint8_t edit_bar; /* 0..3 */

/* Loop (LOOP-001): a loop captures the selected bar of the selected
 * sequencer lane's pattern when switched on and wraps the shared step
 * counter inside it. It is refused on an LFO lane or when the bar lies
 * beyond the pattern, clipped to the pattern end, and cleared if a later
 * edit moves the pattern end before the region. */
static bool loop_on;
static uint8_t loop_track, loop_start, loop_len;

static bool ext_active(void)
{
    return ext_gap_ms != 0 && g_ms - ext_last_ms < 1000;
}

static void realtime_in(uint8_t byte)
{
    if (byte == 0xfa) {
        rt_start = true;
    } else if (byte == 0xfc) {
        rt_stop = true;
    } else if (byte == 0xf8) {
        uint32_t now = g_ms;
        uint32_t gap = now - ext_last_ms;
        ext_last_ms = now;
        if (gap >= 1 && gap <= 1000) {
            /* Snap to the next tick boundary: external clock owns the beat. */
            ext_gap_ms = gap;
            pos_tick++;
            pos_frac = 0;
        } else {
            ext_gap_ms = 0; /* first tick after silence only arms the sync */
        }
    }
}

static void clock_advance_ms(void)
{
    if (ext_active()) {
        /* Interpolate between external ticks but never cross the boundary
         * before the real tick arrives. */
        uint32_t frac = pos_frac + 65536u / ext_gap_ms;
        pos_frac = frac > 65535u ? 65535u : frac;
        return;
    }
    pos_frac += (uint32_t)cfg.bpm * 65536u / 2500u; /* bpm*24/60000 ticks/ms */
    if (pos_frac >= 65536u) {
        pos_frac -= 65536u;
        pos_tick++;
        if (playing) midi_send((const uint8_t[]){0xf8}, 1);
    }
}

static uint32_t pos256(void)
{
    return (pos_tick << 8) | (pos_frac >> 8);
}

static uint32_t step_period_ms(void)
{
    return ext_active() ? ext_gap_ms * 6 : 15000u / cfg.bpm;
}

/* ------------------------------------------------------------------ */
/* Euclidean sequencer engine                                          */

static uint64_t euclid_pattern(const track_cfg_t *t)
{
    uint64_t bits = 0;
    for (int i = 0; i < t->length; i++) {
        int j = (i + t->length - t->rotate) % t->length;
        if ((j * t->pulses) % t->length < t->pulses)
            bits |= 1ull << i;
    }
    return bits;
}

/* The pattern is read every main-loop iteration (LEDs) and per lane per
 * redraw, but its software modulos are expensive enough to let a redraw
 * outrun the panel scan buffer; recompute only when the track changes. */
static uint64_t track_pattern(int t)
{
    static uint8_t key[4][3];
    static uint64_t cache[4];
    const track_cfg_t *c = &cfg.track[t];
    if (key[t][0] != c->length || key[t][1] != c->pulses || key[t][2] != c->rotate) {
        key[t][0] = c->length;
        key[t][1] = c->pulses;
        key[t][2] = c->rotate;
        cache[t] = euclid_pattern(c);
    }
    return cache[t];
}

static void send_note(int track, uint8_t status, uint8_t note)
{
    uint8_t channel = track < 3 ? (uint8_t)track : 9;
    uint8_t velocity = status == 0x90 ? cfg.track[track].velocity : 0;
    uint8_t msg[3] = {(uint8_t)(status | channel), note, velocity};
    midi_send(msg, 3);
}

static void note_off(int track)
{
    if (active_note[track] == 0xff) return;
    send_note(track, 0x80, active_note[track]);
    active_note[track] = 0xff;
    if (track < 3) {
        gate_set(track, false);
        cv_mod(track, 0);
    }
}

static void fire_step(uint32_t now)
{
    uint32_t period = step_period_ms();
    for (int t = 0; t < 4; t++) {
        if (cfg.mode[t] != MODE_SEQ) continue;
        const track_cfg_t *c = &cfg.track[t];
        int pos = (int)(global_step % c->length);
        if (!((track_pattern(t) >> pos) & 1)) continue;
        note_off(t);
        send_note(t, 0x90, c->note);
        active_note[t] = c->note;
        uint32_t gate = period * c->gate_pct / 100;
        gate_off_ms[t] = now + (gate < 5 ? 5 : gate);
        if (t < 3) {
            gate_set(t, true);
            cv_pitch(t, c->note);
            cv_mod(t, (uint8_t)(c->mod * 17));
        } else {
            panel_triggers(1);
            trigger_off_ms = now + 20;
        }
    }
    global_step++;
    if (loop_on && (global_step < loop_start
                    || global_step >= (uint32_t)loop_start + loop_len))
        global_step = loop_start;
}

static void sequencer_tick(void)
{
    uint32_t now = g_ms;
    for (int t = 0; t < 4; t++)
        if (active_note[t] != 0xff && (int32_t)(now - gate_off_ms[t]) >= 0)
            note_off(t);
    if (!playing) return;
    uint32_t t256 = pos256();
    if ((int32_t)(t256 - next_step256) >= 0) {
        fire_step(now);
        /* Swing shifts every second sixteenth later within its pair. */
        uint32_t offset = STEP_T256 * (uint32_t)(cfg.swing - 50) / 50;
        next_step256 += (global_step & 1) ? STEP_T256 + offset : STEP_T256 - offset;
    }
}

/* ------------------------------------------------------------------ */
/* LFO engine                                                          */

static uint16_t wave_value(int i, uint16_t phase)
{
    uint32_t v;
    switch (cfg.lfo[i].wave) {
    case WAVE_SINE: { /* smoothstep of the triangle: a round LFO sine */
        uint32_t t = phase < 32768 ? phase * 2u : (65535u - phase) * 2u;
        uint32_t w = (t * t) >> 16;
        v = 3u * w - 2u * ((w * t) >> 16);
        break;
    }
    case WAVE_TRI: v = phase < 32768 ? phase * 2u : (65535u - phase) * 2u; break;
    case WAVE_RISE: v = phase; break;
    case WAVE_FALL: v = 65535u - phase; break;
    case WAVE_SQR: v = phase < 32768 ? 65535u : 0u; break;
    default: v = lfo_rand[i]; break; /* sample & hold */
    }
    return v > 65535u ? 65535u : (uint16_t)v;
}

static void prng_step(int i)
{
    uint16_t r = lfo_rand[i];
    r ^= (uint16_t)(r << 7);
    r ^= (uint16_t)(r >> 9);
    r ^= (uint16_t)(r << 8);
    lfo_rand[i] = r ? r : 0x7321;
}

static void lfo_update(void)
{
    uint32_t now = g_ms;
    for (int i = 0; i < 4; i++) {
        if (cfg.mode[i] != MODE_LFO) {
            lfo_out[i] = 0;
            continue;
        }
        const lfo_cfg_t *c = &cfg.lfo[i];
        uint32_t period = DIV_TICKS[c->div];
        uint16_t raw = (uint16_t)(((pos_tick % period) << 16 | pos_frac) / period);
        if (raw < lfo_phase_raw[i]) { /* cycle wrapped */
            prng_step(i);
            if (i == 3 && (c->dest & DEST_GATE)) {
                panel_triggers(1);
                trigger_off_ms = now + 20;
            }
        }
        lfo_phase_raw[i] = raw;
        uint16_t value = wave_value(i, (uint16_t)(raw + c->phase * 4096u));
        uint16_t out = (uint16_t)((uint32_t)value * c->depth / 100u);
        lfo_out[i] = out;
        if (i < 3) {
            if (c->dest & DEST_MOD) cv_mod16(i, out);
            if (c->dest & DEST_PITCH) cv_pitch16(i, out);
            if (c->dest & DEST_GATE) gate_set(i, value >= 32768);
        }
        uint8_t cc_value = (uint8_t)(out >> 9);
        if (cc_value != cc_last[i] && now - cc_sent_ms[i] >= 10) {
            uint8_t msg[3] = {(uint8_t)(0xb0 | i), c->cc, cc_value};
            midi_send(msg, 3);
            cc_last[i] = cc_value;
            cc_sent_ms[i] = now;
        }
    }
    if (trigger_off_ms && (int32_t)(now - trigger_off_ms) >= 0) {
        panel_triggers(0);
        trigger_off_ms = 0;
    }
}

/* ------------------------------------------------------------------ */
/* Transport and engine switching                                      */

static void position_reset(void)
{
    pos_tick = 0;
    pos_frac = 0;
    global_step = 0;
    next_step256 = 0;
    for (int i = 0; i < 4; i++) lfo_phase_raw[i] = 0;
}

static void transport_start(void)
{
    playing = true;
    position_reset();
    if (loop_on) global_step = loop_start; /* play starts inside the loop */
    midi_send((const uint8_t[]){0xfa}, 1);
}

static void transport_stop(void)
{
    playing = false;
    for (int t = 0; t < 4; t++) note_off(t);
    if (cfg.mode[3] == MODE_SEQ) {
        panel_triggers(0);
        trigger_off_ms = 0;
    }
    midi_send((const uint8_t[]){0xfc}, 1);
}

static void dest_cleared(int track, uint8_t bit)
{
    if (track < 3) {
        if (bit == DEST_MOD) cv_mod16(track, 0);
        if (bit == DEST_PITCH) cv_pitch16(track, 0);
        if (bit == DEST_GATE) gate_set(track, false);
    } else if (bit == DEST_GATE && !trigger_off_ms) {
        panel_triggers(0);
    }
}

/* Routing edits inside the PITCH/GATE/MOD edit modes (MODE-001) for an LFO
 * lane: the Param 1 encoder turns the mode's destination on (right) or off
 * (left) for the selected track. Tracks A..C own their jacks; on LFO D only
 * the GATE mode's cycle-trigger routing exists. */
static void route_turn(uint8_t bit, int detents)
{
    lfo_cfg_t *c = &cfg.lfo[cfg.selected];
    if (cfg.selected == 3 && bit != DEST_GATE) return; /* D is MIDI + trigger */
    uint8_t want = detents > 0 ? bit : 0;
    if ((c->dest & bit) == want) return;
    c->dest ^= bit;
    if (!want) dest_cleared(cfg.selected, bit);
}

static void loop_toggle(void)
{
    if (loop_on) {
        loop_on = false; /* clear: the step counter runs free again */
        return;
    }
    if (cfg.mode[cfg.selected] != MODE_SEQ) return; /* needs a sequencer lane */
    const track_cfg_t *c = &cfg.track[cfg.selected];
    uint8_t start = (uint8_t)(edit_bar * 16);
    if (start >= c->length) return; /* bar beyond the pattern: refused */
    loop_track = cfg.selected;
    loop_start = start;
    loop_len = (uint8_t)(c->length - start > 16 ? 16 : c->length - start);
    loop_on = true;
}

/* An edit (pad, encoder, engine toggle, project load, new project) may move
 * the looped pattern's end or switch its lane's engine: clip the loop to the
 * pattern, or clear a loop left stranded. */
static void loop_revalidate(void)
{
    if (!loop_on) return;
    const track_cfg_t *c = &cfg.track[loop_track];
    if (cfg.mode[loop_track] != MODE_SEQ || loop_start >= c->length) {
        loop_on = false;
        return;
    }
    uint8_t span = (uint8_t)(c->length - loop_start);
    if (span > 16) span = 16;
    loop_len = span;
}

static void toggle_mode(int t)
{
    if (cfg.mode[t] == MODE_SEQ) {
        /* Leaving the sequencer: silence its voice. */
        note_off(t);
        if (t == 3) {
            panel_triggers(0);
            trigger_off_ms = 0;
        }
        cfg.mode[t] = MODE_LFO;
        cc_last[t] = 0xff; /* re-announce the CC level */
    } else {
        /* Leaving the LFO: release every destination it drove. */
        dest_cleared(t, DEST_MOD);
        dest_cleared(t, DEST_PITCH);
        dest_cleared(t, DEST_GATE);
        lfo_out[t] = 0;
        cfg.mode[t] = MODE_SEQ;
    }
    cfg.selected = (uint8_t)t;
}

/* ------------------------------------------------------------------ */
/* Project manager (PROJECT-001..004)                                  */

static void project_report(int slot, const char *message)
{
    proj_msg = message;
    proj_msg_slot = slot;
    proj_msg_ms = g_ms;
}

static bool project_report_active(void)
{
    return proj_msg && g_ms - proj_msg_ms < 2500;
}

static void project_rescan(void)
{
    for (int n = 0; n < PROJECT_SLOTS; n++)
        slot_states[n] = (uint8_t)project_state(n);
}

/* Replace the workspace content with a project's. Both engines are silenced
 * once; the next loop iteration rewrites whatever the loaded lanes drive. */
static void apply_project(const project_t *p)
{
    for (int t = 0; t < 4; t++) {
        note_off(t);
        lfo_out[t] = 0;
        cc_last[t] = 0xff; /* re-announce the CC level */
    }
    outputs_all_off();
    panel_triggers(0);
    trigger_off_ms = 0;
    cfg.bpm = p->bpm;
    cfg.swing = p->swing;
    for (int t = 0; t < 4; t++) {
        cfg.mode[t] = p->mode[t];
        cfg.track[t] = p->track[t];
        cfg.lfo[t] = p->lfo[t];
    }
}

static void workspace_to_project(project_t *p)
{
    project_defaults(p);
    p->bpm = cfg.bpm;
    p->swing = cfg.swing;
    for (int t = 0; t < 4; t++) {
        p->mode[t] = cfg.mode[t];
        p->track[t] = cfg.track[t];
        p->lfo[t] = cfg.lfo[t];
    }
}

/* PROJECT-001: new project = declared defaults in the workspace; no slot is
 * touched and nothing is persisted until an explicit save. */
static void project_new(void)
{
    project_t p;
    project_defaults(&p);
    apply_project(&p);
    cur_slot = SLOT_NONE;
    project_report(-1, "NEW PROJECT");
}

static void project_pad(int slot)
{
    project_t p;
    if (panel_down(SW_RECORD)) { /* PROJECT-004: explicit delete */
        if (project_delete(slot)) {
            slot_states[slot] = SLOT_EMPTY;
            if (cur_slot == slot) cur_slot = SLOT_NONE;
            project_report(slot, "DELETED");
        } else {
            slot_states[slot] = (uint8_t)project_state(slot);
            project_report(slot, slot_states[slot] == SLOT_UNAVAILABLE
                           ? "NO SLOT" : "DELETE FAILED");
        }
        return;
    }
    if (panel_down(SW_SHIFT)) { /* PROJECT-003: explicit save */
        workspace_to_project(&p);
        if (project_save(slot, &p)) {
            slot_states[slot] = SLOT_VALID;
            cur_slot = (uint8_t)slot;
            project_report(slot, "SAVED");
        } else {
            slot_states[slot] = (uint8_t)project_state(slot);
            project_report(slot, slot_states[slot] == SLOT_UNAVAILABLE
                           ? "NO SLOT" : "SAVE FAILED");
        }
        return;
    }
    switch (project_load(slot, &p)) { /* PROJECT-002: load */
    case SLOT_VALID:
        apply_project(&p);
        cur_slot = (uint8_t)slot;
        project_report(slot, "LOADED");
        break;
    /* The workspace is untouched on every rejection below. */
    case SLOT_EMPTY: project_report(slot, "EMPTY"); break;
    case SLOT_INVALID: project_report(slot, "INVALID"); break;
    default: project_report(slot, "NO SLOT"); break;
    }
}

/* ------------------------------------------------------------------ */
/* Panel input                                                         */

static void select_track(int t, bool shift)
{
    if (shift) toggle_mode(t);
    else cfg.selected = (uint8_t)t;
}

static void clamp_track(track_cfg_t *c)
{
    if (c->pulses > c->length) c->pulses = c->length;
    if (c->rotate >= c->length) c->rotate = (uint8_t)(c->length - 1);
}

static void handle_pad(int track, int pad0, bool shift)
{
    cfg.selected = (uint8_t)track;
    if (cfg.mode[track] == MODE_SEQ) {
        track_cfg_t *c = &cfg.track[track];
        int step = edit_bar * 16 + pad0 + 1; /* step index in the selected bar */
        switch (edit_mode) {
        case EDIT_NONE:
            if (shift) {
                c->length = (uint8_t)step;
                clamp_track(c);
            } else if (step <= c->length) {
                c->pulses = (uint8_t)step;
            }
            break;
        case EDIT_PITCH:
            if (shift) return; /* undeclared SHIFT combination: no effect */
            c->note = (uint8_t)(36 + pad0);
            break;
        case EDIT_GATE:
            if (shift) return;
            c->gate_pct = (uint8_t)(5 + pad0 * 6);
            break;
        case EDIT_MOD:
            if (shift) return;
            c->mod = (uint8_t)pad0;
            break;
        case EDIT_CONTROL:
            if (shift) return;
            c->velocity = (uint8_t)(pad0 * 8 + 7);
            break;
        }
    } else {
        lfo_cfg_t *c = &cfg.lfo[track];
        switch (edit_mode) {
        case EDIT_NONE:
            if (shift) {
                if (pad0 < WAVE_COUNT) c->wave = (uint8_t)pad0;
            } else {
                c->div = (uint8_t)pad0;
            }
            break;
        case EDIT_PITCH:
            if (shift) return;
            c->phase = (uint8_t)pad0; /* pad n = n-1 sixteenths of a cycle */
            break;
        default:
            return; /* pads are documented inert in the other LFO modes */
        }
    }
}

/* The three main-loop workers stay out of line: fully inlined into
 * normal_main they hand GCC a register-pressure monster whose literal-spill
 * prologue becomes a straight-line block of over a hundred instructions, and
 * the browser emulator engine's translator faults on blocks that long (the
 * native engine does not). Out of line, every block stays well under the
 * lengths both engines are proven to run. */
__attribute__((noinline))
static void handle_events(void)
{
    panel_event_t ev;
    while (panel_event_next(&ev)) {
        if (!ev.down) continue;
        switch (ev.sw) {
        case SW_PLAY: transport_start(); continue;
        case SW_STOP: transport_stop(); continue;
        case SW_TAP: {
            uint32_t now = g_ms;
            uint32_t gap = now - tap_last_ms;
            if (gap >= 100 && gap <= 3000) {
                uint32_t bpm = 60000 / gap;
                cfg.bpm = (uint16_t)(bpm < 20 ? 20 : bpm > 300 ? 300 : bpm);
            }
            tap_last_ms = now;
            continue;
        }
        case SW_PROJECT:
            if (panel_down(SW_SHIFT)) {
                project_new();
            } else {
                project_mode = !project_mode;
                if (project_mode) project_rescan();
                else project_report(-1, 0);
            }
            continue;
        case SW_TRACK_A: if (!project_mode) select_track(0, panel_down(SW_SHIFT)); continue;
        case SW_TRACK_B: if (!project_mode) select_track(1, panel_down(SW_SHIFT)); continue;
        case SW_TRACK_C: if (!project_mode) select_track(2, panel_down(SW_SHIFT)); continue;
        case SW_TRACK_D: if (!project_mode) select_track(3, panel_down(SW_SHIFT)); continue;
        /* MODE-001: the same key leaves, another mode key switches; toggling
         * alone changes no pattern, workspace or settings data. */
        case SW_PITCH:   if (!project_mode) edit_mode = edit_mode == EDIT_PITCH   ? EDIT_NONE : EDIT_PITCH;   continue;
        case SW_GATE:    if (!project_mode) edit_mode = edit_mode == EDIT_GATE    ? EDIT_NONE : EDIT_GATE;    continue;
        case SW_MOD:     if (!project_mode) edit_mode = edit_mode == EDIT_MOD     ? EDIT_NONE : EDIT_MOD;     continue;
        case SW_CONTROL: if (!project_mode) edit_mode = edit_mode == EDIT_CONTROL ? EDIT_NONE : EDIT_CONTROL; continue;
        case SW_BAR1: if (!project_mode) edit_bar = 0; continue;
        case SW_BAR2: if (!project_mode) edit_bar = 1; continue;
        case SW_BAR3: if (!project_mode) edit_bar = 2; continue;
        case SW_BAR4: if (!project_mode) edit_bar = 3; continue;
        case SW_LOOP: if (!project_mode) loop_toggle(); continue;
        case SW_GLOBAL: if (!project_mode) settings_save(&cfg); continue;
        default: break;
        }
        for (int t = 0; t < 4; t++)
            for (int p = 0; p < 16; p++)
                if (pad_switch(t, p) == ev.sw) {
                    if (project_mode) project_pad(t * 16 + p);
                    else handle_pad(t, p, panel_down(SW_SHIFT));
                }
    }
    if (project_mode) {
        /* Pads are slot selectors here; drop encoder motion so it does not
         * accumulate and edit the workspace invisibly. */
        for (int e = 0; e < ENC_COUNT; e++) panel_encoder_take(e);
        loop_revalidate(); /* a loaded or new project may move pattern ends */
        return;
    }
    bool seq = cfg.mode[cfg.selected] == MODE_SEQ;
    track_cfg_t *tc = &cfg.track[cfg.selected];
    lfo_cfg_t *lc = &cfg.lfo[cfg.selected];
    int d;
    if ((d = panel_encoder_take(ENC_TEMPO))) {
        /* TEMPO-001: one BPM per detent, clamped at the declared 20..300
         * limits with no wraparound. */
        int bpm = cfg.bpm + d;
        cfg.bpm = (uint16_t)(bpm < 20 ? 20 : bpm > 300 ? 300 : bpm);
    }
    if ((d = panel_encoder_take(ENC_SWING))) {
        /* SWING-001: one percent per detent across the declared 50..75 %
         * range; 50 % is the unswung neutral setting (sequencer lanes). */
        int swing = cfg.swing + d;
        cfg.swing = (uint8_t)(swing < 50 ? 50 : swing > 75 ? 75 : swing);
    }
    int d1 = panel_encoder_take(ENC_PARAM1);
    int d2 = panel_encoder_take(ENC_PARAM2);
    int d3 = panel_encoder_take(ENC_PARAM3);
    int d4 = panel_encoder_take(ENC_PARAM4);
    if (!seq && edit_mode != EDIT_NONE) {
        /* MODE-001, LFO lane: each mode owns its documented parameters; the
         * drained detents of the unassigned encoders edit nothing. */
        switch (edit_mode) {
        case EDIT_PITCH: /* pitch-CV routing and phase offset only */
            if (d1) route_turn(DEST_PITCH, d1);
            if (d2) lc->phase = (uint8_t)((lc->phase + d2) & 15);
            break;
        case EDIT_GATE: /* gate (on D: cycle trigger) routing only */
            if (d1) route_turn(DEST_GATE, d1);
            break;
        case EDIT_MOD: /* mod-CV routing and depth only */
            if (d1) route_turn(DEST_MOD, d1);
            if (d2) {
                int depth = lc->depth + d2;
                lc->depth = (uint8_t)(depth < 0 ? 0 : depth > 100 ? 100 : depth);
            }
            break;
        default: /* EDIT_CONTROL: the MIDI CC number only */
            if (d1) {
                int cc = lc->cc + d1;
                lc->cc = (uint8_t)(cc < 0 ? 0 : cc > 127 ? 127 : cc);
            }
            break;
        }
        loop_revalidate(); /* an engine toggle may have stranded the loop */
        return;
    }
    if (d1) {
        if (seq) {
            int pulses = tc->pulses + d1;
            tc->pulses = (uint8_t)(pulses < 0 ? 0 : pulses > tc->length ? tc->length : pulses);
        } else {
            int wave = lc->wave + d1;
            lc->wave = (uint8_t)(wave < 0 ? 0 : wave >= WAVE_COUNT ? WAVE_COUNT - 1 : wave);
        }
    }
    if (d2) {
        if (seq) {
            int length = tc->length + d2;
            tc->length = (uint8_t)(length < 1 ? 1 : length > 64 ? 64 : length);
            clamp_track(tc);
        } else {
            int div = lc->div + d2;
            lc->div = (uint8_t)(div < 0 ? 0 : div >= DIV_COUNT ? DIV_COUNT - 1 : div);
        }
    }
    if (d3) {
        if (seq) {
            int rotate = tc->rotate + d3;
            while (rotate < 0) rotate += tc->length;
            tc->rotate = (uint8_t)(rotate % tc->length);
        } else {
            int depth = lc->depth + d3;
            lc->depth = (uint8_t)(depth < 0 ? 0 : depth > 100 ? 100 : depth);
        }
    }
    if (d4) {
        if (seq) {
            int note = tc->note + d4;
            tc->note = (uint8_t)(note < 0 ? 0 : note > 127 ? 127 : note);
        } else {
            lc->phase = (uint8_t)((lc->phase + d4) & 15);
        }
    }
    loop_revalidate();
}

/* ------------------------------------------------------------------ */
/* LEDs and display                                                    */

__attribute__((noinline))
static void update_leds(void)
{
    uint32_t rows[8];
    if (project_mode) {
        /* Pad LED n of bank t lights when slot t*16+n holds a project; the
         * current slot blinks. */
        bool blink = (g_ms >> 8) & 1;
        for (int t = 0; t < 4; t++) {
            uint32_t cols = 0;
            for (int p = 0; p < 16; p++) {
                int slot = t * 16 + p;
                bool on = slot_states[slot] == SLOT_VALID;
                if (slot == cur_slot) on = blink;
                if (on) cols |= 2u << p;
            }
            rows[t * 2] = cols;
            rows[t * 2 + 1] = 0;
        }
        rows[SW_ROW(LED_PROJECT)] |= 1u << SW_BIT(LED_PROJECT);
        panel_led_columns(rows);
        bool beat = pos_tick % 24 < 2;
        panel_transport_leds(beat, !playing, panel_down(SW_RECORD), playing);
        return;
    }
    uint32_t base = (uint32_t)edit_bar * 16; /* pads window the selected bar */
    for (int t = 0; t < 4; t++) {
        uint32_t cols;
        if (cfg.mode[t] == MODE_SEQ) {
            const track_cfg_t *c = &cfg.track[t];
            cols = (uint32_t)((track_pattern(t) >> base) & 0xffffu) << 1;
            if (playing) {
                uint32_t pos = (global_step ? global_step - 1 : 0) % c->length;
                if (pos >= base && pos < base + 16)
                    cols ^= 2u << (pos - base); /* playhead inverts its column */
            }
        } else {
            /* The LFO's row of pads is a live level bar. */
            int lit = (lfo_out[t] >> 12) + 1; /* 1..16 pads */
            cols = ((1u << lit) - 1) << 1;
        }
        if (t == cfg.selected) cols |= 1; /* track select LED, column 0 */
        rows[t * 2] = cols;
        rows[t * 2 + 1] = 0;
    }
    /* Active edit mode, selected bar and loop status on their key LEDs. */
    static const uint8_t MODE_LEDS[5] = {0xff, LED_PITCH, LED_GATE, LED_MOD, LED_CONTROL};
    static const uint8_t BAR_LEDS[4] = {LED_BAR1, LED_BAR2, LED_BAR3, LED_BAR4};
    if (MODE_LEDS[edit_mode] != 0xff)
        rows[SW_ROW(MODE_LEDS[edit_mode])] |= 1u << SW_BIT(MODE_LEDS[edit_mode]);
    rows[SW_ROW(BAR_LEDS[edit_bar])] |= 1u << SW_BIT(BAR_LEDS[edit_bar]);
    if (loop_on) rows[SW_ROW(LED_LOOP)] |= 1u << SW_BIT(LED_LOOP);
    panel_led_columns(rows);
    bool beat = pos_tick % 24 < 2;
    panel_transport_leds(beat, !playing, ext_active(), playing);
}

/* Boot splash: drawn in place of the UI for the first moments after power-on
 * (the name, then the site on a new line) while the engines, LEDs and panel
 * input keep running underneath. */
#define SPLASH_NAME_MS 1200u
#define SPLASH_END_MS 2400u
static bool splash_done;

static bool splash_active(void)
{
    if (!splash_done && g_ms >= SPLASH_END_MS) splash_done = true;
    return !splash_done;
}

static void draw_splash(void)
{
    fb_clear();
    fb_text2x(68, 10, "ALEPH VOID", 15);
    fb_text2x(74, 28, "CUSTOM FW", 12);
    if (g_ms >= SPLASH_NAME_MS)
        fb_text2x(20, 48, "SQ64.ALEPHVOID.COM", 9);
    display_flush();
}

static void draw_wave_preview(int x, int y, int w, int h, int lfo)
{
    for (int i = 0; i < w; i++) {
        uint16_t phase = (uint16_t)((i << 16) / w + cfg.lfo[lfo].phase * 4096u);
        int v = wave_value(lfo, phase) * (h - 1) / 65535;
        fb_pixel(x + i, y + h - 1 - v, 9);
    }
    int cursor = lfo_phase_raw[lfo] * w / 65536;
    fb_fill(x + cursor, y, 1, h, 15);
}

static void draw_seq_lane(int t, int y)
{
    const track_cfg_t *tc = &cfg.track[t];
    uint64_t bits = track_pattern(t);
    int base = edit_bar * 16; /* the lane windows the selected bar */
    for (int k = 0; k < 16; k++) {
        int step = base + k;
        int x = 24 + k * 9;
        if (step >= tc->length) continue;
        if ((bits >> step) & 1) fb_fill(x, y, 7, 6, t == cfg.selected ? 13 : 7);
        else fb_fill(x, y + 2, 7, 2, 3);
        if (playing && (int)((global_step ? global_step - 1 : 0) % tc->length) == step)
            fb_fill(x + 2, y + 6, 3, 1, 15);
    }
}

static void draw_lfo_lane(int t, int y)
{
    const lfo_cfg_t *lc = &cfg.lfo[t];
    uint8_t bright = t == cfg.selected ? 15 : 6;
    fb_text(24, y, WAVE_NAMES[lc->wave], bright);
    fb_text(54, y, DIV_NAMES[lc->div], bright);
    fb_text(90, y, "CC", 5);
    fb_number(104, y, lc->cc, bright);
    int width = lfo_out[t] * 90 / 65535;
    fb_fill(124, y + 1, 90, 1, 3);
    if (width) fb_fill(124, y, width, 4, t == cfg.selected ? 13 : 7);
}

static void draw_report(int x, int y)
{
    if (proj_msg_slot >= 0) {
        fb_text(x, y, (const char[]){(char)('A' + proj_msg_slot / 16), 0}, 15);
        fb_number(x + 6, y, proj_msg_slot % 16 + 1, 15);
        fb_text(x + 24, y, proj_msg, 12);
    } else {
        fb_text(x, y, proj_msg, 12);
    }
}

static void draw_projects(void)
{
    fb_clear();
    fb_text(2, 1, "PROJECTS", 15);
    if (project_report_active()) draw_report(120, 1);
    fb_text(2, 12, "PAD LOAD", 8);
    fb_text(68, 12, "SHIFT+PAD SAVE", 8);
    fb_text(166, 12, "REC+PAD DELETE", 8);

    /* Slot catalog: one lane per bank, filled = saved project, baseline =
     * empty, half fill = invalid, nothing = beyond the fitted SPI part; the
     * current slot carries a bright top edge. */
    for (int t = 0; t < 4; t++) {
        int y = 24 + t * 8;
        fb_text(2, y, (const char[]){(char)('A' + t), 0}, 6);
        for (int p = 0; p < 16; p++) {
            int slot = t * 16 + p;
            int x = 12 + p * 15;
            switch (slot_states[slot]) {
            case SLOT_VALID: fb_fill(x, y, 12, 6, 13); break;
            case SLOT_EMPTY: fb_fill(x, y + 4, 12, 2, 3); break;
            case SLOT_INVALID: fb_fill(x, y + 1, 12, 4, 7); break;
            default: break;
            }
            if (slot == cur_slot) fb_fill(x, y, 12, 1, 15);
        }
    }
    fb_text(2, 57, "SHIFT+PROJ NEW  PROJ EXIT", 5);
    display_flush();
}

__attribute__((noinline))
static void draw_ui(void)
{
    if (splash_active()) {
        draw_splash();
        return;
    }
    if (project_mode) {
        draw_projects();
        return;
    }
    bool seq = cfg.mode[cfg.selected] == MODE_SEQ;
    uint16_t bpm_shown = cfg.bpm;
    if (ext_active()) bpm_shown = (uint16_t)(2500 / ext_gap_ms);
    fb_clear();
    fb_text(2, 1, "AV-AGG", 15);
    fb_number(44, 1, bpm_shown, 12);
    fb_text(68, 1, "BPM", 8);
    fb_text(92, 1, ext_active() ? "EXT" : "INT", ext_active() ? 15 : 8);
    fb_text(118, 1, "SW", 8);
    fb_number(134, 1, cfg.swing, 12);
    fb_text(148, 1, "%", 8);
    fb_text(216, 1, playing ? "PLAY" : "STOP", playing ? 15 : 6);

    fb_text(2, 12, seq ? "SEQ" : "LFO", 8);
    fb_text(24, 12, (const char[]){(char)('A' + cfg.selected), 0}, 15);
    if (seq && edit_mode != EDIT_NONE) {
        /* MODE-001: the active mode and its one pad-edited parameter. */
        const track_cfg_t *c = &cfg.track[cfg.selected];
        switch (edit_mode) {
        case EDIT_PITCH:
            fb_text(40, 12, "PITCH", 8);
            fb_text(78, 12, NOTE_NAMES[c->note % 12], 15);
            fb_number(92, 12, c->note / 12 - 1, 15);
            fb_number(112, 12, c->note, 12);
            break;
        case EDIT_GATE:
            fb_text(40, 12, "GATE", 8);
            fb_number(72, 12, c->gate_pct, 15);
            fb_text(90, 12, "%", 8);
            break;
        case EDIT_MOD:
            fb_text(40, 12, "MOD", 8);
            fb_number(66, 12, c->mod, 15);
            break;
        default:
            fb_text(40, 12, "VEL", 8);
            fb_number(66, 12, c->velocity, 15);
            break;
        }
    } else if (!seq && edit_mode != EDIT_NONE) {
        /* MODE-001: the routing this mode edits and its companion value. */
        const lfo_cfg_t *c = &cfg.lfo[cfg.selected];
        bool d_lane = cfg.selected == 3;
        switch (edit_mode) {
        case EDIT_PITCH:
            fb_text(40, 12, "PITCH", 8);
            if (d_lane) fb_text(78, 12, "NO JACK ON D", 12);
            else fb_text(78, 12, c->dest & DEST_PITCH ? "ON" : "OFF", 15);
            fb_text(104, 12, "PHS", 8);
            fb_number(128, 12, c->phase, 15);
            break;
        case EDIT_GATE:
            fb_text(40, 12, d_lane ? "TRIG" : "GATE", 8);
            fb_text(72, 12, c->dest & DEST_GATE ? "ON" : "OFF", 15);
            break;
        case EDIT_MOD:
            fb_text(40, 12, "MOD", 8);
            if (d_lane) fb_text(66, 12, "NO JACK ON D", 12);
            else fb_text(66, 12, c->dest & DEST_MOD ? "ON" : "OFF", 15);
            fb_text(96, 12, "DEP", 8);
            fb_number(120, 12, c->depth, 15);
            break;
        default:
            fb_text(40, 12, "CC", 8);
            fb_number(56, 12, c->cc, 15);
            break;
        }
    } else if (seq) {
        const track_cfg_t *c = &cfg.track[cfg.selected];
        fb_text(40, 12, "PULSE", 8);
        fb_number(76, 12, c->pulses, 15);
        fb_text(96, 12, "LEN", 8);
        fb_number(120, 12, c->length, 15);
        fb_text(140, 12, "ROT", 8);
        fb_number(164, 12, c->rotate, 15);
        fb_text(184, 12, "NOTE", 8);
        fb_text(214, 12, NOTE_NAMES[c->note % 12], 15);
        fb_number(226, 12, c->note / 12 - 1, 15);
    } else {
        const lfo_cfg_t *c = &cfg.lfo[cfg.selected];
        fb_text(40, 12, WAVE_NAMES[c->wave], 15);
        fb_text(76, 12, DIV_NAMES[c->div], 15);
        fb_text(114, 12, "DEP", 8);
        fb_number(138, 12, c->depth, 15);
        fb_text(160, 12, "PHS", 8);
        fb_number(184, 12, c->phase, 15);
        fb_text(200, 12, "CC", 8);
        fb_number(216, 12, c->cc, 15);
        /* Destination flags: Mod / Pitch / Gate (trigger for LFO D). */
        fb_text(238, 12, "M", (c->dest & DEST_MOD) && cfg.selected < 3 ? 15 : 3);
        fb_text(244, 12, "P", (c->dest & DEST_PITCH) && cfg.selected < 3 ? 15 : 3);
        fb_text(250, 12, cfg.selected == 3 ? "T" : "G", c->dest & DEST_GATE ? 15 : 3);
    }

    /* The lane area shows the active program only: the selected track's
     * engine. A track running the other engine still runs (and keeps its pad
     * LED feedback) but is listed here with just a dim tag, so the screen is
     * never a mix of both engines' idioms. */
    for (int t = 0; t < 4; t++) {
        int y = 24 + t * 8;
        uint8_t bright = t == cfg.selected ? 15 : 6;
        fb_text(2, y, (const char[]){(char)('A' + t), 0}, bright);
        fb_text(10, y, cfg.mode[t] == MODE_SEQ ? "S" : "L", 5);
        if ((cfg.mode[t] == MODE_SEQ) != seq) {
            fb_text(24, y, cfg.mode[t] == MODE_SEQ ? "SEQ" : "LFO", 3);
            continue;
        }
        if (seq) draw_seq_lane(t, y);
        else draw_lfo_lane(t, y);
    }
    if (!seq) draw_wave_preview(222, 26, 32, 28, cfg.selected);
    /* Status footer: the active edit mode, bar and loop are observable. */
    static const char *const MODE_NAMES[5] = {"----", "PITCH", "GATE", "MOD", "CTRL"};
    fb_text(2, 57, MODE_NAMES[edit_mode], edit_mode ? 12 : 5);
    fb_text(40, 57, "BAR", 5);
    fb_number(64, 57, edit_bar + 1, 5);
    fb_text(78, 57, "LOOP", loop_on ? 12 : 3);
    if (project_report_active()) draw_report(110, 57);
    else fb_text(116, 57, "GLB SAVES SHIFT+TRK ENG", 5);
    display_flush();
}

/* ------------------------------------------------------------------ */
/* Power and the normal-mode main loop                                 */

static void power_off(void)
{
    if (playing) transport_stop();
    outputs_all_off();
    panel_triggers(0);
    settings_save(&cfg);
    fb_clear();
    fb_text(2, 28, "POWERED OFF", 12);
    display_flush();
    for (int i = 0; i < 10000 && (REG32(DISPLAY_DMA_STREAM) & DMA_CR_EN); i++) { }
    display_pump();
    for (int i = 0; i < 10000 && (REG32(DISPLAY_DMA_STREAM) & DMA_CR_EN); i++) { }
    GPIO_BSRR(GPIOB) = 1u << (16 + PIN_POWER_HOLD); /* release the hold */
    for (;;) { }
}

static void normal_main(void)
{
    settings_load(&cfg);
    display_init();
    outputs_init();
    panel_init();
    /* Long redraws drain the panel scan from inside the framebuffer
     * primitives, so detents and edges landing mid-draw are not lost. */
    display_set_breather(panel_poll);
    midi_init(0, realtime_in);

    /* 1 ms tick: 216 MHz / (215+1) / (999+1). */
    REG32(TICK_TIM + 0x28) = 215;
    REG32(TICK_TIM + 0x2c) = 999;
    REG32(TICK_TIM + 0x0c) = 1;
    NVIC_ISER(TICK_IRQ / 32) = 1u << (TICK_IRQ % 32);
    REG32(TICK_TIM) = 1;

    bool power_seen_released = false;
    uint32_t last_draw_ms = 0;
    for (;;) {
        panel_poll();
        handle_events();
        midi_poll();
        if (rt_start) { rt_start = false; transport_start(); }
        if (rt_stop) { rt_stop = false; transport_stop(); }
        uint32_t now = g_ms;
        while ((int32_t)(now - last_ms) > 0) {
            last_ms++;
            clock_advance_ms();
        }
        sequencer_tick();
        lfo_update();
        update_leds();
        if ((int32_t)(g_ms - last_draw_ms) >= 40) {
            last_draw_ms = g_ms;
            draw_ui(); /* LFO lanes animate: redraw on a fixed cadence */
        }
        display_pump();
        bool pressed = !(GPIO_IDR(GPIOC) & (1u << PIN_POWER_BTN));
        if (power_seen_released && pressed)
            power_off();
        power_seen_released = power_seen_released || !pressed;
    }
}

void main_entry(void)
{
    /* Output pins: PB0 (panel latch), PB7 (power hold), PB9 (flash CS),
     * PB12/PB15/PC9 (gates). Everything idles high; gates are inverted by
     * their drivers, so high means inactive. */
    GPIO_BSRR(GPIOB) = (1u << PIN_PANEL_LATCH) | (1u << PIN_POWER_HOLD)
                     | (1u << PIN_FLASH_CS) | (1u << PIN_GATE_A) | (1u << PIN_GATE_B);
    GPIO_BSRR(GPIOC) = 1u << PIN_GATE_C;
    GPIO_MODER(GPIOB) |= (1u << (2 * PIN_PANEL_LATCH)) | (1u << (2 * PIN_POWER_HOLD))
                       | (1u << (2 * PIN_FLASH_CS)) | (1u << (2 * PIN_GATE_A))
                       | (1u << (2 * PIN_GATE_B));
    GPIO_MODER(GPIOC) |= 1u << (2 * PIN_GATE_C);

    /* Startup chords (BOOT-004): GLOBAL alone selects version mode,
     * GLOBAL+PATTERN selects update mode. Anything else boots normally.
     * Version mode returns when PLAY is pressed, continuing the normal
     * boot sequence below. */
    uint32_t rows[8];
    panel_startup_scan(rows);
    bool global_held = (rows[SW_ROW(SW_GLOBAL)] >> SW_BIT(SW_GLOBAL)) & 1u;
    bool pattern_held = (rows[SW_ROW(SW_PATTERN)] >> SW_BIT(SW_PATTERN)) & 1u;
    uint32_t others = 0;
    for (int r = 0; r < 8; r++) {
        uint32_t buttons = rows[r] & (r == 0 ? 0x07ff0003u : r == 1 ? 0x07fe0003u : 0x07fe0000u);
        if (r == SW_ROW(SW_GLOBAL)) buttons &= ~(1u << SW_BIT(SW_GLOBAL));
        if (r == SW_ROW(SW_PATTERN)) buttons &= ~(1u << SW_BIT(SW_PATTERN));
        others |= buttons;
    }

    if (global_held && pattern_held && !others)
        updater_main();
    if (global_held && !pattern_held && !others)
        version_main();
    normal_main();
}
