// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC
//
// Assemble the companion "SD pack": what the device serves at /web/.
//
//   npm install          (once - pulls esbuild + the vendored libraries)
//   node build_sdpack.mjs
//
// CRITICAL design constraint (learned on hardware): the device's async web
// server serves the SD card over a single
// SPI stream and WEDGES under concurrent reads. A browser loading a
// multi-file page fires the CSS + JS-module graph in parallel and hangs the
// server. So the shell is bundled into ONE self-contained index.html
// (inlined CSS + JS, no separate module/manifest/sw fetches) -> page load is
// a single request. The only other files are the on-demand transcription
// vendor library, fetched only when the user opts into captions.
//
// Output dist/web/:
//   index.html            (self-contained: live listening works with ONLY this)
//   vendor/transformers.min.js, vendor/ort/*   (transcription, on demand)
//
// transformers.js + onnxruntime-web are copied out of node_modules at build
// time so multi-megabyte binaries never live in git.

import { build } from 'esbuild';
import { cpSync, mkdirSync, rmSync, existsSync, readdirSync, copyFileSync, writeFileSync, readFileSync, statSync, unlinkSync } from 'node:fs';
import { join, dirname, relative } from 'node:path';
import { fileURLToPath } from 'node:url';
import { gzipSync, gunzipSync } from 'node:zlib';
import { lf } from './sync_chrome.mjs';

const root = dirname(fileURLToPath(import.meta.url));
const src = join(root, 'src');
const out = join(root, 'dist', 'web');
// The gzipped shell is embedded in the firmware image so live listening works
// with no memory card at all. Written into lib/ next to the other page headers.
const shellHeader = join(root, '..', 'lib', 'WebPortalApp', 'companion_shell_gz.h');

// Node's gzip writes MTIME as 0, but the OS byte (RFC 1952 header offset 9) is
// NOT fixed - zlib stamps the platform it ran on, 0x0a from Windows and 0x03
// from the Linux CI runner. Identical input, one byte of difference, and a
// byte-for-byte drift gate that no amount of regenerating can satisfy. Stamp
// 0xff ("unknown") instead: the field is advisory, every decompressor ignores
// it, and the whole point of a tracked generated artifact is that it describes
// its source and not the machine that happened to build it.
//
// This is what lets the generated header be a tracked file that does not churn
// on every build (and lets CI detect drift with a plain `git diff --exit-code`).
const GZIP_OS_UNKNOWN = 0xff;
const gz = (buf) => {
  const out = gzipSync(buf, { level: 9 });
  out[9] = GZIP_OS_UNKNOWN;
  return out;
};

// Every text input to the shell goes through here. The sources are checked out
// CRLF on Windows (core.autocrlf=true) and LF on the Linux CI runner, so
// reading them raw makes the inlined HTML - and therefore the gzip stream and
// every byte of the generated header - differ by platform, and the drift gate
// can never pass from a Windows checkout. The bundled JS is immune (esbuild
// minifies the newlines away) and the shared kit is immune (sync_chrome.mjs
// normalises the same way, which is why portal_page.h never drifted); this is
// the half that was missing.
const readText = (path) => lf(readFileSync(path, 'utf8'));

// Emit a byte array as a C header. Not a string literal: the gzip stream
// contains NULs, so it has to be uint8_t[] with an explicit sizeof().
function writeByteHeader(path, symbol, bytes, note) {
  const lines = [];
  for (let i = 0; i < bytes.length; i += 16) {
    lines.push('  ' + [...bytes.subarray(i, i + 16)]
      .map((b) => '0x' + b.toString(16).padStart(2, '0')).join(','));
  }
  writeFileSync(path,
    '// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception\n' +
    '// Copyright (c) 2023-2026 Dismo Industries LLC\n' +
    '//\n' +
    '// GENERATED FILE - DO NOT EDIT.\n' +
    '// Regenerate with:  cd portal-companion && npm run build\n' +
    '//\n' +
    `// ${note}\n` +
    '//\n' +
    '// Tracked in git on purpose: the firmware must build without a JS\n' +
    '// toolchain installed. The cost of that choice is drift, so CI rebuilds\n' +
    '// the pack and fails if this file changes.\n' +
    '\n#pragma once\n\n#include <stdint.h>\n\n' +
    `const uint8_t ${symbol}[] PROGMEM = {\n${lines.join(',\n')}\n};\n`);
}

function findPkg(name) {
  const candidates = [
    join(root, 'node_modules', name),
    join(root, 'node_modules', '@huggingface', 'transformers', 'node_modules', name),
  ];
  for (const c of candidates) if (existsSync(c)) return c;
  return null;
}

console.log('[sdpack] cleaning', out);
rmSync(join(root, 'dist'), { recursive: true, force: true });
mkdirSync(out, { recursive: true });

