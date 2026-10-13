#!/usr/bin/env node
// Normal-mode interaction test against the browser (WASM) engine: panel DMA
// scanning, both engines' pad LEDs and outputs, per-track engine toggling
// with SHIFT+track, transport, the spec panel-key requirements (edit modes,
// bar selection, loop) on both engine types, LFO destination routing inside
// the edit modes, MIDI note and CC output, the full project lifecycle over
// the 64 documented saved-project slots (PROJECT-001..004, covering both
// engine modes), settings commit and persistence.
//
// Usage: node tools/browser_panel_test.mjs [path/to/emulator/web] [image.vsb]
import {readFileSync} from 'node:fs';
import {pathToFileURL} from 'node:url';
import {resolve} from 'node:path';

const webRoot = resolve(process.argv[2] || new URL('../../../emulator/web', import.meta.url).pathname);
const imagePath = resolve(process.argv[3] || new URL('../build/av-aggregate.vsb', import.meta.url).pathname);
const load = path => import(pathToFileURL(resolve(webRoot, path)));

const {default: createUnicorn} = await load('vendor/unicorn-arm.js');
const {Machine} = await load('emulator/core.js');
const {loadImage} = await load('emulator/image.js');
const {outputLevels} = await load('emulator/outputs.js');

const failures = [];
const check = (condition, label) => {
  console.log((condition ? 'ok  ' : 'FAIL') + ' ' + label);
  if (!condition) failures.push(label);
};

const engine = await createUnicorn();
const midi = [];
const machine = new Machine(engine, await loadImage(new Uint8Array(readFileSync(imagePath))),
  {onMidi: m => midi.push(Array.from(m))});
const run = n => {
  while (n > 0) {
    const report = machine.run(Math.min(n, 200000));
    if (report.error) throw Error('emulator: ' + report.error);
    n -= 200000;
  }
};
const press = (control, instructions = 400000) => {
  machine.event({type: 'button', control, down: true});
  run(instructions);
  machine.event({type: 'button', control, down: false});
  run(instructions);
};
// Deliver encoder detents one at a time with virtual time between them, the
// way the workbench's interactive input layer spaces real knob gestures (its
// dwell is 40 virtual ms per detent). The emulator's panel scan DMA is an
// approximation ('panel-dma-scan-timing'): a detent can occasionally land in
// a scan half-buffer that is replaced mid-read, so closed-loop turning (like
// a human watching the value) is used where an exact target matters.
const encoder = (control, delta) => {
  for (let i = 0; i < Math.abs(delta); i++) {
    machine.event({type: 'encoder', control, delta: Math.sign(delta)});
    run(600000);
  }
};
const turnTo = (control, read, target, tries = 12) => {
  for (let i = 0; i < tries && read() !== target; i++) {
    encoder(control, Math.sign(target - read()));
    run(600000);
  }
  return read();
};
// SHIFT + a track button toggles that track between sequencer and LFO.
const shiftPress = control => {
  machine.event({type: 'button', control: 'shift', down: true});
  run(400000);
  press(control);
  machine.event({type: 'button', control: 'shift', down: false});
  run(400000);
};

run(1500000); // boot into normal mode
// Prime the emulator's encoder model: the first encoder event after boot is
// not delivered to the firmware (the baseline firmware tests sent an
// unasserted tempo nudge first, which this reproduces deliberately).
machine.event({type: 'encoder', control: 'tempo', delta: 1});
run(1000000);
check(machine.board.panel.displayFrames > 0, 'display rendered');
const idleLeds = {...machine.board.panel.leds};
check(idleLeds.trackA > 128, 'selected-track LED lit');
check(idleLeds.A1 > 128 && idleLeds.A5 > 128 && idleLeds.A2 < 64,
  'sequencer track A default pattern E(4,16) on pad LEDs');
check(idleLeds.stop > 128 && idleLeds.play < 64, 'stop LED lit while stopped');
check(idleLeds.mod < 64 && idleLeds.pitch < 64 && idleLeds.gate < 64 && idleLeds.control < 64,
  'mode-key LEDs dark while no edit mode is active');
check(idleLeds.bar1 > 128 && idleLeds.bar2 < 64, 'bar 1 selected by default');

