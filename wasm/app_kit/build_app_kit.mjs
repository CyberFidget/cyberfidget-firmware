// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2026 Dismo Industries LLC
import fs from 'node:fs/promises';
import path from 'node:path';
import os from 'node:os';
import { fileURLToPath } from 'node:url';
import { execFileSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import { runClang } from '@yowasp/clang';
import { recipe } from './recipe.mjs';
import { fileTree, substitute } from './compile_recipe.mjs';
import { zip } from './deterministic_zip.mjs';

const here = path.dirname(fileURLToPath(import.meta.url)), root = path.resolve(here, '../..');
const args = process.argv.slice(2), options = {};
for (let i = 0; i < args.length; i += 2) {
    if (!['--out', '--core', '--tag', '--check'].includes(args[i]) || !args[i + 1] || args[i + 1].startsWith('--')) {
        throw new Error('Usage: node wasm/app_kit/build_app_kit.mjs --out <dir> [--core <dir>] [--tag <tag>] [--check <kit-dir>]');
    }
    if (args[i] in options) throw new Error('Duplicate option: ' + args[i]);
    options[args[i]] = args[i + 1];
}
if (!options['--out'] && !options['--check']) throw new Error('--out or --check is required');
if (!options['--core']) {
    try {
        execFileSync('cmake', ['--version'], { stdio: 'ignore' });
        execFileSync('ninja', ['--version'], { stdio: 'ignore' });
    } catch (cause) {
        throw new Error('Core build needs CMake and Ninja on PATH; alternatively supply --core from this checkout', { cause });
    }
}
const hash = bytes => createHash('sha256').update(bytes).digest('hex');
const metadata = bytes => ({ sha256: hash(bytes), size: bytes.length });
const json = value => Buffer.from(JSON.stringify(value, null, 2) + '\n');
// Git may check out text as CRLF on Windows. Canonicalize firmware text so
// platform checkout settings cannot change source, fixture or adapter bytes.
const readText = async file => (await fs.readFile(path.join(root, file), 'utf8')).replaceAll('\r\n', '\n');
const git = (...args) => execFileSync('git', ['-c', 'safe.directory=' + root.replaceAll('\\', '/'), '-C', root, ...args], { encoding: 'utf8' }).trim();
const commit = git('rev-parse', 'HEAD'), base = (await fs.readFile(path.join(root, 'version.txt'), 'utf8')).trim();
if (!/^\d+\.\d+\.\d+$/.test(base)) throw new Error('Malformed version.txt');
// --check inherits the recorded tag unless explicitly supplied. It never trusts
// the old manifest's hashes or compiler pin when rebuilding.
const checkDir = options['--check'] && path.resolve(options['--check']);
const tag = options['--tag'] ?? (checkDir ? JSON.parse(await fs.readFile(path.join(checkDir, 'manifest.json'), 'utf8')).tag : null);
if (tag !== null && (typeof tag !== 'string' ||
    new RegExp('^v' + base.replaceAll('.', '\\.') + '(?:-[A-Za-z0-9.-]+)?$').exec(tag)?.[0] !== tag)) {
    throw new Error('Tag must match version.txt (v' + base + ' or a prerelease)');
}
const shortCommit = git('rev-parse', '--short=7', 'HEAD');
const version = (tag ? tag.slice(1) : base) + '+' + shortCommit;
const compiler = JSON.parse(await fs.readFile(path.join(here, 'node_modules/@yowasp/clang/package.json'), 'utf8'));
const pin = JSON.parse(await fs.readFile(path.join(here, 'package.json'), 'utf8')).dependencies['@yowasp/clang'];
if (compiler.version !== pin) throw new Error('Installed compiler does not match the exact package pin');
const compilerFiles = {};
for (const name of (await fs.readdir(path.join(here, 'node_modules/@yowasp/clang/gen'))).sort()) {
    compilerFiles[name] = metadata(await fs.readFile(path.join(here, 'node_modules/@yowasp/clang/gen', name)));
}

const sources = {};
async function collect(dir, prefix) {
    for (const ent of (await fs.readdir(dir, { withFileTypes: true })).sort((a, b) => a.name < b.name ? -1 : a.name > b.name ? 1 : 0)) {
        if (ent.isSymbolicLink()) throw new Error('Source tree must not contain symlinks');
        if (ent.isDirectory()) await collect(path.join(dir, ent.name), prefix + '/' + ent.name);
        else if (/\.(h|hpp|cpp|inc|def)$/.test(ent.name)) sources[prefix + '/' + ent.name] = await readText(path.relative(root, path.join(dir, ent.name)));
    }
}
await collect(path.join(root, 'wasm/device_module/shims'), 'wasm/device_module/shims');
for (const name of ['cf_hal_imports.h', 'cf_hal_abi.h', 'cf_params.h', 'cf_imports.def']) {
    sources['wasm/device_module/' + name] = await readText('wasm/device_module/' + name);
}
await collect(path.join(root, 'lib/CFGraphics/include'), 'lib/CFGraphics/include');
sources[recipe.prelude.source] = '#include "' + recipe.force_include + '"\n';
const values = { prelude_source: recipe.prelude.source, prelude_expanded: recipe.prelude.expanded, prelude_output: recipe.prelude.output };
const output = { 'recipe.json': json(recipe) };
const quiet = { stdout: null, stderr: bytes => { if (bytes) process.stderr.write(bytes); } };
const command = step => {
    const files = Object.assign({}, ...step.inputs.map(input => {
        if (input === 'sources') return sources;
        if (input === 'prelude') return { [recipe.prelude.output]: output[recipe.prelude.output] };
        throw new Error('Unknown build input: ' + input);
    }));
    return runClang([recipe.driver, ...recipe.compile_flags, ...substitute(step.args, values)], fileTree(files), quiet);
};
console.log('Expanding force-include and building prelude');
const expanded = (await command(recipe.prelude.expand))[recipe.prelude.expanded];
sources[recipe.prelude.expanded] = typeof expanded === 'string' ? expanded : new TextDecoder().decode(expanded);
console.log('Building PCH from ' + Buffer.byteLength(sources[recipe.prelude.expanded]) + ' expanded bytes');
output[recipe.prelude.output] = (await command(recipe.prelude.build))[recipe.prelude.output];
for (const graphics of recipe.graphics) {
    console.log('Building ' + graphics.object);
    values.graphics_source = graphics.source; values.graphics_object = graphics.object;
    sources[graphics.source] = await readText(graphics.checkout_source);
    output[graphics.object] = (await command(recipe.graphics_build))[graphics.object];
}
output['sources.json'] = Buffer.from(JSON.stringify(sources));
// Package the two-file fixtures so the smoke test never reads checkout sources.
output['examples.json'] = json([
    { name: 'StopwatchDemo', instance: 'stopwatchDemo',
        header: await readText('wasm/device_module/example_app/StopwatchDemo.h'),
        source: await readText('wasm/device_module/example_app/StopwatchDemo.cpp') },
    { name: 'BreakoutGame', instance: 'breakoutGame',
        header: await readText('lib/BreakoutGame/BreakoutGame.h'),
        source: (await readText('lib/BreakoutGame/BreakoutMath.h')) + '\n' +
            (await readText('lib/BreakoutGame/BreakoutGame.cpp')).replace('#include "BreakoutMath.h"', '') },
]);
output['LICENSE'] = Buffer.from(await readText('LICENSE'));
output['LICENSES.md'] = Buffer.from(await readText('wasm/app_kit/LICENSES.md'));

let scratch, coreError;
try {
    let coreDir = options['--core'] && path.resolve(options['--core']);
    if (!coreDir) {
        scratch = await fs.mkdtemp(path.join(os.tmpdir(), 'cf-app-kit-'));
        coreDir = path.join(scratch, 'core');
        // A private force-include avoids writing wasm/generated/version.h and
        // excludes wall-clock timestamps / dirty-tree branding from kit builds.
        const [major, minor, patch] = base.split('.'), prerelease = tag ? tag.slice(base.length + 2) : '';
        const header = `#ifndef CYBERFIDGET_VERSION_H\n#define CYBERFIDGET_VERSION_H\n` +
            `#define FW_VERSION_MAJOR ${major}\n#define FW_VERSION_MINOR ${minor}\n#define FW_VERSION_PATCH ${patch}\n` +
            `#define VERSION_ENCODE(major, minor, patch) (((major) << 16) | ((minor) << 8) | (patch))\n` +
            `#define FW_VERSION VERSION_ENCODE(FW_VERSION_MAJOR, FW_VERSION_MINOR, FW_VERSION_PATCH)\n` +
            `#define FW_VERSION_PRERELEASE "${prerelease}"\n#define FW_VERSION_STRING "${tag ? tag.slice(1) : base}"\n` +
            `#define FW_GIT_HASH "${shortCommit}"\n#define FW_GIT_DIRTY 0\n` +
            `#define FW_BUILD_TIMESTAMP "${git('log', '-1', '--format=%cI')}"\n#define FW_BUILD_TYPE "wasm"\n` +
            `#define FW_VERSION_FULL_STRING "${version}"\n#endif\n`;
        await fs.writeFile(path.join(scratch, 'version.h'), header);
        const flags = `-include "${path.join(scratch, 'version.h').replaceAll('\\', '/')}" -I"${scratch.replaceAll('\\', '/')}"` +
            ` -ffile-prefix-map="${root.replaceAll('\\', '/')}"=. -ffile-prefix-map="${scratch.replaceAll('\\', '/')}"=.`;
        // Locate the activated SDK without invoking a batch wrapper or shell.
        const candidates = [process.env.EMSDK, process.env.EMSDK_ROOT].filter(Boolean)
            .map(dir => path.join(dir, 'upstream/emscripten'));
        candidates.push(...(process.env.PATH ?? '').split(path.delimiter));
        let toolchain;
        for (const dir of candidates) {
            const candidate = path.join(dir, 'cmake/Modules/Platform/Emscripten.cmake');
            try { await fs.access(candidate); toolchain = candidate; break; } catch { /* Try next SDK path. */ }
        }
        if (!toolchain) throw new Error('Activate Emscripten or set EMSDK/EMSDK_ROOT to locate its CMake toolchain');
        const run = (program, args) => execFileSync(program, args, { stdio: 'inherit' });
        // Initial-cache data preserves the force-include flags as CMake data.
        const cache = path.join(scratch, 'kit-cache.cmake');
        await fs.writeFile(cache, `set(CMAKE_CXX_FLAGS [=[${flags}]=] CACHE STRING "Kit version header" FORCE)\n`);
        run('cmake', ['-DCMAKE_TOOLCHAIN_FILE=' + toolchain.replaceAll('\\', '/'),
            '-S', path.join(root, 'wasm'), '-B', coreDir, '-G', 'Ninja',
            '-DCF_WASM_MODULE_HOST=ON', '-DCMAKE_BUILD_TYPE=Release', '-C', cache]);
        run('cmake', ['--build', coreDir]);
    }
    for (const name of ['cyberfidget-core.js', 'cyberfidget-core.wasm', 'cf-imports.json']) output[name] = await fs.readFile(path.join(coreDir, name));
} catch (error) {
    coreError = error; throw error;
} finally {
    if (scratch) {
        try {
            if (path.dirname(path.resolve(scratch)) !== path.resolve(os.tmpdir())) throw new Error('Unexpected scratch path');
            await fs.rm(scratch, { recursive: true, force: true });
        } catch (error) {
            if (!coreError) throw error;
            console.error('Core scratch cleanup failed; preserving original error:', error.message);
        }
    }
}
const manifest = {
    kit_format: 1, version, tag, commit,
    hal_abi: Number(sources['wasm/device_module/cf_hal_abi.h'].match(/#define\s+CF_HAL_ABI\s+(\d+)/)[1]),
    compiler: { package: compiler.name, version: compiler.version, files: compilerFiles },
    files: Object.fromEntries(Object.keys(output).sort().map(name => [name, metadata(output[name])])),
};
output['manifest.json'] = json(manifest);
const archive = zip(output);
async function inventory(dir, prefix = '') {
    const names = [];
    for (const ent of await fs.readdir(dir, { withFileTypes: true })) {
        if (ent.isSymbolicLink()) throw new Error('Kit must not contain symlinks');
        const name = prefix + ent.name;
        if (ent.isDirectory()) names.push(...await inventory(path.join(dir, ent.name), name + '/'));
        else names.push(name);
    }
    return names.sort();
}
if (checkDir) {
    if (JSON.stringify(await inventory(checkDir)) !== JSON.stringify(Object.keys(output).sort())) throw new Error('Kit file inventory differs');
    for (const name of Object.keys(output).sort()) {
        if (!Buffer.from(output[name]).equals(await fs.readFile(path.join(checkDir, name)))) throw new Error('Kit file differs: ' + name);
    }
    const parent = path.dirname(checkDir);
    if (!archive.equals(await fs.readFile(path.join(parent, 'cf-app-kit.zip')))) throw new Error('Kit zip differs');
    if (!output['manifest.json'].equals(await fs.readFile(path.join(parent, 'cf-app-kit.manifest.json')))) throw new Error('Sidecar manifest differs');
    console.log('Determinism check passed (all files, manifest and zip)');
} else {
    const out = path.resolve(options['--out']), kit = path.join(out, 'cf-app-kit');
    // Refuse to overwrite an existing kit: stale files cannot enter a release.
    await fs.mkdir(out, { recursive: true }); await fs.mkdir(kit);
    for (const name of Object.keys(output).sort()) await fs.writeFile(path.join(kit, name), output[name]);
    await fs.writeFile(path.join(out, 'cf-app-kit.zip'), archive);
    await fs.writeFile(path.join(out, 'cf-app-kit.manifest.json'), output['manifest.json']);
}
console.log(JSON.stringify({ version, commit, files: Object.keys(output).length,
    rawSize: Object.values(output).reduce((sum, b) => sum + b.length, 0), zipSize: archive.length,
    manifestSha256: hash(output['manifest.json']), zipSha256: hash(archive) }, null, 2));
