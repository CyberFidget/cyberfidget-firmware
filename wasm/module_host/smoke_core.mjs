// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2026 Dismo Industries LLC
import fs from 'node:fs';
import path from 'node:path';
import assert from 'node:assert/strict';
import { createRequire } from 'node:module';
import { createHash } from 'node:crypto';
import { attachGuest, framebuffer } from './host.mjs';
import { readManifest, verifyImports, requiredHalAbi } from '../device_module/verify_imports.mjs';
import { fileURLToPath } from 'node:url';

const [corePath, guestPath, manifestPath] = process.argv.slice(2);
if (!manifestPath) throw new Error('Usage: node smoke_core.mjs <core.js> <guest.wasm> <cf-imports.json>');
const manifest = readManifest(fs.readFileSync(manifestPath, 'utf8'));
const bytes = fs.readFileSync(guestPath);
verifyImports(bytes, manifest);
// Node has no audio device: audio is checked as rendered samples
// (wasm_audio_render), not as browser playback. See wasm/audio_parity.mjs.
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
steps.setFloat32(stepsPtr, 440, true); steps.setUint16(stepsPtr + 4, 30, true);
steps.setFloat32(stepsPtr + 8, 660, true); steps.setUint16(stepsPtr + 12, 30, true);
guest.imports.cf.seq_play(stepsPtr, 1000); // Validate/copy only the clamped 64 steps.
new Uint8Array(memory.buffer, stepsPtr, 512).fill(0);
core.HEAPU8.fill(0, core._wasm_module_buffer(), core._wasm_module_buffer() + 512);
// Steps 440 Hz then 660 Hz, 30 ms (1323 samples) each, from the first
// rendered sample. Each must sound at its own pitch in its own window: a
// zeroed or partly zeroed copy renders rests there. Zero crossings over 1000
// samples: 440 Hz ~ 20, 660 Hz ~ 30.
const seq = new Int16Array(3072);
for (let off = 0; off < seq.length; off += 256) {
    const n = core._wasm_audio_render(256);
    seq.set(new Int16Array(core.HEAP16.buffer, core._wasm_audio_buffer(), n), off);
}
function stepWindow(from) {
    const w = seq.subarray(from, from + 1000);
    let peak = 0, crossings = 0;
    for (let i = 0; i < w.length; ++i) {
        peak = Math.max(peak, Math.abs(w[i]));
        if (i && (w[i - 1] < 0) !== (w[i] < 0)) ++crossings;
    }
    return { peak, crossings };
}
const seqSteps = [stepWindow(250), stepWindow(1323 + 250)];
assert.ok(seqSteps[0].peak > 2000 && Math.abs(seqSteps[0].crossings - 20) <= 2, 'Sequence step 1 (440 Hz) must play: ' + JSON.stringify(seqSteps[0]));
assert.ok(seqSteps[1].peak > 2000 && Math.abs(seqSteps[1].crossings - 30) <= 2, 'Sequence step 2 (660 Hz) must play: ' + JSON.stringify(seqSteps[1]));
core._wasm_audio_set_autoclock(1);   // nothing pulls samples from here on
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
const fixture = name => fs.readFileSync(fileURLToPath(new URL(`../../test/bench/fixtures/${name}.wasm`, import.meta.url)));
const audioBytes = fixture('audio_levels');
assert.equal(requiredHalAbi(bytes, manifest), 1, 'Existing fixture needs only level 1');
assert.equal(requiredHalAbi(audioBytes, manifest), 2, 'Note/mic fixture needs level 2');
const audioGuest = await attachGuest(core, audioBytes, manifest);
// Peak of `ms` of rendered audio (rendering also hands the clock back to us).
const renderPeak = ms => {
    let peak = 0;
    for (let left = Math.round(ms * 44.1); left > 0; left -= 256) {
        const n = core._wasm_audio_render(Math.min(256, left));
        for (const v of new Int16Array(core.HEAP16.buffer, core._wasm_audio_buffer(), n)) peak = Math.max(peak, Math.abs(v));
    }
    return peak;
};
core._wasm_audio_reset();
core._wasm_module_start();
assert.ok(audioGuest.instance.exports.test_note_handle() > 0);
assert.equal(audioGuest.instance.exports.test_mic_level(), 0, 'Emulator mic is an honest stub');
assert.equal(audioGuest.instance.exports.test_mic_db(), -60);
const notePeak = renderPeak(30);
assert.ok(notePeak > 2000, 'Guest note sounds: peak ' + notePeak);
assert.equal(audioGuest.importCounts.note_play, 2);
assert.equal(audioGuest.importCounts.note_stop, 1);
assert.equal(audioGuest.importCounts.note_all_off, 1);
assert.equal(audioGuest.importCounts.mic_enable, 1);
assert.equal(audioGuest.importCounts.mic_level, 1);
assert.equal(audioGuest.importCounts.mic_level_db, 1);
assert.ok(renderPeak(20) > 2000, 'Guest leaves a held note playing');
core._wasm_module_end();
renderPeak(10);   // the 5 ms end-of-app fade
assert.equal(renderPeak(20), 0, 'App end silences held notes');
assert.equal(core._wasm_audio_active(), 0, 'Nothing left sounding or queued');
await assert.rejects(attachGuest(core, fixture('unknown_import'), manifest), error => {
    assert.equal(error.name, 'CfImportUnsupported');
    assert.deepEqual(error.missing, ['cf.made_up']);
    return true;
});
console.log(JSON.stringify({ frames: 300, nonBlank, distinctFramebuffers: hashes.size, flushes,
    ledUpdates, seqSteps, importCounts: guest.importCounts, deferredExit: 'passed',
    notePeak, audioLevel: 2, noteCleanup: 'passed', unknownImport: 'passed' }, null, 2));
core._wasm_stop();
