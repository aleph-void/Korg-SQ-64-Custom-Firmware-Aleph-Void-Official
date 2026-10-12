/* Aleph Void aggregate firmware for the SQ-64: boot mode selection and
 * normal mode.
 *
 * Each of the four tracks runs one of two engines, toggled per track with
 * SHIFT + its track button:
 *
 *  - MODE_SEQ: a Euclidean rhythm E(pulses, length) with rotation. Tracks
 *    A/B/C drive their gate jack, pitch CV, mod CV and MIDI notes on
 *    channels 1..3; track D drives trigger jack 1 and MIDI channel 10.
 *  - MODE_LFO: a tempo-synced LFO with waveform, rate division, depth,
 *    phase offset and a MIDI CC streamed on channels 1..4. LFOs A/B/C can
 *    drive their mod CV, pitch CV and gate jacks; LFO D pulses trigger
 *    jack 1 once per cycle.
 *
 * Both engines hang off one shared 24 PPQN transport position, advanced by
 * the internal tempo clock or following incoming MIDI clock automatically,
 * so sequencer steps and LFO phases stay locked to the same beat. Pads and
 * the param encoders edit whichever engine the selected track runs, GLOBAL
 * commits settings to SPI flash, and the power button saves and shuts down
 * cleanly.
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

static uint16_t euclid_pattern(const track_cfg_t *t)
{
    uint16_t bits = 0;
    for (int i = 0; i < t->length; i++) {
        int j = (i + t->length - t->rotate) % t->length;
        if ((j * t->pulses) % t->length < t->pulses)
            bits |= (uint16_t)(1u << i);
    }
    return bits;
}

/* The pattern is read every main-loop iteration (LEDs) and per lane per
 * redraw, but its software modulos are expensive enough to let a redraw
 * outrun the panel scan buffer; recompute only when the track changes. */
static uint16_t track_pattern(int t)
{
    static uint8_t key[4][3];
    static uint16_t cache[4];
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
    uint8_t msg[3] = {(uint8_t)(status | channel), note, status == 0x90 ? 100 : 0};
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
            cv_mod(t, 220);
        } else {
            panel_triggers(1);
            trigger_off_ms = now + 20;
        }
    }
    global_step++;
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

static void toggle_dest(uint8_t bit)
{
    if (cfg.mode[cfg.selected] != MODE_LFO) return;
    lfo_cfg_t *c = &cfg.lfo[cfg.selected];
    if (cfg.selected == 3 && bit != DEST_GATE) return; /* D is MIDI + trigger */
    c->dest ^= bit;
    if (!(c->dest & bit)) dest_cleared(cfg.selected, bit);
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
        if (shift) {
            c->length = (uint8_t)(pad0 + 1);
            clamp_track(c);
        } else if (pad0 + 1 <= c->length) {
            c->pulses = (uint8_t)(pad0 + 1);
        }
    } else {
        if (shift) {
            if (pad0 < WAVE_COUNT) cfg.lfo[track].wave = (uint8_t)pad0;
        } else {
            cfg.lfo[track].div = (uint8_t)pad0;
        }
    }
}

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
        case SW_TRACK_A: select_track(0, panel_down(SW_SHIFT)); continue;
        case SW_TRACK_B: select_track(1, panel_down(SW_SHIFT)); continue;
        case SW_TRACK_C: select_track(2, panel_down(SW_SHIFT)); continue;
        case SW_TRACK_D: select_track(3, panel_down(SW_SHIFT)); continue;
        case SW_MOD: toggle_dest(DEST_MOD); continue;
        case SW_PITCH: toggle_dest(DEST_PITCH); continue;
        case SW_GATE: toggle_dest(DEST_GATE); continue;
        case SW_GLOBAL: settings_save(&cfg); continue;
        default: break;
        }
        for (int t = 0; t < 4; t++)
            for (int p = 0; p < 16; p++)
                if (pad_switch(t, p) == ev.sw)
                    handle_pad(t, p, panel_down(SW_SHIFT));
    }
    bool seq = cfg.mode[cfg.selected] == MODE_SEQ;
    track_cfg_t *tc = &cfg.track[cfg.selected];
    lfo_cfg_t *lc = &cfg.lfo[cfg.selected];
    int d;
    if ((d = panel_encoder_take(ENC_TEMPO))) {
        int bpm = cfg.bpm + d;
        cfg.bpm = (uint16_t)(bpm < 20 ? 20 : bpm > 300 ? 300 : bpm);
    }
    if ((d = panel_encoder_take(ENC_SWING))) {
        if (seq) {
            int swing = cfg.swing + d;
            cfg.swing = (uint8_t)(swing < 50 ? 50 : swing > 75 ? 75 : swing);
        } else {
            int cc = lc->cc + d;
            lc->cc = (uint8_t)(cc < 0 ? 0 : cc > 127 ? 127 : cc);
        }
    }
    if ((d = panel_encoder_take(ENC_PARAM1))) {
        if (seq) {
            int pulses = tc->pulses + d;
            tc->pulses = (uint8_t)(pulses < 0 ? 0 : pulses > tc->length ? tc->length : pulses);
        } else {
            int wave = lc->wave + d;
            lc->wave = (uint8_t)(wave < 0 ? 0 : wave >= WAVE_COUNT ? WAVE_COUNT - 1 : wave);
        }
    }
    if ((d = panel_encoder_take(ENC_PARAM2))) {
        if (seq) {
            int length = tc->length + d;
            tc->length = (uint8_t)(length < 1 ? 1 : length > 16 ? 16 : length);
            clamp_track(tc);
        } else {
            int div = lc->div + d;
            lc->div = (uint8_t)(div < 0 ? 0 : div >= DIV_COUNT ? DIV_COUNT - 1 : div);
        }
    }
    if ((d = panel_encoder_take(ENC_PARAM3))) {
        if (seq) {
            int rotate = tc->rotate + d;
            while (rotate < 0) rotate += tc->length;
            tc->rotate = (uint8_t)(rotate % tc->length);
        } else {
            int depth = lc->depth + d;
            lc->depth = (uint8_t)(depth < 0 ? 0 : depth > 100 ? 100 : depth);
        }
    }
    if ((d = panel_encoder_take(ENC_PARAM4))) {
        if (seq) {
            int note = tc->note + d;
            tc->note = (uint8_t)(note < 0 ? 0 : note > 127 ? 127 : note);
        } else {
            lc->phase = (uint8_t)((lc->phase + d) & 15);
        }
    }
}

