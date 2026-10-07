// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2026 Dismo Industries LLC
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
const root = fileURLToPath(new URL('../../', import.meta.url));
const scratch = fs.mkdtempSync(path.join(os.tmpdir(), 'cf-contract-'));
const cases = [
    ['unknown-cf', 'cf', 'unknown_import', '(param i32) (result i32)', '(drop (call $host (i32.const 0)))', /Unknown device import/],
    ['wrong-cf-signature', 'cf', 'nop', '(param f32) (result i32)', '(drop (call $host (f32.const 0)))', /Wrong signature/],
    ['unknown-env', 'env', 'unknown_stub', '', '(call $host)', /Unknown device import/],
    ['wrong-wasi-signature', 'wasi_snapshot_preview1', 'fd_close', '(param f32) (result i32)', '(drop (call $host (f32.const 0)))', /Wrong signature/],
];
for (const [name, module, imported, signature, call, error] of cases) {
    test(name, () => {
        const wat = path.join(scratch, `${name}.wat`), wasm = path.join(scratch, `${name}.wasm`);
        fs.writeFileSync(wat, `(module
          (import "${module}" "${imported}" (func $host ${signature}))
          (memory (export "memory") 1)
          (func (export "app_begin") ${call})
          (func (export "app_update"))
          (func (export "app_end"))
          (func (export "app_handle_button") (param i32 i32)))`);
        const build = spawnSync(process.env.CF_WASM_AS || 'wasm-as', [wat, '-o', wasm], { encoding: 'utf8' });
        assert.equal(build.status, 0, `wasm-as failed: ${build.error || build.stderr}`);
        const result = spawnSync('bash', [path.join(root, 'wasm/device_module/verify_device_contract.sh'), wasm], { encoding: 'utf8' });
        assert.equal(result.status, 1, result.stdout + result.stderr);
        assert.match(result.stdout + result.stderr, error);
    });
}
// Keep artifacts on failure for diagnosis; the caller owns its scratch directory.
