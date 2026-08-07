// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC
//
// Generate the shared kit into both device documents.
//
//   node sync_chrome.mjs        (or: npm run chrome:sync)
//
// The device serves two documents - the portal at / and the companion at /web/ -
// and DEVICE-SURFACES-HANDOFF.md decision 4 requires that the navigation cannot
// drift between them. Rather than keep two hand-maintained copies and diff them,
// there is ONE source in src/shared/ and this writes it into both:
//
//   src/shared/chrome.css + icons.mjs  ->  src/css/kit.gen.css      (companion)
//                                          portal_page.h CF-KIT-CSS (portal)
//   src/shared/chrome.js               ->  src/js/kit.gen.js        (companion)
//                                          portal_page.h CF-KIT-JS  (portal)
//
// The companion's generated files are picked up by build_sdpack.mjs and inlined,
// same as the rest of its CSS/JS. The portal's copies are spliced between marker
// comments in the PROGMEM literal, which stays the editable source for
// everything outside those markers - so scripts/gzip_portal_page.py is unchanged
// and the firmware still builds with no JS toolchain installed.
//
// verify_chrome.mjs re-derives all four outputs and fails if any has drifted.
// Run this after editing anything in src/shared/.

import { readFileSync, writeFileSync } from 'node:fs';
import { join, dirname, relative } from 'node:path';
import { fileURLToPath } from 'node:url';
import { iconRules } from './src/shared/icons.mjs';

const root = dirname(fileURLToPath(import.meta.url));
const shared = join(root, 'src', 'shared');
export const PORTAL = join(root, '..', 'lib', 'WebPortalApp', 'portal_page.h');
export const GEN_CSS = join(root, 'src', 'css', 'kit.gen.css');
export const GEN_JS = join(root, 'src', 'js', 'kit.gen.js');

const BANNER = (what) =>
  `/* GENERATED FROM portal-companion/src/shared/ - DO NOT EDIT HERE.\n` +
  `   Edit the shared source, then run: cd portal-companion && npm run chrome:sync\n` +
  `   ${what} */\n`;

// portal_page.h is checked out with CRLF on Windows and the shared sources are
// LF, so everything is compared and generated in LF and only converted on the
// way into the portal. Otherwise the drift check would fail on line endings
// alone, on one developer's machine and not another's.
export const lf = (s) => s.replace(/\r\n/g, '\n');

// The kit stylesheet is the hand-written chrome plus the pixel-icon rules
// expanded from the bitmaps, so the icons ship as precomputed box-shadow lists
// and no renderer has to run on the device's surfaces.
export function buildCss() {
  const css = lf(readFileSync(join(shared, 'chrome.css'), 'utf8')).trimEnd();
  return `${css}\n\n/* Pixel icons, expanded from shared/icons.mjs. */\n${iconRules()}\n`;
}

export function buildJs() {
  return lf(readFileSync(join(shared, 'chrome.js'), 'utf8')).trimEnd() + '\n';
}

// Locate the payload between `/* MARK:BEGIN ... */` and `/* MARK:END */`.
// Returns null when either marker is absent, so the verifier can report that
// distinctly from "present but drifted".
export function markerRange(text, mark) {
  const open = text.match(new RegExp(`[ \\t]*/\\* ${mark}:BEGIN[^]*?\\*/\\r?\\n`));
  const close = text.match(new RegExp(`[ \\t]*/\\* ${mark}:END \\*/`));
  if (!open || !close) return null;
  const start = text.indexOf(open[0]) + open[0].length;
  const end = text.indexOf(close[0]);
  return end >= start ? { start, end } : null;
}

// Splice a block between the markers, keeping the marker lines themselves.
// Throws rather than guessing if either is absent.
export function splice(text, mark, body) {
  const at = markerRange(text, mark);
  if (!at) throw new Error(`marker pair ${mark}:BEGIN/:END not found (or reversed) in the portal page`);
  const eol = text.includes('\r\n') ? '\r\n' : '\n';
  return text.slice(0, at.start) + body.replace(/\n/g, eol) + text.slice(at.end);
}

function main() {
  const css = buildCss();
  const js = buildJs();

  writeFileSync(GEN_CSS, BANNER('Shared kit stylesheet.') + css);
  writeFileSync(GEN_JS, BANNER('Shared kit behaviour.') + js);

  let portal = readFileSync(PORTAL, 'utf8');
  portal = splice(portal, 'CF-KIT-CSS', css);
  portal = splice(portal, 'CF-KIT-JS', js);
  writeFileSync(PORTAL, portal);

  const kb = (s) => (s.length / 1024).toFixed(1) + ' KB';
  console.log(`[chrome:sync] kit css ${kb(css)}, kit js ${kb(js)}`);
  console.log(`[chrome:sync] -> ${relative(root, GEN_CSS)}, ${relative(root, GEN_JS)}`);
  console.log(`[chrome:sync] -> ${relative(join(root, '..'), PORTAL)} (both marker blocks)`);
}

// Importable by verify_chrome.mjs without doing the writes.
if (process.argv[1] && process.argv[1].endsWith('sync_chrome.mjs')) main();
