// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2026 Dismo Industries LLC
import fs from 'node:fs';
import { pathToFileURL } from 'node:url';

export function readManifest(text) {
    const rows = JSON.parse(text).filter(Boolean);
    const keys = new Set();
    const type = t => ({ void: 'v', float: 'f', double: 'F', int64_t: 'I', uint64_t: 'I' }[t] ?? 'i');
    for (const row of rows) {
        const key = `${row.module}.${row.name}`;
        if (keys.has(key)) throw new Error(`Duplicate interface entry: ${key}`);
        keys.add(key);
        if (!Number.isInteger(row.since) || row.since < 1) throw new Error(`Invalid since level: ${key}`);
        if (row.parameters) {
            const sig = `${type(row.returnType)}(${row.parameters.map(p => p.pointer ? 'i' : type(p.type)).join('')})`;
            if (sig !== row.signature) throw new Error(`Interface C/wasm signature mismatch: ${key}`);
        }
    }
    return rows;
}

// Read the binary type/import sections, rather than depending on WAT formatting.
export function moduleImports(bytes) {
    let pos = 8;
    const byte = () => {
        if (pos >= bytes.length) throw new Error('Truncated wasm');
        return bytes[pos++];
    };
    const uleb = () => {
        let n = 0, shift = 0;
        for (let i = 0; i < 5; ++i) {
            const b = byte(); n += (b & 127) * 2 ** shift;
            if (!(b & 128)) return n;
            shift += 7;
        }
        throw new Error('Invalid wasm integer');
    };
    const str = () => {
        const len = uleb(), end = pos + len;
        if (end > bytes.length) throw new Error('Truncated wasm name');
        const value = new TextDecoder().decode(bytes.subarray(pos, end)); pos = end; return value;
    };
    const types = [], imports = [];
    const valueType = () => {
        const t = { 0x7f: 'i', 0x7e: 'I', 0x7d: 'f', 0x7c: 'F' }[byte()];
        if (!t) throw new Error('Unsupported import value type');
        return t;
    };
    if (!WebAssembly.validate(bytes)) throw new Error('Invalid wasm module');
    while (pos < bytes.length) {
        const id = byte(), size = uleb(), end = pos + size;
        if (end > bytes.length) throw new Error('Truncated wasm section');
        if (id === 1) {
            const count = uleb();
            for (let i = 0; i < count; ++i) {
                if (byte() !== 0x60) throw new Error('Unsupported wasm type');
                const params = Array.from({ length: uleb() }, valueType);
                const results = Array.from({ length: uleb() }, valueType);
                types.push(`${results.join('') || 'v'}(${params.join('')})`);
            }
        } else if (id === 2) {
            const count = uleb();
            for (let i = 0; i < count; ++i) {
                const module = str(), name = str(), kind = byte();
                if (kind !== 0) throw new Error(`Unsupported non-function import: ${module}.${name}`);
                const signature = types[uleb()];
                if (!signature) throw new Error(`Missing type for ${module}.${name}`);
                imports.push({ module, name, signature });
            }
        }
        pos = end;
    }
    return imports;
}

export function verifyImports(bytes, rows) {
    const allowed = new Map(rows.map(r => [`${r.module}.${r.name}`, r.signature]));
    for (const row of moduleImports(bytes)) {
        const key = `${row.module}.${row.name}`, expected = allowed.get(key);
        if (!expected) throw new Error(`Unknown device import: ${key}`);
        if (expected !== row.signature) throw new Error(`Wrong signature for ${key}: ${row.signature}, expected ${expected}`);
    }
}

export function requiredHalAbi(bytes, rows) {
    verifyImports(bytes, rows);
    const levels = new Map(rows.map(r => [`${r.module}.${r.name}`, r.since]));
    return Math.max(1, ...moduleImports(bytes).map(r => levels.get(`${r.module}.${r.name}`)));
}

if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
    try {
        const rows = readManifest(fs.readFileSync(0, 'utf8'));
        const level = requiredHalAbi(fs.readFileSync(process.argv[2]), rows);
        console.log(process.argv.includes('--hal-abi') ? level : 'Device WASM import names and signatures: allowed');
    } catch (error) {
        console.error(`::error::${error.message}`);
        process.exitCode = 1;
    }
}
