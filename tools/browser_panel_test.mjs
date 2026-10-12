#!/usr/bin/env node
// Normal-mode interaction test against the browser (WASM) engine: panel DMA
// scanning, both engines' pad LEDs and outputs, per-track mode toggling with
// SHIFT+track, transport, LFO destinations, MIDI note and CC output,
// settings commit and persistence.
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
check(idleLeds.mod < 64, 'destination LEDs dark while a sequencer track is edited');

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
check(machine.board.panel.leds.mod > 128, 'MOD destination LED lit for LFO A');
const aSamples = [];
for (let i = 0; i < 6; i++) { aSamples.push(outputLevels(machine.board).Amod); run(800000); }
check(new Set(aSamples).size > 2, `LFO A mod CV animates (${aSamples.join(',')})`);
const ccMark = midi.length;
run(4000000);
check(midi.slice(ccMark).some(m => (m[0] & 0xff) === 0xb0 && m[1] === 1),
  'LFO A streams CC 1 on channel 1');

// The CC encoder (swing position while an LFO is edited) re-assigns LFO A's
// controller number upward; whatever number it lands on must stream and then
// persist exactly across the commit and restart below.
encoder('swing', 5);
run(3000000);
const ccMark2 = midi.length;
run(4000000);
const reassigned = midi.slice(ccMark2).filter(m => (m[0] & 0xff) === 0xb0).map(m => m[1]);
const assignedCC = reassigned.length ? reassigned[reassigned.length - 1] : 1;
check(assignedCC > 1 && assignedCC <= 6,
  `LFO A streams on a re-assigned controller (CC ${assignedCC})`);

// Gate destination on LFO A: square-thresholded gate jack activity.
press('gate');
check(machine.board.panel.leds.gate > 128, 'gate destination LED lit after toggle');
let gateHigh = false, gateLow = false;
for (let i = 0; i < 40 && !(gateHigh && gateLow); i++) {
  run(300000);
  const g = outputLevels(machine.board).Agate;
  if (g === 255) gateHigh = true;
  if (g === 0) gateLow = true;
}
check(gateHigh && gateLow, 'square-thresholded gate toggles on LFO A jack');
press('gate'); // toggle back off
run(400000);
check(outputLevels(machine.board).Agate === 0, 'gate off after destination cleared');

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
let n = 6000000;
while (n > 0) { const r = machine2.run(200000); if (r.error) throw Error(r.error); n -= 200000; }
check(machine2.board.panel.leds.trackA > 128, 'committed selection (track A) restored after restart');
check(midi2.some(m => (m[0] & 0xff) === 0xb0 && m[1] === assignedCC),
  `committed mode and CC (A as LFO on CC ${assignedCC}) restored after restart`);
const restoredB = Array.from({length: 16}, (_, i) => machine2.board.panel.leds['B' + (i + 1)] > 128).filter(Boolean).length;
check(restoredB === 5, `committed sequencer edit restored after restart (track B, ${restoredB} pulses)`);
machine2.close();

machine.close();
if (failures.length) { console.error('FAILED: ' + failures.join('; ')); process.exit(1); }
console.log('browser-engine panel test passed');
