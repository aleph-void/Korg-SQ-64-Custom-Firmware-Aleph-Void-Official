# UI demo scripts

Portable `sq64-ui-script` files for the browser workbench's **UI scripts**
panel. Together they demonstrate every major function of this firmware in the
emulator — no hardware involved.

## Running a script

1. In `../../../../emulator/web`: `npm ci && npm run dev`, open the printed
   URL and expand **Browser emulator**.
2. Load `build/aleph-void-custom-fw.vsb` (build it first with `make`).
3. Expand **UI scripts** below the instrument panel, choose **Import
   script**, select one of these files, then **Run script**.

Scripts drive the normal emulator input path, so you can watch the pads,
LEDs, display, and the top-edge gate/CV/trigger indicators respond live.
Each script that boots the instrument waits **20 s** after `power-on` for
the firmware to start scanning the panel (first display frame took about 4 s
on the reference host); on a slow machine or a throttled background tab,
raise the `wait` after each `power-on` before reporting a problem. An
instance keeps running after a script finishes unless the script powers it
off.

## The scripts

The firmware boots with the mixed default configuration: Euclidean sequencer
lanes on tracks A/B, synced LFOs on C/D.

| Script | Demonstrates | Watch for |
| --- | --- | --- |
| `01-transport-and-mixed-engines.json` | PLAY, STOP, tap tempo with both engines live | A/B pattern playheads move while C/D level bars keep breathing; four TAP presses ~420 ms apart raise the BPM readout and speed up everything together; STOP clears the sequencer gates, the LFOs keep running |
| `02-sequencer-editing.json` | Pad = pulses, SHIFT+pad = length, param encoders 3/4 (rotation, note) on a sequencer track | Pad B9 selects track B with E(9,16); SHIFT+B12 trims length to 12; rotation shifts the pattern; note +12 transposes an octave; then it plays |
| `03-lfo-editing.json` | Pad = rate, SHIFT+pad = waveform, param encoders 3/4 (depth, phase), the CONTROL mode (CC assign) and the PITCH/GATE modes (destination routing) on an LFO track | Pad C8 sets LFO C to 1/4; SHIFT+C5 switches it to square; depth drops; in the CONTROL mode the CC number climbs on Param 1; in the PITCH and GATE modes Param 1 routes those destinations on and off with jack activity on the top edge |
| `04-engine-toggle.json` | SHIFT+track button converts a track between engines | A's pattern row becomes a live level bar (A is now an LFO on mod CV); D becomes a Euclidean trigger lane; after PLAY the mix runs; A toggles back mid-playback |
| `05-midi-remote-start-stop.json` | MIDI real-time start/stop in | Transport starts without touching the panel; sequencer lanes fire; stop silences them |
| `06-external-midi-clock.json` | External MIDI clock following | The header flips INT→EXT with the measured BPM; sequencer steps and LFO phases follow the external ticks; a second after they stop it falls back to INT |
| `07-save-and-persistence.json` | GLOBAL commit and persistence (including engine modes) | Track B edited to 5 pulses, track C toggled to a sequencer lane, GLOBAL commits, power cycles, and both edits come back |
| `08-version-mode.json` | GLOBAL boot chord, PLAY continues boot | The version screen with firmware 1.0, the original-firmware notice and `PRESS PLAY TO CONTINUE` with the PLAY LED lit; pressing PLAY resumes the normal boot to the mixed-engine screen (after the splash), then a clean power-off |
| `09-update-mode-install.json` | GLOBAL+PATTERN boot chord and a full self-update | The updater's progress through all 224 packets of a second image, then the completion screen |
| `10-project-management.json` | The PROJECT manager: save, new project, load, empty-slot report, delete — a project carries both engine modes | Track B gets 5 pulses and track A becomes an LFO, then the mix is saved to slot A1 (its pad lights and blinks as current); SHIFT+PROJECT resets the workspace to the A/B-sequencer defaults; loading A1 brings the LFO on A and the 5 pulses back; empty slot C5 reports `C5 EMPTY` without changing anything; RECORD+pad deletes A1 (its pad goes dark); PROJECT returns to the lanes |
| `11-mode-switch-full-sweep.json` | SHIFT+track walks every track through both engines: all-LFO, then all-Euclidean, then back to the mixed defaults, mid-playback | A and B become level bars (sine 1/4 and triangle 1/2 on mod CV) so all four rows breathe together after PLAY, with no gates firing; then A–D flip to pattern lanes one by one and playheads, gates and D's triggers come alive; finally C and D return to LFOs, restoring the boot mix without stopping the transport |
| `12-mode-switch-keeps-settings.json` | Each engine's settings survive switching away and back | Track A is edited to E(5,16), switched to an LFO and edited there too (pad A11 sets 1/8, SHIFT+A5 sets square); toggling back shows the 5-pulse pattern intact, toggling again shows the square 1/8 LFO intact, and a final toggle plus PLAY runs the preserved E(5,16) |
| `13-seq-modes-loop-bars.json` | The panel-key requirements on a sequencer lane: PITCH/GATE/MOD/CONTROL edit modes, bar selection, the loop | Each mode key lights its LED and the pads retarget one parameter of track B (note, gate %, mod level — watch the B MOD indicator jump to full — velocity); key 2 selects bar 2 and SHIFT+B4 stretches the pattern to 20 steps; LOOP pins playback inside that bar until cleared; key 1 returns to bar 1 |
