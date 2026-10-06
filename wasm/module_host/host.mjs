// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2026 Dismo Industries LLC
// Browser-compatible adapter. Pass the preprocessed cf-imports.json table.
export async function attachGuest(core, bytes, manifest) {
    const rows = manifest.filter(Boolean);
    const module = await WebAssembly.compile(bytes);
    const allowed = new Set(rows.map(row => `${row.module}.${row.name}`));
    const missing = WebAssembly.Module.imports(module)
        .filter(row => ['cf', 'wasi_snapshot_preview1', 'env'].includes(row.module))
        .map(row => `${row.module}.${row.name}`).filter(key => !allowed.has(key));
    if (missing.length) {
        const error = new Error(`Unsupported app imports: ${missing.join(', ')}`);
        error.name = 'CfImportUnsupported';
        error.missing = missing;
        throw error;
    }
    let memory;
    const importCounts = {};
    const range = (ptr, len) => {
        ptr >>>= 0; len >>>= 0;
        if (!memory || ptr + len > memory.buffer.byteLength) {
            throw new WebAssembly.RuntimeError(`Guest memory range ${ptr}+${len} is invalid`);
        }
        // Always construct a fresh view: memory.grow detaches previous views.
        return new Uint8Array(memory.buffer, ptr, len);
    };
    const transfer = (ptr, len, cap = len) => {
        const source = range(ptr, len), n = Math.min(len, cap);
        if (n > core._wasm_module_buffer_size()) throw new Error('Core transfer buffer overflow');
        const target = core._wasm_module_buffer();
        core.HEAPU8.set(source.subarray(0, n), target);
        return target;
    };
    const imports = {};
    const stubs = {
        proc_exit: code => { throw new WebAssembly.RuntimeError(`proc_exit(${code})`); },
        fd_write: () => 8, fd_close: () => 8, fd_seek: () => 8,
        environ_sizes_get: (count, size) => { range(count, 4).fill(0); range(size, 4).fill(0); return 0; },
        environ_get: () => 0,
        clock_time_get: (_id, _precision, out) => {
            range(out, 8);
            new DataView(memory.buffer).setBigUint64(out >>> 0, BigInt(core._cf_millis() >>> 0) * 1000000n, true);
            return 0;
        },
        random_get: (ptr, len) => {
            const buffer = range(ptr, len);
            for (let i = 0; i < buffer.length; ++i) buffer[i] = core._cf_random(0, 256);
            return 0;
        },
        emscripten_notify_memory_growth() {},
    };
    for (const row of rows) {
        const namespace = imports[row.module] ??= {};
        if (row.module !== 'cf') {
            if (!stubs[row.name]) throw new Error(`Missing stub behavior: ${row.module}.${row.name}`);
            namespace[row.name] = stubs[row.name];
            continue;
        }
        const fn = core[`_cf_${row.name}`];
        if (typeof fn !== 'function') throw new Error(`Missing core export: cf_${row.name}`);
        const pointer = row.parameters.findIndex(p => p.pointer);
        namespace[row.name] = (...args) => {
            importCounts[row.name] = (importCounts[row.name] ?? 0) + 1;
            if (pointer >= 0) {
                if (row.policy === 'XBM') {
                    const w = args[row.parameters.findIndex(p => p.name === 'w')];
                    const h = args[row.parameters.findIndex(p => p.name === 'h')];
                    if (w <= 0 || h <= 0 || w > 128 || h > 64) return;
                    const len = Math.ceil(w / 8) * h;
                    args[pointer] = transfer(args[pointer], len);
                    args.push(len); // Explicit core-memory byte length (guest signature stays unchanged).
                } else if (row.policy === 'SEQUENCE') {
                    const count = args[pointer + 1] = Math.max(0, Math.min(args[pointer + 1], 64));
                    args[pointer] = transfer(args[pointer], count * 8);
                } else if (row.policy === 'STRING95' || row.policy === 'STRING127') {
                    const len = args[pointer + 1] >>> 0;
                    const cap = row.policy === 'STRING95' ? 95 : 127;
                    args[pointer] = transfer(args[pointer], len, cap);
                    args[pointer + 1] = Math.min(len, cap);
                } else {
                    throw new Error(`Unknown pointer policy: ${row.policy}`);
                }
            }
            return fn(...args);
        };
    }
    const instance = await WebAssembly.instantiate(module, imports);
    memory = instance.exports.memory;
    if (!(memory instanceof WebAssembly.Memory)) throw new Error('Guest must export memory');
    const ex = instance.exports;
    for (const name of ['app_begin', 'app_update', 'app_end', 'app_handle_button']) {
        if (typeof ex[name] !== 'function') throw new Error(`Missing guest export: ${name}`);
    }
    ex._initialize?.();
    core.guestCall = (kind, index, event) => {
        if (kind === 0) ex.app_begin();
        else if (kind === 1) ex.app_update();
        else if (kind === 2) ex.app_end();
        else ex.app_handle_button(index, event);
    };
    return { instance, imports, importCounts };
}

export function framebuffer(core) {
    const ptr = core._wasm_get_framebuffer();
    return core.HEAPU8.slice(ptr, ptr + core._wasm_get_framebuffer_size());
}
