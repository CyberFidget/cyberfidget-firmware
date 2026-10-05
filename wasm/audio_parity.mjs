// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Dismo Industries LLC
//
// Audio parity check for emulator builds: the emulator renders the same
// samples as the device's audio engine for the same commands.
//
//   node wasm/audio_parity.mjs <core.js> [<core.js> ...]
//   e.g. node wasm/audio_parity.mjs wasm/build/cyberfidget.js wasm/build/module-host/cyberfidget-core.js
//
// For every build it checks:
//   1. the engine golden script (test/test_audio_engine_core/golden_script.h)
//      renders the native test's checksum (kGoldenChecksum);
//   2. the AudioManager tone script (tone_script.h) through the emulator's
//      real AudioManager and render path matches the native model of the
//      device glue (kToneScriptChecksum);
//   3. the render contract basics (AUDIO_RENDER_CONTRACT.md): rate, buffer,
//      reset silences, and - in a module-host core, which exports the cf_*
//      calls - that a tone sounds, ends on time, and that the auto clock
//      keeps time when nothing pulls samples.
// Node has no audio device: this proves the samples, not what a browser plays.
import fs from 'node:fs';
import path from 'node:path';
import assert from 'node:assert/strict';
import { createRequire } from 'node:module';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const testDir = path.join(here, '..', 'test', 'test_audio_engine_core');
function headerConstant(file, name) {
    const text = fs.readFileSync(path.join(testDir, file), 'utf8');
    const m = text.match(new RegExp(`${name}\\s*=\\s*(0x[0-9A-Fa-f]+)u?;`));
    if (!m) throw new Error(`${name} not found in ${file}`);
    return Number.parseInt(m[1], 16) >>> 0;
}
const kGolden = headerConstant('golden_script.h', 'kGoldenChecksum');
const kToneScript = headerConstant('tone_script.h', 'kToneScriptChecksum');
const hex = v => '0x' + (v >>> 0).toString(16).toUpperCase().padStart(8, '0');

const cores = process.argv.slice(2);
if (!cores.length) throw new Error('Usage: node audio_parity.mjs <core.js> [<core.js> ...]');

function peak(core, frames) {
    const n = core._wasm_audio_render(frames);
    const view = new Int16Array(core.HEAP16.buffer, core._wasm_audio_buffer(), n);
    let p = 0;
    for (const s of view) p = Math.max(p, Math.abs(s));
    return p;
}
function renderMs(core, ms) {   // peak over `ms` of sound, in 256-frame blocks
    let p = 0;
    for (let left = Math.round(ms * 44.1); left > 0; left -= 256) p = Math.max(p, peak(core, Math.min(256, left)));
    return p;
}
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));

let failed = false;
for (const corePath of cores) {
    const factory = createRequire(import.meta.url)(path.resolve(corePath));
    const core = await factory({ wasmBinary: fs.readFileSync(corePath.replace(/\.js$/, '.wasm')) });
    const result = { core: corePath };
    try {
        assert.equal(core._wasm_audio_sample_rate(), 44100);
        assert.ok(core._wasm_audio_buffer_frames() >= 1024);

        const golden = core._wasm_audio_selftest_golden() >>> 0;
        result.golden = hex(golden);
        assert.equal(hex(golden), hex(kGolden), 'engine golden script');

        const tone = core._wasm_audio_selftest_tone_script() >>> 0;
        result.toneScript = hex(tone);
        assert.equal(hex(tone), hex(kToneScript), 'AudioManager tone script');

        core._wasm_audio_reset();
        assert.equal(peak(core, 1024), 0, 'silent after reset');
        assert.equal(core._wasm_audio_render(100000), core._wasm_audio_buffer_frames(), 'render clamps to the buffer');

        if (typeof core._cf_tone_play === 'function') {
            // A timed tone sounds, then ends on its own (100 ms + 5 ms release).
            core._cf_tone_play(1000, 100);
            assert.ok(renderMs(core, 90) > 8000, 'tone sounds');
            renderMs(core, 30);
            assert.equal(core._wasm_audio_active(), 0, 'timed tone ended');
            assert.equal(peak(core, 512), 0, 'silent after the tone');
            // Reset silences a held tone at once, and it does not come back.
            core._cf_tone_play(440, 0);
            assert.ok(renderMs(core, 20) > 8000, 'held tone sounds');
            core._wasm_audio_reset();
            assert.equal(peak(core, 1024), 0, 'reset silences a held tone');
            // Auto clock: with nothing pulling samples, frames keep time.
            core._wasm_audio_set_autoclock(1);
            core._wasm_module_frame();   // starts the clock
            core._cf_tone_play(1000, 30);
            core._wasm_module_frame();
            assert.equal(core._wasm_audio_active(), 1, 'tone running on the auto clock');
            await sleep(80);
            core._wasm_module_frame();
            assert.equal(core._wasm_audio_active(), 0, 'auto clock ended the tone');
            result.renderContract = 'tone, auto-stop, reset, auto clock';
        } else {
            result.renderContract = 'rate, buffer, reset';
        }
        result.ok = true;
    } catch (e) {
        failed = true;
        result.ok = false;
        result.error = e.message;
    }
    if (typeof core._wasm_stop === 'function') core._wasm_stop();
    console.log(JSON.stringify(result));
}
console.log(failed ? 'AUDIO PARITY: FAIL' : `AUDIO PARITY: PASS (golden ${hex(kGolden)}, tone script ${hex(kToneScript)})`);
process.exitCode = failed ? 1 : 0;