// LFO lanes free-run from boot: C's mod CV must keep moving while stopped.
const samples = [];
for (let i = 0; i < 6; i++) { samples.push(outputLevels(machine.board).Cmod); run(800000); }
check(new Set(samples).size > 2, `LFO C mod CV animates while stopped (${samples.join(',')})`);
check(midi.some(m => (m[0] & 0xf0) === 0xb0 && (m[0] & 15) === 2 && m[1] === 11),
  'LFO C streams CC 11 on channel 3');
check(midi.some(m => (m[0] & 0xf0) === 0xb0 && (m[0] & 15) === 3 && m[1] === 74),
  'LFO D streams CC 74 on channel 4');
check(!midi.some(m => (m[0] & 0xf0) === 0x90), 'no sequenced notes while stopped');

// Transport: PLAY starts the sequencer lanes and the MIDI clock, and
// resyncs the LFO phases.
press('play');
run(6000000); // almost a second of virtual time at 120 BPM
const playing = outputLevels(machine.board);
check(midi.some(m => m[0] === 0xfa), 'MIDI start sent');
check(midi.some(m => m[0] === 0xf8), 'MIDI clock sent while playing');
check(midi.some(m => (m[0] & 0xf0) === 0x90 && (m[0] & 15) === 0), 'track A note-ons sent on channel 1');
check(machine.board.panel.leds.play > 128, 'play LED lit');
check(playing.Apitch > 0, `track A pitch CV level ${playing.Apitch}`);
let gateSeen = playing.Agate === 255;
for (let i = 0; i < 20 && !gateSeen; i++) { run(400000); gateSeen = outputLevels(machine.board).Agate === 255; }
check(gateSeen, 'track A gate observed active');

// SHIFT+D switches track D from LFO to sequencer: E(7,16) on trigger jack 1.
shiftPress('trackD');
check(machine.board.panel.leds.trackD > 128, 'mode toggle selected track D');
let trigSeen = false;
for (let i = 0; i < 40 && !trigSeen; i++) { run(200000); trigSeen = outputLevels(machine.board).D1 === 255; }
check(trigSeen, 'sequencer track D fires trigger jack 1');
check(midi.some(m => (m[0] & 0xf0) === 0x90 && (m[0] & 15) === 9),
  'sequencer track D sends notes on channel 10');

press('stop');
run(2000000);
check(midi.some(m => m[0] === 0xfc), 'MIDI stop sent');
const stopped = outputLevels(machine.board);
check(stopped.Agate === 0 && stopped.Bgate === 0 && stopped.D1 === 0,
  'sequencer gates and triggers inactive after stop');

// Pad B7 sets sequencer track B pulses to 7 and selects track B.
press('B7');
run(1000000);
const ledsB = machine.board.panel.leds;
check(ledsB.trackB > 128 && ledsB.trackA < 64, 'pad press selected track B');
const bOn = Array.from({length: 16}, (_, i) => ledsB['B' + (i + 1)] > 128).filter(Boolean).length;
check(bOn === 7, `track B shows 7 pulses on LEDs (got ${bOn})`);

// Param1 (pulses) encoder turns track B down to 5 pulses.
const bPulses = () => Array.from({length: 16}, (_, i) => machine.board.panel.leds['B' + (i + 1)] > 128).filter(Boolean).length;
const bAfter = turnTo('param1', bPulses, 5);
check(bAfter === 5, `pulses encoder reduced track B to 5 pulses (got ${bAfter})`);

// SHIFT+A switches track A from sequencer to LFO: its default destination is
// mod CV, its CC is 1 on channel 1, and the pad row becomes a level bar.
shiftPress('trackA');
const aSamples = [];
for (let i = 0; i < 6; i++) { aSamples.push(outputLevels(machine.board).Amod); run(800000); }
check(new Set(aSamples).size > 2, `LFO A mod CV animates (${aSamples.join(',')})`);
const ccMark = midi.length;
run(4000000);
check(midi.slice(ccMark).some(m => (m[0] & 0xff) === 0xb0 && m[1] === 1),
  'LFO A streams CC 1 on channel 1');

