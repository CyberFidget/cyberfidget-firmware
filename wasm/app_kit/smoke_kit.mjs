// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2026 Dismo Industries LLC
import fs from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import Module from 'node:module';
import { createHash } from 'node:crypto';
import assert from 'node:assert/strict';

const args = process.argv.slice(2), kit = args[0] && path.resolve(args[0]), options = {};
for (let i = 1; i < args.length; i += 2) {
    if (!['--out', '--reference'].includes(args[i]) || !args[i + 1]) throw new Error('Unknown smoke option');
    options[args[i]] = args[i + 1];
}
if (!kit) throw new Error('Usage: node wasm/app_kit/smoke_kit.mjs <kit-dir> [--out <module-dir>] [--reference <compiler-options.mjs>]');
const hash = bytes => createHash('sha256').update(bytes).digest('hex');
const read = name => fs.readFile(path.join(kit, name));
const manifest = JSON.parse(await read('manifest.json'));
assert.equal(manifest.kit_format, 1);
const bytes = {};
// Verify all kit inputs before executing the core or using compile inputs.
for (const [name, expected] of Object.entries(manifest.files)) {
    assert.ok(!path.isAbsolute(name) && !name.split(/[\\/]/).includes('..'), 'Invalid manifest path');
    bytes[name] = await read(name);
    assert.equal(bytes[name].length, expected.size, name + ' size');
    assert.equal(hash(bytes[name]), expected.sha256, name + ' hash');
}
const here = path.dirname(fileURLToPath(import.meta.url));
const compilerDir = path.join(here, 'node_modules/@yowasp/clang');
const pkg = JSON.parse(await fs.readFile(path.join(compilerDir, 'package.json'), 'utf8'));
const pin = JSON.parse(await fs.readFile(path.join(here, 'package.json'), 'utf8')).dependencies['@yowasp/clang'];
assert.equal(manifest.compiler.package, '@yowasp/clang');
assert.equal(manifest.compiler.version, pin); assert.equal(pkg.version, pin);
assert.deepEqual((await fs.readdir(path.join(compilerDir, 'gen'))).sort(), Object.keys(manifest.compiler.files).sort());
for (const [name, expected] of Object.entries(manifest.compiler.files)) {
    assert.equal(path.basename(name), name);
    const file = await fs.readFile(path.join(compilerDir, 'gen', name));
    assert.equal(file.length, expected.size, name + ' compiler size');
    assert.equal(hash(file), expected.sha256, name + ' compiler hash');
}
const { runClang } = await import('@yowasp/clang');
const { compileApp } = await import('./compile_recipe.mjs');
const { attachGuest, framebuffer } = await import('../module_host/host.mjs');
const { readManifest, verifyImports } = await import('../device_module/verify_imports.mjs');
const recipe = JSON.parse(bytes['recipe.json']), sources = JSON.parse(bytes['sources.json']);
assert.equal(manifest.hal_abi, Number(sources['wasm/device_module/cf_hal_abi.h'].match(/#define\s+CF_HAL_ABI\s+(\d+)/)[1]));
const inputs = { sources, prelude: bytes[recipe.prelude.output],
    objects: Object.fromEntries(recipe.graphics.map(g => [g.object, bytes[g.object]])) };
const table = readManifest(bytes['cf-imports.json'].toString());
const examples = JSON.parse(bytes['examples.json']);
assert.deepEqual(examples.map(e => e.name), ['StopwatchDemo', 'BreakoutGame']);
let reference;
if (options['--reference']) {
    // The supplied reference snapshot omits its diagnostic formatter. Stub only
    // that import in memory; its compileApp, flags and commands stay verbatim.
    const code = (await fs.readFile(options['--reference'], 'utf8'))
        .replace("import { compileDiagnostics } from './diagnostics.mjs';", 'const compileDiagnostics = text => text;');
    reference = await import('data:text/javascript;base64,' + Buffer.from(code).toString('base64'));
}
if (options['--out']) await fs.mkdir(path.resolve(options['--out']), { recursive: true });
// Node has no audio device. Exercise the bridge without claiming audible parity.
class AudioContext {
    currentTime = 0; destination = {};
    createOscillator() { return { frequency: { setValueAtTime() {} }, connect() {}, start() {}, stop() {} }; }
    createGain() { return { gain: { setValueAtTime() {} }, connect() {} }; }
}
globalThis.window = { AudioContext };
// Load verified CommonJS bytes explicitly, independent of the output parent's
// package type. The kit does not carry a Node package or repository tools.
const corePath = path.join(kit, 'cyberfidget-core.js');
const coreModule = new Module(corePath);
coreModule.filename = corePath;
coreModule.paths = Module._nodeModulePaths(kit);
coreModule._compile(bytes['cyberfidget-core.js'].toString(), corePath);
const factory = coreModule.exports;
const results = [];
for (const app of examples) {
    console.log('Compiling ' + app.name);
    const first = await compileApp(runClang, recipe, inputs, app);
    const warm = await compileApp(runClang, recipe, inputs, app);
    assert.deepEqual(warm.wasm, first.wasm, 'Repeated app build must be byte-identical');
    verifyImports(warm.wasm, table);
    console.log(app.name + ': verify_imports.mjs passed');
    if (reference && app.name === 'StopwatchDemo') {
        const ref = await reference.compileApp(runClang, inputs, app);
        assert.deepEqual(ref.wasm, warm.wasm, 'Reference compileApp must equal the kit recipe');
        console.log('Stopwatch recipe equivalence: byte-identical');
    }
    if (options['--out']) await fs.writeFile(path.join(path.resolve(options['--out']), app.name + '.wasm'), warm.wasm);
    let flushes = 0, exits = 0;
    const core = await factory({ wasmBinary: bytes['cyberfidget-core.wasm'],
        onFrameReady: () => ++flushes, onAppExit: () => ++exits });
    try {
        await attachGuest(core, warm.wasm, table);
        core._wasm_module_start();
        const hashes = new Set(); let nonBlank = 0;
        for (let frame = 0; frame < 300; ++frame) {
            // Enter starts Stopwatch and selects Breakout's input mode. Held
            // long enough for the real button debounce. No synthetic clock.
            if (frame === 2) core._wasm_button_press(5);
            if (frame === 7) core._wasm_button_release(5);
            if (frame % 50 === 15) core._wasm_button_press(3);
            if (frame % 50 === 20) core._wasm_button_release(3);
            core._wasm_set_slider((frame * 31) % 4096);
            core._wasm_set_accel(Math.sin(frame / 10), Math.cos(frame / 10), 1);
            core._wasm_module_frame();
            const pixels = framebuffer(core);
            if (pixels.some(Boolean)) ++nonBlank;
            hashes.add(hash(pixels));
            await new Promise(resolve => setTimeout(resolve, 20));
        }
        assert.equal(nonBlank, 300, app.name + ' must render every frame');
        assert.ok(hashes.size > 1, app.name + ' framebuffer must change');
        assert.ok(flushes >= 300); assert.equal(exits, 0, 'Unexpected guest exit');
        core._wasm_module_end();
        assert.equal(exits, 1);
        results.push({ app: app.name, sha256: hash(warm.wasm), size: warm.wasm.length,
            warmTimings: warm.timings, frames: 300, nonBlank, distinctFramebuffers: hashes.size,
            imports: 'passed', recipeEquivalence: reference && app.name === 'StopwatchDemo' ? 'passed' : 'not requested' });
    } finally { core._wasm_stop(); }
}
console.log(JSON.stringify(results, null, 2));
