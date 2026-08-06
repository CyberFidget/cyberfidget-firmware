// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC
//
// Drift check for the shared kit (DEVICE-SURFACES-HANDOFF.md decision 4).
//
//   node verify_chrome.mjs        (or: npm run verify:chrome)
//
// The device serves two documents and the user is meant to experience one app
// spanning both. That only stays true if the navigation, the player, the gate
// panel and the download flow are literally the same code on each side. They
// have one source in src/shared/; sync_chrome.mjs writes it into four places.
// This re-derives all four and fails if any has been edited in place or left
// stale after a change to the shared source.
//
// The failure mode this exists to prevent is quiet: an edit to the portal's copy
// of the tab bar looks completely fine in the portal, and the seam only reopens
// for a user crossing between the two - which is the one thing nobody does while
// working on a single surface.

import { readFileSync, existsSync } from 'node:fs';
import { relative, dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { buildCss, buildJs, lf, markerRange, PORTAL, GEN_CSS, GEN_JS } from './sync_chrome.mjs';

const root = dirname(fileURLToPath(import.meta.url));
const rel = (p) => relative(join(root, '..'), p).replace(/\\/g, '/');

const problems = [];

function checkGenerated(path, expected, what) {
  if (!existsSync(path)) {
    problems.push(`${rel(path)} is missing (${what})`);
    return;
  }
  const actual = lf(readFileSync(path, 'utf8'));
  // The generated files carry a banner; compare only the payload after it.
  const body = actual.slice(actual.indexOf('*/\n') + 3);
  if (body !== expected) problems.push(`${rel(path)} has drifted from src/shared/ (${what})`);
}

const css = buildCss();
const js = buildJs();

checkGenerated(GEN_CSS, css, 'kit stylesheet');
checkGenerated(GEN_JS, js, 'kit behaviour');

if (!existsSync(PORTAL)) {
  problems.push(`${rel(PORTAL)} is missing`);
} else {
  const portal = readFileSync(PORTAL, 'utf8');
  for (const [mark, expected, what] of [
    ['CF-KIT-CSS', css, 'kit stylesheet'],
    ['CF-KIT-JS', js, 'kit behaviour'],
  ]) {
    const at = markerRange(portal, mark);
    if (!at) problems.push(`${rel(PORTAL)} has no ${mark} marker pair`);
    else if (lf(portal.slice(at.start, at.end)) !== expected) {
      problems.push(`${rel(PORTAL)} ${mark} block has drifted (${what})`);
    }
  }
}

if (problems.length) {
  console.error('[verify:chrome] FAILED - the shared kit is out of sync:');
  for (const p of problems) console.error('   ' + p);
  console.error('\n   The portal and the companion must carry the same navigation,');
  console.error('   player and download code, or the seam between the two documents');
  console.error('   reopens for anyone crossing between them.');
  console.error('\n   Fix: cd portal-companion && npm run chrome:sync');
  process.exit(1);
}

console.log('[verify:chrome] shared kit is identical in both device documents ' +
            `(${(css.length / 1024).toFixed(1)} KB css, ${(js.length / 1024).toFixed(1)} KB js)`);