// The CONTROL edit mode (MODE-001) scopes Param 1 to LFO A's MIDI CC number;
// whatever number it lands on must stream and then persist exactly across
// the commit and restart below.
press('control');
check(machine.board.panel.leds.control > 128, 'CONTROL mode LED lit');
encoder('param1', 5);
run(3000000);
const ccMark2 = midi.length;
run(4000000);
const reassigned = midi.slice(ccMark2).filter(m => (m[0] & 0xff) === 0xb0).map(m => m[1]);
const assignedCC = reassigned.length ? reassigned[reassigned.length - 1] : 1;
check(assignedCC > 1 && assignedCC <= 6,
  `LFO A streams on a re-assigned controller (CC ${assignedCC})`);
press('control');
check(machine.board.panel.leds.control < 64, 'second press leaves CONTROL mode');

// Gate routing on LFO A, edited inside the GATE mode (Param 1 right = on,
// left = off): square-thresholded gate jack activity.
press('gate');
check(machine.board.panel.leds.gate > 128, 'GATE mode LED lit');
encoder('param1', 1); // route the gate destination on
let gateHigh = false, gateLow = false;
for (let i = 0; i < 40 && !(gateHigh && gateLow); i++) {
  run(300000);
  const g = outputLevels(machine.board).Agate;
  if (g === 255) gateHigh = true;
  if (g === 0) gateLow = true;
}
check(gateHigh && gateLow, 'square-thresholded gate toggles on LFO A jack');
encoder('param1', -1); // route it back off
run(400000);
check(outputLevels(machine.board).Agate === 0, 'gate off after routing cleared');
press('gate'); // leave the GATE mode
run(400000);

// --- Edit modes on a sequencer lane (MODE-001): PITCH/GATE/MOD/CONTROL
// scope the pads to one parameter of the selected track; entry, edits and
// exit write no storage. ---
const noteOns = (from, channel) =>
  midi.slice(from).filter(m => (m[0] & 0xf0) === 0x90 && (m[0] & 15) === channel);
press('trackB');
const writesBeforeModes = machine.board.flash.writes;
press('pitch');
check(machine.board.panel.leds.pitch > 128, 'PITCH mode LED lit');
press('B8'); // note 43 (G2)
press('pitch');
check(machine.board.panel.leds.pitch < 64, 'second press leaves PITCH mode');
press('control');
press('B16'); // velocity 127
press('control');
press('gate');
press('B15'); // gate 89 % of a step
press('gate');
press('mod');
check(machine.board.panel.leds.mod > 128, 'MOD mode LED lit');
press('B16'); // mod level 15 (full scale)
press('mod');
check(bPulses() === 5, 'mode edits left the pattern pulses alone');
check(machine.board.flash.writes === writesBeforeModes,
  'mode entry, edits and exit wrote nothing to storage');

// The mode edits are audible: track B now plays note 43 at velocity 127
// with a full-scale mod CV while its gate is open.
const modeMark = midi.length;
press('play');
run(4000000);
const bOns = noteOns(modeMark, 1);
check(bOns.length > 0 && bOns.every(m => m[1] === 43 && m[2] === 127),
  `track B plays the pitch and velocity set in the edit modes (${bOns.length} notes)`);
let modSeen = false;
for (let i = 0; i < 40 && !modSeen; i++) { run(200000); modSeen = outputLevels(machine.board).Bmod === 255; }
check(modSeen, 'track B mod CV reaches the mod-mode level');
press('stop');
run(1000000);

// --- Bar selection (BAR-001): keys 1..4 window the pads onto one 16-step
// bar of the 64-step pattern space. ---
press('bar2');
const barLeds = machine.board.panel.leds;
check(barLeds.bar2 > 128 && barLeds.bar1 < 64, 'bar 2 selected and shown');
check(bPulses() === 0, 'bar 2 of a 16-step pattern shows no steps');
shiftPress('B4'); // SHIFT+pad in bar 2: length = step 20
check(bPulses() === 1, 'E(5,20) shows exactly its bar-2 step (17)');
press('bar1');
check(machine.board.panel.leds.bar1 > 128, 'bar 1 reselected');
check(bPulses() === 4, 'bar 1 of E(5,20) shows its four steps');

