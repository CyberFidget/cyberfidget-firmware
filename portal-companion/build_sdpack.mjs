// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC
//
// Assemble the companion "SD pack": what the device serves at /web/.
//
//   npm install          (once - pulls esbuild + the vendored libraries)
//   node build_sdpack.mjs
//
// CRITICAL design constraint (learned on hardware, see firmware L-017 /
// L-016): the device's async web server serves the SD card over a single
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
import { cpSync, mkdirSync, rmSync, existsSync, readdirSync, copyFileSync, writeFileSync, readFileSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = dirname(fileURLToPath(import.meta.url));
const src = join(root, 'src');
const out = join(root, 'dist', 'web');

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

// --- 2. Inline CSS ---
const css = readFileSync(join(src, 'css', 'tokens.css'), 'utf8') + '\n' +
            readFileSync(join(src, 'css', 'app.css'), 'utf8');

// --- 3. Inline the icon as a data URI (no separate favicon request) ---
const iconSvg = readFileSync(join(src, 'icons', 'icon.svg'), 'utf8');
const iconData = 'data:image/svg+xml,' + encodeURIComponent(iconSvg);

// --- 4. Stitch one self-contained index.html ---
// Function-form replacements: the inlined CSS/JS contain `$` (the DOM helper)
// which a string replacement would interpret as $&/$1/... - the () => form
// inserts the value literally.
let html = readFileSync(join(src, 'index.html'), 'utf8');
html = html
  // first stylesheet link -> the whole inline <style>; second -> removed
  .replace(/<link rel="stylesheet" href="css\/tokens\.css">/, () => '<style>\n' + css + '\n</style>')
  .replace(/[ \t]*<link rel="stylesheet" href="css\/app\.css">\r?\n/, '')
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

writeFileSync(join(out, 'engine.worker.js'), workerJs);
console.log(`[sdpack] wrote engine.worker.js (${(workerJs.length / 1024).toFixed(1)} KB, on-demand)`);

// --- 5. Vendor the on-demand transcription library ---
const tf = findPkg('@huggingface/transformers');
if (!tf) {
  console.error('[sdpack] @huggingface/transformers not found - run `npm install` first');
  process.exit(1);
}
const vendor = join(out, 'vendor');
mkdirSync(vendor, { recursive: true });
copyFileSync(join(tf, 'dist', 'transformers.min.js'), join(vendor, 'transformers.min.js'));
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
    copyFileSync(join(ort, 'dist', f), join(ortOut, f));
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

console.log('[sdpack] done ->', out);
console.log('[sdpack] live listening needs only index.html; captions also need engine.worker.js + vendor/');

// esbuild's background service can leave node with a non-zero exit on Windows
// even on success; the work above is complete, so exit clean explicitly.
process.exit(0);