// --- 1. Bundle the JS module graph into one blob ---
// The runtime transcription library is a dynamic import of a device URL
// (/web/vendor/...), NOT a build-time module - mark it external so esbuild
// leaves the import in place to resolve against the page origin at runtime.
console.log('[sdpack] bundling app.js');
const bundle = await build({
  entryPoints: [join(src, 'js', 'app.js')],
  bundle: true,
  format: 'esm',
  minify: true,
  write: false,
  external: ['/web/vendor/transformers.min.js'],
  legalComments: 'none',
});
const js = bundle.outputFiles[0].text;

// --- 1b. Bundle the inference worker as a SEPARATE on-demand file ---
// It's a module worker fetched only when captions/transcription start, so the
// page-load stays a single request. Keeps the heavy model + inference off the
// main thread (smooth waveform / responsive UI during transcription).
console.log('[sdpack] bundling engine.worker.js');
const workerBundle = await build({
  entryPoints: [join(src, 'js', 'engine.worker.js')],
  bundle: true,
  format: 'esm',
  minify: true,
  write: false,
  external: ['/web/vendor/transformers.min.js'],
  legalComments: 'none',
});
const workerJs = workerBundle.outputFiles[0].text;

// --- 1c. The shared kit's classic script ---
// Generated by sync_chrome.mjs from src/shared/ and written into the portal's
// PROGMEM literal too, so the two documents carry the same navigation, player,
// gate and download code. Deliberately NOT part of the module bundle: the portal
// has a plain <script> and the kit has to be valid in both.
const kitJs = readText(join(src, 'js', 'kit.gen.js'));

// --- 2. Inline CSS ---
// Kit first so the document's own components can override it, not the reverse.
const css = readText(join(src, 'css', 'kit.gen.css')) + '\n' +
            readText(join(src, 'css', 'tokens.css')) + '\n' +
            readText(join(src, 'css', 'app.css'));

// --- 3. Inline the icon as a data URI (no separate favicon request) ---
// Normalised before encoding: encodeURIComponent turns a stray CR into %0D,
// so a CRLF checkout would cost three bytes a line instead of one.
const iconSvg = readText(join(src, 'icons', 'icon.svg'));
const iconData = 'data:image/svg+xml,' + encodeURIComponent(iconSvg);

// --- 4. Stitch one self-contained index.html ---
// Function-form replacements: the inlined CSS/JS contain `$` (the DOM helper)
// which a string replacement would interpret as $&/$1/... - the () => form
// inserts the value literally.
let html = readText(join(src, 'index.html'));
html = html
  // first stylesheet link -> the whole inline <style>; the rest -> removed
  .replace(/<link rel="stylesheet" href="css\/kit\.gen\.css">/, () => '<style>\n' + css + '\n</style>')
  .replace(/[ \t]*<link rel="stylesheet" href="css\/tokens\.css">\r?\n/, '')
  .replace(/[ \t]*<link rel="stylesheet" href="css\/app\.css">\r?\n/, '')
  // shared kit -> inline classic script, before the module
  .replace(/<script src="js\/kit\.gen\.js"><\/script>/,
           () => '<script>\n' + kitJs + '\n</script>')
  // manifest + apple-touch-icon are secure-context-only (PWA install can't
  // run on the device's http origin) - drop the requests entirely
  .replace(/[ \t]*<link rel="manifest"[^>]*>\r?\n/, '')
  .replace(/[ \t]*<link rel="apple-touch-icon"[^>]*>\r?\n/, '')
  // favicon -> inline data URI
  .replace(/<link rel="icon"[^>]*>/, () => `<link rel="icon" href="${iconData}">`)
  // external module script -> inline module (keeps the runtime dynamic import)
  .replace(/<script type="module" src="js\/app\.js"><\/script>/,
           () => '<script type="module">\n' + js + '\n</script>');