// --- Loop (LOOP-001): LOOP loops step playback over the selected bar of the
// selected sequencer lane's pattern; clearing it resumes normal playback. ---
press('B1'); // pulses = 1: track B fires only at step 1 (inside bar 1)
press('bar2');
press('loop');
check(machine.board.panel.leds.loop > 128, 'loop LED lit');
const loopMark = midi.length;
press('play');
run(8000000);
press('stop');
run(500000);
const loopB = noteOns(loopMark, 1).length, loopD = noteOns(loopMark, 9).length;
check(loopB === 0 && loopD > 0,
  `loop over bar 2 keeps playback off track B's bar-1 pulse (D ${loopD}, B ${loopB})`);
press('loop');
check(machine.board.panel.leds.loop < 64, 'loop cleared');
const clearMark = midi.length;
press('play');
run(8000000);
press('stop');
run(500000);
check(noteOns(clearMark, 1).length > 0, 'clearing the loop resumes normal playback');

// A loop is refused on an LFO lane, and on a bar beyond the active pattern.
press('trackA'); // track A is an LFO here
press('loop');
check(machine.board.panel.leds.loop < 64, 'loop refused on an LFO lane');

// Restore the workspace (track B, E(5,16), bar 1) for the project checks.
press('bar1');
shiftPress('B16'); // length = step 16
press('B5');
check(bPulses() === 5 && machine.board.panel.leds.trackB > 128,
  'workspace restored to track B E(5,16)');
press('bar2');
press('loop'); // bar 2 now lies beyond the 16-step pattern
check(machine.board.panel.leds.loop < 64, 'loop beyond the pattern bounds refused');
press('bar1');
press('trackA'); // restore the edited-track selection for the commit below
run(400000);

// --- Project management (PROJECT-001..004) --------------------------------
// Slot n's record lives in the first bytes of SPI 0xC0000 + n * 0x3D000. The
// workspace here is distinct in both engine modes: A is an LFO on the
// reassigned CC, B a sequencer lane at 5 pulses.
const slotBytes = (m, slot, n = 64) => {
  const base = 0xc0000 + slot * 0x3d000;
  return Array.from(m.board.flash.data.slice(base, base + n));
};
const settingsSector = m => Array.from(m.board.flash.data.slice(0x40000, 0x41000)).join();
const hold = (control, down) => { machine.event({type: 'button', control, down}); run(400000); };
const bLit = m => Array.from({length: 16}, (_, i) => m.board.panel.leds['B' + (i + 1)] > 128).filter(Boolean).length;

press('project'); // enter the project manager
run(400000);
check(machine.board.panel.leds.project > 128, 'PROJECT LED lit in project mode');

// PROJECT-003: SHIFT+pad saves the workspace to slot A1 only.
const settingsBefore = settingsSector(machine);
hold('shift', true);
press('A1');
hold('shift', false);
const a1 = slotBytes(machine, 0);
check(a1[0] === 0x41 && a1[1] === 0x47 && a1[2] === 0x50 && a1[3] === 0x32,
  'save wrote a project record to slot A1 (magic AGP2)');
check(a1[8] === 1 && a1[9] === 0, 'saved record carries the mixed engine modes (A LFO, B sequencer)');
check(slotBytes(machine, 1).every(b => b === 0xff), 'save left neighbor slot A2 untouched');
check(settingsSector(machine) === settingsBefore, 'save left the settings sector untouched');

// PROJECT-001: SHIFT+PROJECT starts a new default project in the workspace
// without touching any saved slot or the flash.
const writesBeforeNew = machine.board.flash.writes;
const a1Saved = slotBytes(machine, 0).join();
hold('shift', true);
press('project');
hold('shift', false);
check(machine.board.flash.writes === writesBeforeNew, 'new project performed no flash writes');
check(slotBytes(machine, 0).join() === a1Saved, 'new project left saved slot A1 untouched');
press('project'); // leave the manager to observe the workspace
run(1000000);
check(bLit(machine) === 3, `new project restored declared defaults (track B at E(3,16), got ${bLit(machine)})`);
check(machine.board.panel.leds.A1 > 128 && machine.board.panel.leds.A2 < 64,
  'new project restored track A as the default sequencer lane');

// PROJECT-002: loading slot A1 reproduces the saved workspace exactly, in
// both engine modes: B back at 5 pulses, A back to an LFO streaming its CC.
press('project');
press('A1');
press('project');
run(1000000);
check(bLit(machine) === 5, 'loading slot A1 restored the sequencer edit (track B at 5 pulses)');
const loadMark = midi.length;
run(4000000);
check(midi.slice(loadMark).some(m => (m[0] & 0xff) === 0xb0 && m[1] === assignedCC),
  `loading slot A1 restored track A as an LFO on CC ${assignedCC}`);

