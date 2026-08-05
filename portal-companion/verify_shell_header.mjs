// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC
//
// Drift check for the shell embedded in the firmware image.
//
//   node verify_shell_header.mjs        (or: npm run verify)
//
// lib/WebPortalApp/companion_shell_gz.h is a GENERATED file that is tracked in
// git, so that the firmware builds with no JS toolchain installed. The cost of
// that choice is that it can go stale against portal-companion/src/. This
// decompresses the committed header and compares it byte-for-byte with the
// built pack's index.html.
//
// Needs a prior `npm run build` (it compares against dist/web/index.html).
//
// In CI, pair it with a git check, which additionally catches "edited src and
// forgot to regenerate":
//   npm run build && git diff --exit-code ../lib/WebPortalApp/companion_shell_gz.h

import { readFileSync, existsSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { gunzipSync } from 'node:zlib';

const root = dirname(fileURLToPath(import.meta.url));
const header = join(root, '..', 'lib', 'WebPortalApp', 'companion_shell_gz.h');
const built = join(root, 'dist', 'web', 'index.html');

const fail = (msg) => { console.error(`[verify] FAILED: ${msg}`); process.exit(1); };

if (!existsSync(header)) fail(`missing ${header} - run npm run build`);
if (!existsSync(built)) fail(`missing ${built} - run npm run build first`);

const src = readFileSync(header, 'utf8');
const open = src.indexOf('{', src.indexOf('PROGMEM'));
const close = src.lastIndexOf('}');
if (open < 0 || close < 0) fail('no PROGMEM byte array found in the header');

const bytes = Buffer.from(src.slice(open + 1, close)
  .split(',').map((t) => t.trim()).filter((t) => t.length)
  .map((t) => Number.parseInt(t, 16)));

let embedded;
try {
  embedded = gunzipSync(bytes);
} catch (e) {
  fail(`embedded shell is not a valid gzip stream: ${e.message}`);
}

const expected = readFileSync(built);
if (!embedded.equals(expected)) {
  fail(`embedded shell has drifted from the built pack\n` +
       `         embedded: ${embedded.length} B, built: ${expected.length} B\n` +
       `         regenerate with: npm run build`);
}

console.log(`[verify] embedded shell matches the built pack ` +
            `(${expected.length} B raw, ${bytes.length} B gzipped)`);