// Guard against leftover same-origin sub-resource references in the page's
// HEAD/links only (the bundled JS legitimately contains route strings like
// "/web/..." - scope the check to <link>/<script src> tags).
const leftover = [...html.matchAll(/<(?:link|script)\b[^>]*\b(?:href|src)="(?:css|js|icons)\/[^"]*"[^>]*>/g)];
if (leftover.length) {
  console.error('[sdpack] FAILED: index.html still references separate files:');
  for (const m of leftover) console.error('   ', m[0].slice(0, 100));
  process.exit(1);
}
writeFileSync(join(out, 'index.html'), html);
console.log(`[sdpack] wrote self-contained index.html (${(html.length / 1024).toFixed(1)} KB, single request)`);

// --- 4b. Embed the gzipped shell in the firmware image ---
// Raw, the shell costs ~1.6 points of app0 flash; gzipped it costs ~0.5, which
// is cheap enough to make live listening work on a device with an empty or
// absent memory card - no "not on the memory card yet" dead end for anyone who
// never wants captions. Browsers decompress Content-Encoding: gzip natively.
const shellRaw = Buffer.from(html, 'utf8');
const shellGz = gz(shellRaw);
// Round-trip assert: a corrupt embedded shell would only show up as a blank
// page on hardware, which is an expensive way to find an encoder bug.
if (!gunzipSync(shellGz).equals(shellRaw)) {
  console.error('[sdpack] FAILED: gzipped shell does not round-trip');
  process.exit(1);
}
writeByteHeader(shellHeader, 'COMPANION_SHELL_GZ', shellGz,
  `Gzipped companion shell (${shellRaw.length} B raw -> ${shellGz.length} B). Served for ` +
  '/web/index.html when the memory card has no copy of its own.');
console.log(`[sdpack] embedded shell -> ${relative(join(root, '..'), shellHeader)} ` +
            `(${(shellRaw.length / 1024).toFixed(1)} KB -> ${(shellGz.length / 1024).toFixed(1)} KB gzipped)`);

// --- 4c. On-demand files go on the card pre-compressed ---
// Same header, applied to what stays on the card. The firmware serves a
// `<name>.gz` sibling when the plain name is missing, so a card written by an
// older pack (uncompressed) keeps working.
const workerGz = gz(Buffer.from(workerJs, 'utf8'));
writeFileSync(join(out, 'engine.worker.js.gz'), workerGz);
console.log(`[sdpack] wrote engine.worker.js.gz (${(workerJs.length / 1024).toFixed(1)} KB -> ` +
            `${(workerGz.length / 1024).toFixed(1)} KB, on-demand)`);

// --- 5. Vendor the on-demand transcription library ---
const tf = findPkg('@huggingface/transformers');
if (!tf) {
  console.error('[sdpack] @huggingface/transformers not found - run `npm install` first');
  process.exit(1);
}
const vendor = join(out, 'vendor');
mkdirSync(vendor, { recursive: true });

// Vendor payload goes on the card gzipped, not alongside an uncompressed twin:
// the point is to shrink the card footprint (and the WiFi transfer time), so
// only the .gz is written. Licence text stays plain - it is small and someone
// may want to read it off the card directly.
let vendorRaw = 0, vendorGz = 0;
function copyGz(from, toGz) {
  const buf = readFileSync(from);
  const out = gz(buf);
  writeFileSync(toGz, out);
  vendorRaw += buf.length;
  vendorGz += out.length;
}
copyGz(join(tf, 'dist', 'transformers.min.js'), join(vendor, 'transformers.min.js.gz'));
if (existsSync(join(tf, 'LICENSE'))) {
  copyFileSync(join(tf, 'LICENSE'), join(vendor, 'transformers.LICENSE.txt'));
}
const tfVersion = JSON.parse(readFileSync(join(tf, 'package.json'), 'utf8')).version;
console.log('[sdpack] vendored transformers.js', tfVersion);

const ort = findPkg('onnxruntime-web');
if (!ort) {
  console.error('[sdpack] onnxruntime-web not found - run `npm install` first');
  process.exit(1);
}
const ortOut = join(vendor, 'ort');
mkdirSync(ortOut, { recursive: true });
let ortFiles = 0;
for (const f of readdirSync(join(ort, 'dist'))) {
  if (/^ort-.*\.(wasm|mjs)$/.test(f)) {
    copyGz(join(ort, 'dist', f), join(ortOut, f + '.gz'));
    ortFiles++;
  }
}
writeFileSync(join(ortOut, 'LICENSE.txt'),
  'onnxruntime-web is (c) Microsoft Corporation, MIT licensed.\n' +
  'Full text: https://github.com/microsoft/onnxruntime/blob/main/LICENSE\n');
const ortVersion = JSON.parse(readFileSync(join(ort, 'package.json'), 'utf8')).version;
console.log(`[sdpack] vendored onnxruntime-web ${ortVersion} (${ortFiles} runtime files)`);

writeFileSync(join(out, 'pack-info.json'), JSON.stringify({
  built: new Date().toISOString(),
  transformers: tfVersion,
  onnxruntimeWeb: ortVersion,
}, null, 2));

console.log(`[sdpack] vendor payload gzipped: ${(vendorRaw / 1048576).toFixed(2)} MB -> ` +
            `${(vendorGz / 1048576).toFixed(2)} MB on the card`);

console.log('[sdpack] done ->', out);
console.log('[sdpack] live listening needs NO card files - the shell is in firmware.');
console.log('[sdpack] captions need engine.worker.js.gz + vendor/ on the card.');

// esbuild's background service can leave node with a non-zero exit on Windows
// even on success; the work above is complete, so exit clean explicitly.
process.exit(0);