// PROJECT-002: an empty slot is reported without changing the workspace.
press('project');
press('C5');
press('project');
run(400000);
check(bLit(machine) === 5, 'empty-slot load left the workspace unchanged');

// PROJECT-002: a corrupted record is rejected without changing the workspace.
machine.board.flash.data[0xc0000] ^= 0xff;
press('project');
press('A1');
press('project');
run(400000);
check(bLit(machine) === 5, 'invalid-slot load left the workspace unchanged');

// PROJECT-004: RECORD+pad deletes exactly one slot, observably empty after.
press('project');
hold('shift', true); // repopulate A1 first (over the corrupted record)
press('A1');
hold('shift', false);
hold('record', true);
press('A1');
hold('record', false);
check(slotBytes(machine, 0, 4096).every(b => b === 0xff), 'deleted slot A1 is observably empty');
press('A1'); // a load attempt now reports empty and changes nothing

// Save to the last slot D16 (63) for the cold-restart persistence check.
hold('shift', true);
press('D16');
hold('shift', false);
check(slotBytes(machine, 63)[0] === 0x41, 'project saved to last slot D16');
press('project'); // leave the project manager
run(400000);
check(machine.board.panel.leds.project < 64, 'PROJECT LED off after leaving project mode');
check(bLit(machine) === 5, 'deleted-slot load left the workspace unchanged');

// Commit with GLOBAL, then power off and verify the hold release.
const writesBefore = machine.board.flash.writes;
press('global');
run(2000000);
check(machine.board.flash.writes > writesBefore, 'GLOBAL committed settings to SPI flash');

machine.event({type: 'power', down: true});
run(4000000);
check(machine.board.poweredOff, 'power button shutdown releases the hold');

// Cold restart with the same SPI flash contents: committed settings persist,
// including the per-track engine modes.
const savedSpi = machine.board.flash.data.slice();
const midi2 = [];
const machine2 = new Machine(engine, await loadImage(new Uint8Array(readFileSync(imagePath))), {onMidi: m => midi2.push(Array.from(m))});
machine2.board.flash.data.set(savedSpi);
const run2 = n => {
  while (n > 0) { const r = machine2.run(200000); if (r.error) throw Error(r.error); n -= 200000; }
};
run2(6000000);
check(machine2.board.panel.leds.trackA > 128, 'committed selection (track A) restored after restart');
check(midi2.some(m => (m[0] & 0xff) === 0xb0 && m[1] === assignedCC),
  `committed mode and CC (A as LFO on CC ${assignedCC}) restored after restart`);
const restoredB = Array.from({length: 16}, (_, i) => machine2.board.panel.leds['B' + (i + 1)] > 128).filter(Boolean).length;
check(restoredB === 5, `committed sequencer edit restored after restart (track B, ${restoredB} pulses)`);

// Saved projects persist across the cold restart: reset the workspace to the
// defaults (B back to 3 pulses), then load slot D16 to bring back the mixed
// workspace that was saved before the shutdown.
const press2 = control => {
  machine2.event({type: 'button', control, down: true}); run2(400000);
  machine2.event({type: 'button', control, down: false}); run2(400000);
};
const bLit2 = () => Array.from({length: 16}, (_, i) => machine2.board.panel.leds['B' + (i + 1)] > 128).filter(Boolean).length;
machine2.event({type: 'button', control: 'shift', down: true}); run2(400000);
press2('project');
machine2.event({type: 'button', control: 'shift', down: false}); run2(400000);
check(bLit2() === 3, `new project after restart reset track B to the default (got ${bLit2()})`);
press2('project');
press2('D16');
press2('project');
run2(1000000);
check(bLit2() === 5, 'project in last slot D16 reloads after the cold restart');
const mark2 = midi2.length;
run2(4000000);
check(midi2.slice(mark2).some(m => (m[0] & 0xff) === 0xb0 && m[1] === assignedCC),
  'reloaded project restored both engine modes (A as LFO streaming again)');
machine2.close();

machine.close();
if (failures.length) { console.error('FAILED: ' + failures.join('; ')); process.exit(1); }
console.log('browser-engine panel test passed');
