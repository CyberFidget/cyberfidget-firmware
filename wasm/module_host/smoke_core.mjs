// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2026 Dismo Industries LLC
import fs from 'node:fs';
import path from 'node:path';
import assert from 'node:assert/strict';
import { createRequire } from 'node:module';
import { createHash } from 'node:crypto';
import { attachGuest, framebuffer } from './host.mjs';
import { readManifest, verifyImports } from '../device_module/verify_imports.mjs';

const [corePath, guestPath, manifestPath] = process.argv.slice(2);
if (!manifestPath) throw new Error('Usage: node smoke_core.mjs <core.js> <guest.wasm> <cf-imports.json>');
const manifest = readManifest(fs.readFileSync(manifestPath, 'utf8'));
const bytes = fs.readFileSync(guestPath);
verifyImports(bytes, manifest);
// Node has no audio device. This mock lets the existing Web Audio bridge run;
// it does not establish audible/browser parity.
const parameter = () => ({ setValueAtTime() {} });
let toneStarts = 0;
const frequencies = [];
class AudioContext {
    currentTime = 0; destination = {};
    createOscillator() { return { frequency: { setValueAtTime(value) { frequencies.push(value); } }, connect() {}, start() { ++toneStarts; }, stop() {} }; }
    createGain() { return { gain: parameter(), connect() {} }; }
}
globalThis.window = { AudioContext };
const factory = createRequire(import.meta.url)(path.resolve(corePath));
let flushes = 0, ledUpdates = 0, exits = 0;
const core = await factory({
    wasmBinary: fs.readFileSync(corePath.replace(/\.js$/, '.wasm')),
    onFrameReady: () => ++flushes,
    onLedUpdate: () => ++ledUpdates,
    onAppExit: () => ++exits,
});
for (const name of ['wasm_button_press', 'wasm_button_release', 'wasm_set_slider', 'wasm_set_accel',
    'wasm_get_framebuffer', 'wasm_get_framebuffer_size', 'wasm_module_frame']) {
    assert.equal(typeof core[`_${name}`], 'function', `Missing export ${name}`);
}
// Check LED delivery independently of whether this guest uses LEDs.
assert.equal(core._cf_nop(7), 8);
core._cf_led_set(0, 32, 0, 0, 0);
core._cf_led_all_off();
const guest = await attachGuest(core, bytes, manifest);
// Pin the adapter's validation order at the end of memory and after growth.
const memory = guest.instance.exports.memory;
let end = memory.buffer.byteLength;
assert.throws(() => guest.imports.cf.display_draw_string(0, 0, end - 1, 96), /Guest memory range/);
assert.throws(() => guest.imports.cf.log(0, -1), /Guest memory range/);
assert.throws(() => guest.imports.cf.display_draw_xbm(0, 0, 128, 64, end - 1), /Guest memory range/);
assert.doesNotThrow(() => guest.imports.cf.display_draw_xbm(0, 0, 129, 64, -1));
assert.doesNotThrow(() => guest.imports.cf.seq_play(end, -1));
assert.throws(() => guest.imports.cf.seq_play(end - 511, 1000), /Guest memory range/);
memory.grow(1);
end = memory.buffer.byteLength;
new Uint8Array(memory.buffer, end - 96, 96).fill(65);
assert.doesNotThrow(() => guest.imports.cf.display_string_width(end - 96, 96));
// A sequence must survive overwriting both guest memory and the transfer buffer.
const stepsPtr = end - 512;
const steps = new DataView(memory.buffer);
new Uint8Array(memory.buffer, stepsPtr, 512).fill(0);
steps.setFloat32(stepsPtr, 440, true); steps.setUint16(stepsPtr + 4, 1, true);
steps.setFloat32(stepsPtr + 8, 660, true); steps.setUint16(stepsPtr + 12, 1, true);
guest.imports.cf.seq_play(stepsPtr, 1000); // Validate/copy only the clamped 64 steps.
new Uint8Array(memory.buffer, stepsPtr, 512).fill(0);
core.HEAPU8.fill(0, core._wasm_module_buffer(), core._wasm_module_buffer() + 512);
core._wasm_module_frame();
await new Promise(resolve => setTimeout(resolve, 20));
core._wasm_module_frame();
assert.deepEqual(frequencies.slice(0, 2), [440, 660], 'Audio loop must play the persistent sequence copy');
core._cf_seq_stop();
core._wasm_module_start();
const hashes = new Set();
let nonBlank = 0;
for (let frame = 0; frame < 300; ++frame) {
    // Real wall-clock pacing; no synthetic clock enters production builds.
    if (frame % 25 === 2) core._wasm_button_press(5);
    if (frame % 25 === 7) core._wasm_button_release(5);
    core._wasm_set_slider((frame * 31) % 4096);
    core._wasm_set_accel(Math.sin(frame / 10), Math.cos(frame / 10), 1);
    core._wasm_module_frame();
    const pixels = framebuffer(core);
    if (pixels.some(Boolean)) ++nonBlank;
    hashes.add(createHash('sha256').update(pixels).digest('hex'));
    await new Promise(resolve => setTimeout(resolve, 20));
}
assert.equal(nonBlank, 300, 'Guest must render every frame');
assert.ok(hashes.size > 1, 'Framebuffer must change');
assert.ok(flushes >= 300, 'Frame callback must fire');
assert.ok(ledUpdates > 0, 'LED callback must fire');
assert.equal(exits, 0, 'Guest exited unexpectedly');
core._wasm_module_end();
core._wasm_module_end();
assert.equal(exits, 1, 'End must be idempotent');
// Exit is deferred until after a guest call, then ends exactly once.
let endCalls = 0;
core.guestCall = kind => {
    if (kind === 1) core._cf_exit_to_menu();
    if (kind === 2) ++endCalls;
};
core._wasm_module_start();
core._wasm_module_frame();
core._wasm_module_frame();
assert.equal(endCalls, 1);
assert.equal(exits, 2);
console.log(JSON.stringify({ frames: 300, nonBlank, distinctFramebuffers: hashes.size, flushes,
    ledUpdates, toneStarts, importCounts: guest.importCounts, deferredExit: 'passed' }, null, 2));
core._wasm_stop();