/* ------------------------------------------------------------------ */
/* LEDs and display                                                    */

static void update_leds(void)
{
    uint32_t rows[8];
    for (int t = 0; t < 4; t++) {
        uint32_t cols;
        if (cfg.mode[t] == MODE_SEQ) {
            const track_cfg_t *c = &cfg.track[t];
            cols = (uint32_t)track_pattern(t) << 1; /* pads on columns 1..16 */
            if (playing) {
                uint32_t pos = (global_step ? global_step - 1 : 0) % c->length;
                cols ^= 2u << pos; /* playhead inverts its column */
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
    /* Destination LEDs for an edited LFO (row 0 columns 18..20). */
    if (cfg.mode[cfg.selected] == MODE_LFO) {
        const lfo_cfg_t *c = &cfg.lfo[cfg.selected];
        if (c->dest & DEST_GATE) rows[0] |= 1u << 18;
        if (c->dest & DEST_PITCH) rows[0] |= 1u << 19;
        if (c->dest & DEST_MOD) rows[0] |= 1u << 20;
    }
    panel_led_columns(rows);
    bool beat = pos_tick % 24 < 2;
    panel_transport_leds(beat, !playing, ext_active(), playing);
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
    uint16_t bits = track_pattern(t);
    for (int k = 0; k < 16; k++) {
        int x = 24 + k * 9;
        if (k >= tc->length) continue;
        if ((bits >> k) & 1) fb_fill(x, y, 7, 6, t == cfg.selected ? 13 : 7);
        else fb_fill(x, y + 2, 7, 2, 3);
        if (playing && (int)((global_step ? global_step - 1 : 0) % tc->length) == k)
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

static void draw_ui(void)
{
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
    if (seq) {
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

    /* One lane per track, drawn in its engine's own idiom. */
    for (int t = 0; t < 4; t++) {
        int y = 24 + t * 8;
        uint8_t bright = t == cfg.selected ? 15 : 6;
        fb_text(2, y, (const char[]){(char)('A' + t), 0}, bright);
        fb_text(10, y, cfg.mode[t] == MODE_SEQ ? "S" : "L", 5);
        if (cfg.mode[t] == MODE_SEQ) draw_seq_lane(t, y);
        else draw_lfo_lane(t, y);
    }
    if (!seq) draw_wave_preview(222, 26, 32, 28, cfg.selected);
    fb_text(2, 57, "GLOBAL SAVES  SHIFT+TRK MODE", 5);
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
     * GLOBAL+PATTERN selects update mode. Anything else boots normally. */
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
