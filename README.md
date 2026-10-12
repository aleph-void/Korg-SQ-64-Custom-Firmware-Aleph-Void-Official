# AV aggregate — original SQ-64 firmware

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
length) with rotation, 1–16 steps, polymetric against the other lanes; swing
50–75 % across all sequencer lanes. Lanes A/B/C drive their gate jack, pitch
CV, mod CV and MIDI notes on channels 1–3; lane D drives trigger jack 1 and
MIDI channel 10. Pad LEDs show the pattern with a moving playhead.

**LFO lane** (per track in LFO mode): six waveforms — sine, triangle, rise,
fall, square, sample & hold — sixteen synced rate divisions from 8 bars to
1/32 including dotted and triplet values, per-LFO depth, phase offset and
MIDI CC number streamed on channels 1–4 (USB and DIN, rate-limited, sent only
on change). LFOs A/B/C can drive their mod CV, pitch CV and gate jacks
(gate = square-thresholded LFO), toggled with the GATE / PITCH / MOD buttons;
LFO D pulses trigger jack 1 once per cycle. Pad LEDs are a live level bar and
the display shows a waveform preview with a phase cursor.

Settings persist to SPI flash (GLOBAL commits; the power button saves and
shuts down cleanly through the PB7 power hold). Boot chords per the spec:
hold **GLOBAL** at power-on for version mode, **GLOBAL+PATTERN** for update
mode, with the same complete VUP self-updater as the sibling firmwares
(protocol 1, 1024-byte packets, full validation before a RAM-resident
installer programs anything).

## Controls (normal mode)

| Control | Action |
| --- | --- |
| PLAY / STOP | Start from step 0 with all LFO phases resynced / stop (sequencer notes off, LFOs keep running) |
| TAP | Tap tempo |
| A B C D | Select the edited track |
| SHIFT + A B C D | Toggle that track between sequencer lane and LFO |
| Pad *n* (row of track *t*) | SEQ: set pulses to *n* · LFO: set rate division *n* (1=8 bars … 16=1/32) |
| SHIFT + pad *n* | SEQ: set length to *n* · LFO: set waveform (1=sine 2=tri 3=rise 4=fall 5=square 6=S+H) |
| GATE / PITCH / MOD | LFO lane only: toggle that destination (on D, GATE = cycle trigger) |
| Tempo encoder | BPM (internal clock) |
| Swing encoder | SEQ: swing % · LFO: MIDI CC number |
| Param 1–4 encoders | SEQ: pulses, length, rotation, note · LFO: waveform, rate, depth, phase |
| GLOBAL | Commit settings to SPI flash |
| Power button | Save and shut down |

The second display line and the param encoders always follow the selected
track's engine; each lane row renders in its own idiom (pattern cells with a
playhead, or wave/rate/CC with a live level bar).

## Build

Requires the GNU Arm toolchain (`arm-none-eabi-gcc`) and Python 3.

```sh
make            # build/av-aggregate.elf, .bin and .vsb
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
dev`, open the printed URL, load `build/av-aggregate.vsb` in **Browser
emulator**, and run. The LFO lanes animate from the moment the panel comes
up; press PLAY to add the Euclidean lanes. Importable demo scripts for the
workbench's **UI scripts** panel live in [`scripts/`](scripts/README.md);
they walk through every major function (mixed-engine transport, editing both
engine types, the SHIFT+track engine toggle, MIDI remote start/stop,
external MIDI clock sync, persistence, version mode and a full self-update).

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
  update boot chords, VUP discovery / version / begin, ordered packets, and
  rejection of out-of-order packets, wrong blocks, and installs outside
  update mode — with zero flash writes.
- `tools/browser_update_test.mjs` (browser WASM engine): full end-to-end
  installation — update-mode boot via the panel chord, USB MIDI enumeration,
  all 224 packets through the firmware's own updater under the workbench's
  installation context, then byte-exact verification of internal flash and
  the three installed SPI regions, with no critical hazard tripped.
- `tools/browser_panel_test.mjs` (browser WASM engine): DMA panel scanning,
  both engines' pad LEDs and outputs, the SHIFT+track engine toggle in both
  directions (including track D's trigger lane and channel-10 notes), LFO
  destination toggles, MIDI note and CC output with CC reassignment,
  transport, GLOBAL commit, power-button shutdown, and settings persistence
  across a cold restart.

## Storage map

| Region | Use |
| --- | --- |
| Internal flash 0x00200000 + 64 KiB | This firmware (VSB payload 0x00000) |
| SPI 0x00000–0x30000 | Installed-image regions; written only by the updater during a host-initiated installation |
| SPI 0x40000 (one 4 KiB sector) | Committed settings (magic `AGG1`, FNV-1a checked); rewritten only when contents change |
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

- The stock backup/restore SysEx transport is not implemented; settings are
  this firmware's only persistent data.
- The analog sync jack is not used; external sync means incoming MIDI clock.
- External clock below ~2.5 BPM-equivalent tick spacing (more than one
  second between ticks) is treated as absent.
- MIDI input notes are ignored; the sequenced notes and LFO CC/CV outputs
  are the product.
- Swing applies to the sequencer lanes only; LFO phases follow the unswung
  transport grid.
- Quantitative timing claims (jitter, latency) require hardware measurement
  and are not made.
