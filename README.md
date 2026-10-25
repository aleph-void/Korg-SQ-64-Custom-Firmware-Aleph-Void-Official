# AV aggregate — original SQ-64 firmware

[![Sponsor Aleph Void](https://img.shields.io/badge/Sponsor-Aleph%20Void-EA4AAA?logo=githubsponsors&logoColor=white)](https://github.com/sponsors/aleph-void)

A from-scratch firmware for the Korg SQ-64 hardware sequencer that aggregates
both Aleph Void firmware feature sets into one image: each of the four tracks
runs either a **Euclidean rhythm sequencer** lane (as in the sibling
[euclidean](../euclidean) firmware) or a **tempo-synced LFO** (as in the
sibling [lfos](../lfos) firmware), toggled per track at any time with
SHIFT + its track button. It is written entirely from the interface facts
published by the independent [SQ-64 firmware specification](../../spec) and
exercised against the independent [SQ-64 emulator](../../emulator). **It
contains no Korg source code, no decompiled vendor functions, no vendor
assets and no bytes copied from stock firmware.**

> **Hardware warning.** The specification's board contract is explicitly
> incomplete: exact MCU part, wiring, electrical limits and protected-storage
> ownership remain unverified release blockers. This image passes the spec's
> `image` profile and runs in the emulator; that is **not** a certification
> that it is safe on a physical SQ-64. Flashing real hardware is at your own
> risk, and power loss while internal flash is being programmed is not
> guaranteed to be recoverable (spec VUP-008). If you accept that, the
> sibling [euclidean firmware's HARDWARE.md](../euclidean/HARDWARE.md)
> documents the identical install / operate / restore procedure (all three
> images use the same container, updater and storage map).

## One transport, two engines

Everything hangs off a single shared 24 PPQN transport position, so sequencer
steps and LFO phases stay locked to the same beat and to each other. The
position advances from the internal tempo clock (20–300 BPM, tap tempo, MIDI
clock out while playing) and **follows incoming MIDI clock automatically**
when one is present — the header flips from INT to EXT with the measured BPM,
every lane locks to the external ticks (the Euclidean engine gains external
sync it did not have standalone), and a second after the ticks stop it falls
back to the internal clock. PLAY resyncs everything from zero and starts the
sequencer lanes; STOP silences the sequencer lanes while the LFOs keep
free-running, phase-locked. MIDI start/stop in are honored.

By default tracks A/B boot as sequencer lanes and C/D as LFOs; any mix is a
SHIFT+track press away, and the mix persists with the rest of the settings.

## Feature sets, as in the originals

**Sequencer lane** (per track in SEQ mode): Euclidean pattern E(pulses,
length) with rotation, 1–64 steps in four bars of 16, polymetric against the
other lanes; per-track note, gate length, mod CV level and velocity; swing
50–75 % across all sequencer lanes. Lanes A/B/C drive their gate jack, pitch
CV, mod CV and MIDI notes on channels 1–3; lane D drives trigger jack 1 and
MIDI channel 10. Pad LEDs show the selected bar of the pattern with a moving
playhead (outside the [edit modes](#edit-modes), which repaint the rows with
the edited parameter).

**LFO lane** (per track in LFO mode): six waveforms — sine, triangle, rise,
fall, square, sample & hold — sixteen synced rate divisions from 8 bars to
1/32 including dotted and triplet values, per-LFO depth, phase offset and
MIDI CC number streamed on channels 1–4 (USB and DIN, rate-limited, sent only
on change). LFOs A/B/C can drive their mod CV, pitch CV and gate jacks
(gate = square-thresholded LFO), routed inside the PITCH / GATE / MOD edit
modes; LFO D pulses trigger jack 1 once per cycle. Pad LEDs are a live level
bar (in the PITCH edit mode, the phase-offset pad) and the display shows a
waveform preview with a phase cursor.

**The spec's panel-key requirements** (TRACK/MODE/LOOP/BAR/TAP/TEMPO/
SWING-001): A–D select the edited track; the PITCH, GATE, MOD and CONTROL
keys each toggle an edit mode scoping the pads and param encoders to that
mode's documented parameters on the selected track — on a sequencer lane one
pad-edited parameter (note, gate %, mod CV level, velocity), on an LFO lane
that destination's routing on the Param 1 encoder plus its companion
parameter (phase offset, depth, or the MIDI CC number in the CONTROL mode).
Keys 1–4 select which 16-step bar the pads, pad LEDs and display lanes
address; LOOP loops step playback over the selected bar of the selected
sequencer lane's pattern (refused on an LFO lane or when the bar lies beyond
the pattern, clipped or cleared if a later edit moves the pattern end), and
clearing it resumes the free-running step counter. The tempo encoder moves
one BPM per detent across 20–300, the swing encoder one percent across
50–75 (50 = unswung), both clamped without wraparound. Track selection, mode
entry/exit, bar keys, loop toggles and taps alone never change stored data.

Settings persist to SPI flash (GLOBAL commits; the power button saves and
shuts down cleanly through the PB7 power hold), and the PROJECT manager
drives the full project lifecycle over the 64 documented saved-project slots
— new, load, save and delete, with empty and invalid slots reported and the
workspace left unchanged on rejection (spec PROJECT-001..004) — see
[Projects](#projects). Boot chords per the spec:
hold **GLOBAL** at power-on for version mode, **GLOBAL+PATTERN** for update
mode. In version mode the PLAY indicator is lit, and pressing **PLAY**
continues the normal boot sequence to the ready state (BOOT-004). Update
mode carries the same complete VUP self-updater as the sibling firmwares
(protocol 1, 1024-byte packets, full validation before a RAM-resident
installer programs anything).

A boot splash is declared (spec SPLASH-001): entering normal mode draws the
Aleph Void name for the first 2.4 s — through the ordinary framebuffer
primitives and the same DMA display path as every other frame — while the
engines, pad LEDs and panel input already run underneath, so the declared
ready state is reached and the startup-mode chords (sampled before normal
mode starts) are honored regardless of the splash.

## Controls (normal mode)

| Control | Action |
| --- | --- |
| PLAY / STOP | Start from step 0 (or the loop start) with all LFO phases resynced / stop (sequencer notes off, LFOs keep running) |
| TAP | Tap tempo |
| A B C D | Select the edited track |
| SHIFT + A B C D | Toggle that track between sequencer lane and LFO |
| PITCH / GATE / MOD / CONTROL | Toggle that [edit mode](#edit-modes) (same key again leaves it) |
| 1 2 3 4 | Select the bar the pads address (pad *n* = step (bar−1)×16+*n*) |
| LOOP | Toggle a loop over the selected bar of the selected sequencer lane's pattern |
| Pad *n* (row of track *t*) | SEQ: set pulses to the selected step · LFO: set rate division *n* (1=8 bars … 16=1/32) — or the active edit mode's parameter |
| SHIFT + pad *n* | Outside the edit modes only — SEQ: set length to the selected step · LFO: set waveform (1=sine 2=tri 3=rise 4=fall 5=square 6=S+H) |
| Tempo encoder | BPM 20–300, one per detent, clamped |
| Swing encoder | Swing 50–75 %, one per detent, clamped; 50 = unswung (sequencer lanes) |
| Param 1–4 encoders | SEQ: pulses, length, rotation, note · LFO: waveform, rate, depth, phase — or the active edit mode's parameters |
| GLOBAL | Commit settings to SPI flash |
| PROJECT | Open / close the project manager (pads become the 64 slots) |
| Param 2 / Param 3 encoders (project manager) | Edit the [project name](#projects): character at the cursor / cursor position |
| SHIFT + PROJECT | New project: reset the workspace to the declared defaults |
| Power button | Save and shut down |

Declared SHIFT combinations (spec SHIFT-001): SHIFT+track (engine toggle),
SHIFT+pad outside the edit modes (length on a sequencer lane, waveform on an
LFO lane), SHIFT+pad in the project manager (save) and SHIFT+PROJECT (new
project). SHIFT alone, and SHIFT with any other control — including pads
inside the edit modes — has no effect.

## Edit modes

The PITCH, GATE, MOD and CONTROL keys each toggle a documented edit mode
over the normal surface (spec MODE-001). The active mode is reported on its
key LED and named on the display; pressing the same key leaves, pressing a
different mode key switches directly. The pad LEDs follow the mode too:
every row whose pads edit a parameter in the mode (sequencer rows in all
four modes, LFO rows in PITCH) lights the single pad whose press sets that
track's current value — dark when the value lies off the pad scale, such as
an encoder-set note outside the pads' 36–51 range — while rows whose pads
are inert in the mode (LFO rows in GATE/MOD/CONTROL) keep their live level
bar. Leaving the mode restores the pattern/playhead view. Entering or
leaving a mode changes no pattern, workspace or settings data, and all edits
inside a mode go to that mode's documented parameters on the selected track
only. What a mode edits follows the selected track's engine:

| Mode | Sequencer lane (pads 1–16) | LFO lane (Param 1 / Param 2 / pads) |
| --- | --- | --- |
| PITCH | Note 36+*n*−1 | Pitch-CV routing on/off (right/left; A–C) / phase offset / pad *n* = *n*−1 sixteenths of phase |
| GATE | Gate length 5+(*n*−1)×6 % | Gate routing on/off (A–C; on D the cycle trigger) / — / — |
| MOD | Mod CV level *n*−1 | Mod-CV routing on/off (A–C) / depth 0–100 % / — |
| CONTROL | Velocity (*n*−1)×8+7 | MIDI CC number 0–127 / — / — |

On a sequencer lane the param encoders keep their normal assignments inside
every mode. LFO D has no pitch/mod jacks: the PITCH and MOD modes report
`NO JACK ON D` there and route nothing. Controls marked — are documented
inert in that mode.

## Projects

A project is the workspace's musical content in both engine modes: each
track's engine selection **and** both of its engine configurations — the
sequencer parameters (pulses, length, rotation, note, gate, velocity, mod
level) and the LFO
parameters (waveform, rate division, depth, phase, CC number, destinations) —
plus the tempo, swing and a 12-character name. Loading a project therefore
restores a track in
either mode with the other engine's setup intact behind the toggle. The
edited-track selection stays in the settings sector. The 64 saved-project
slots follow the spec's documented catalog at SPI `0xC0000 + n × 0x3D000`;
this firmware's record occupies the first 4 KiB sector of its slot (magic
`AGP3`, FNV-1a checked) and the rest of the slot region is never written.
(Records from builds before names, magic `AGP2`, are reported `INVALID`.)

Press **PROJECT** to open the project manager. The 64 pads become the 64
slots (bank A pad 1 = slot A1 … bank D pad 16 = slot D16), lit when populated
and blinking for the current slot; the display shows the workspace name, the
catalog grid and a
status line that reports every operation. Both engines keep running
throughout — browsing slots silences nothing.

The project name is edited here, the way stock firmware's save flow does it:
the **Param 3** encoder moves the cursor (a bright underline on the name,
over a dim baseline) and the **Param 2** encoder steps the character at the
cursor through the character list — space, `A`–`Z`, `0`–`9`, then
`!?#$%&'()*+,-./:;<=>@` (the stock order, reduced to what the uppercase 5×7
font renders distinctly) — both clamped at their ends without wraparound.
Names are a fixed 12 characters, space padded; a new project (and a first
boot) is named `INIT PROJECT`, as in stock. The name is workspace data like
any other edit: saved into the slot's record by SHIFT+pad, restored by a
load, reset by SHIFT+PROJECT, and committed with the settings by GLOBAL or
the power button.

- **Pad** — load that slot into the workspace. An empty or invalid slot is
  reported (`EMPTY` / `INVALID`) and the workspace is not changed.
- **SHIFT + pad** — the explicit save operation: write the workspace to that
  slot. Only that slot is modified, and `SAVED` is reported only after the
  committed record reads back from flash.
- **RECORD + pad** — the explicit delete operation: erase that slot's record
  sector, leaving it observably empty. Deleting an empty slot is accepted
  and performs no flash write. Every other slot, the workspace and the
  settings are untouched.
- **SHIFT + PROJECT** (in any mode) — new project: the workspace returns to
  the declared defaults (the same mixed A/B-sequencer, C/D-LFO setup a first
  boot shows). No saved slot is modified and nothing persists until an
  explicit save.
- **PROJECT** again leaves the manager.

Saved projects persist like settings: they survive power-button shutdown and
a cold restart. An interrupted save or delete leaves the previous record, the
complete new record, or an erased sector that is reported as empty/invalid on
load — never undefined data in the workspace. Re-saving identical content
performs no erase/program cycle (wear bounding, spec STORE-007). The firmware
sizes the catalog from the SPI flash's JEDEC density byte and only addresses
slots whose region physically fits: on the native emulator's 1 MiB model one
slot is available, on the 16 MiB browser model (and the presumed real part)
all 64; out-of-range slots report `NO SLOT` and are never addressed.

The display and the param encoders always follow the selected track's engine
— the **active program**. Only that engine's lanes are drawn (pattern cells
with a playhead on a sequencer program, wave/rate/CC with a live level bar on
an LFO program); a track running the other engine keeps playing and keeps its
pad-LED feedback, but its display row is just a dim `SEQ`/`LFO` tag until a
track of that engine is selected. The screen is never a mix of both engines'
idioms.

## Build

Requires the GNU Arm toolchain (`arm-none-eabi-gcc`) and Python 3.

```sh
make            # build/aleph-void-custom-fw.elf, .bin and .vsb
make check      # run the spec's seven image-profile checks on the VSB
make test       # headless + browser-engine behavioral tests (see below)
```

The code is compiled as the ARMv6-M subset so it runs on both emulator
engines (the native engine's stock Unicorn build cannot deliver interrupts
inside IT blocks); `make ARCH=-mcpu=cortex-m7` builds Thumb-2 instead.
`tools/make_vsb.py` wraps the 64 KiB binary into a spec-conformant VSB,
filling unused payload with 0xFF and balancing the four region checksums.

## Run it

Browser workbench (recommended): in `../../emulator/web`, `npm ci && npm run
dev`, open the printed URL, load `build/aleph-void-custom-fw.vsb` in **Browser
emulator**, and run. The C/D pad rows animate as LFO level bars from the
moment the panel comes up (select track C or D to bring the LFO program onto
the display); press PLAY to run the Euclidean lanes. Importable demo scripts for the
workbench's **UI scripts** panel live in [`scripts/`](scripts/README.md);
they walk through every major function (mixed-engine transport, editing both
engine types, the edit modes with bar selection and the loop, the
SHIFT+track engine toggle, MIDI remote start/stop, external MIDI clock sync,
persistence, project management, version mode and a full self-update).

Native headless:

```sh
python -m pip install -e ../../emulator
make smoke      # 2M instructions, report in build/smoke.json
```

## Tests

- `tools/headless_test.py` (native engine): boot with the mixed defaults,
  tick interrupts, LFO MIDI CC streaming while stopped (and no clock or
  notes), MIDI start driving sequenced note-ons plus clock with CCs
  continuing, external-clock following without re-emission, version and
  update boot chords (including the lit PLAY indicator in version mode and
  PLAY continuing the boot to the ready state, BOOT-004), VUP discovery /
  version / begin, ordered packets, and rejection of out-of-order packets,
  wrong blocks, and installs outside update mode — with zero flash writes.
  Project mode runs against this engine's 1 MiB SPI model: the JEDEC
  capacity probe keeps every access inside the part, slot 0 saves / loads /
  deletes, and the unavailable slot 1 is refused without a flash write.
- `tools/browser_update_test.mjs` (browser WASM engine): full end-to-end
  installation — update-mode boot via the panel chord, USB MIDI enumeration,
  all 224 packets through the firmware's own updater under the workbench's
  installation context, then byte-exact verification of internal flash and
  the three installed SPI regions, with no critical hazard tripped.
- `tools/browser_panel_test.mjs` (browser WASM engine): DMA panel scanning,
  both engines' pad LEDs and outputs, the SHIFT+track engine toggle in both
  directions (including track D's trigger lane and channel-10 notes), the
  spec panel-key requirements on both engine types — the PITCH/GATE/MOD/
  CONTROL edit modes (storage-silent entry/exit, audible pitch/velocity/mod
  edits on a sequencer lane, CC reassignment and gate routing on an LFO
  lane), bar selection windowing the 64-step pattern space, and the loop
  (pinned playback, clearing, refusal on an LFO lane and beyond the pattern
  bounds) — MIDI note and CC output,
  transport, GLOBAL commit, the full project lifecycle (the encoder name
  editor driven closed-loop against the rendered name field, save to first
  and last slots with untouched-slot and settings guards, new-project
  defaults without flash writes, exact load-back of both engine modes
  including the name, empty/corrupt-slot rejection with the workspace
  unchanged, delete to an observably empty slot), power-button shutdown, and
  settings plus saved-project persistence across a cold restart.

## Storage map

| Region | Use |
| --- | --- |
| Internal flash 0x00200000 + 64 KiB | This firmware (VSB payload 0x00000) |
| SPI 0x00000–0x30000 | Installed-image regions; written only by the updater during a host-initiated installation |
| SPI 0x40000 (one 4 KiB sector) | Committed settings (magic `AGG3`, FNV-1a checked); rewritten only when contents change |
| SPI 0xC0000 + n × 0x3D000 (first 4 KiB sector of each of the 64 documented slots) | Saved projects (magic `AGP3`, FNV-1a checked); written only by the explicit save and delete operations |
| Everything else | Never written |

## Provenance

Original work. Interface facts (container format, update protocol, panel
wiring, peripheral behavior) come from the sibling spec's `board.json`,
`contracts.md` and the emulator's independently authored board model — see
`src/board.h` for the consolidated register map. The hardware-interface
modules and both engines are shared with the sibling
[euclidean](../euclidean) and [lfos](../lfos) firmwares by the same authors;
the per-track engine switching, the unified transport and the tests for the
combination are new. This firmware's display driver additionally drains the
panel scan from inside long framebuffer draws (`display_set_breather`) so
encoder detents are never lost to a redraw — the combined two-engine UI
redraws continuously, where the originals could afford to redraw rarely.
Licensed under the MIT License (see LICENSE).

## Known limits

- The stock backup/restore SysEx transport is not implemented; settings and
  the saved-project catalog are this firmware's only persistent data.
- The analog sync jack is not used; external sync means incoming MIDI clock.
- External clock below ~2.5 BPM-equivalent tick spacing (more than one
  second between ticks) is treated as absent.
- MIDI input notes are ignored; the sequenced notes and LFO CC/CV outputs
  are the product.
- Swing applies to the sequencer lanes only; LFO phases follow the unswung
  transport grid.
- LFO lanes have no step grid: the bar keys and the loop act on sequencer
  lanes (the pads still edit an LFO lane's rate/waveform in every bar), and
  the pads are documented inert in an LFO lane's GATE, MOD and CONTROL
  modes.
- The PATTERN key has no normal-mode function; patterns-per-track as in the
  sibling lfos firmware are not implemented. PATTERN participates only in
  the GLOBAL+PATTERN update-mode boot chord.
- Quantitative timing claims (jitter, latency) require hardware measurement
  and are not made.
