// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC
//
// Drift check between the two device-served surfaces' design tokens.
//
//   node verify_tokens.mjs        (or: npm run verify:tokens)
//
// The portal (lib/WebPortalApp/portal_page.h) and the companion
// (src/css/tokens.css) are separate documents with separate token blocks,
// because the portal must render with no memory card and no build step. They
// were aligned by hand once; nothing stopped them drifting apart again, and a
// drifted pair is exactly the visual seam the alignment work removed.
//
// This compares the VALUES the two blocks share, keyed by role rather than by
// name (the portal uses short names to stay small in flash; the companion uses
// the website kit's names). A token missing on one side is not an error - the
// surfaces genuinely have different needs - but a shared role resolving to two
// different colours is.

import { readFileSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = dirname(fileURLToPath(import.meta.url));
const portalPath = join(root, '..', 'lib', 'WebPortalApp', 'portal_page.h');
const tokensPath = join(root, 'src', 'css', 'tokens.css');

// role -> [portal var name, companion var name]
const SHARED = {
  'page background':   ['--bg-primary',    '--cf-bg-page'],
  'card background':   ['--bg-secondary',  '--cf-bg-card'],
  'deep background':   ['--bg-tertiary',   '--cf-bg-deep'],
  'elevated surface':  ['--bg-elev',       '--cf-bg-elev'],
  'sunk fill':         ['--bg-sunk',       '--cf-bg-sunk'],
  'ink':               ['--text-primary',  '--cf-ink'],
  'ink dim':           ['--text-secondary', '--cf-ink-dim'],
  'ink muted':         ['--text-mute',     '--cf-ink-mute'],
  'cyan':              ['--accent',        '--cf-cyan'],
  'magenta':           ['--magenta',       '--cf-magenta'],
  'purple':            ['--purple',        '--cf-purple'],
  'green':             ['--success',       '--cf-green'],
  'yellow':            ['--warn',          '--cf-yellow'],
  'red':               ['--danger',        '--cf-red'],
  'hairline':          ['--border',        '--cf-bg-line'],
};

// Both files declare tokens as `--name: value;` inside a :root block.
function readTokens(text) {
  const out = new Map();
  for (const m of text.matchAll(/(--[a-z0-9-]+)\s*:\s*([^;}]+)/gi)) {
    out.set(m[1], m[2].trim().replace(/\s+/g, ' '));
  }
  return out;
}

// #RGB / #RRGGBB / rgba(...) all normalise so 0.08 vs .08 and case do not trip it.
function norm(v) {
  let s = v.toLowerCase().replace(/\s/g, '');
  const short = s.match(/^#([0-9a-f])([0-9a-f])([0-9a-f])$/);
  if (short) s = '#' + short.slice(1).map((c) => c + c).join('');
  s = s.replace(/(^|[(,])0\./g, '$1.');
  return s;
}

const portal = readTokens(readFileSync(portalPath, 'utf8'));
const companion = readTokens(readFileSync(tokensPath, 'utf8'));

const problems = [];
let compared = 0;
for (const [role, [pName, cName]] of Object.entries(SHARED)) {
  const p = portal.get(pName);
  const c = companion.get(cName);
  if (p === undefined || c === undefined) {
    problems.push(`${role}: ${p === undefined ? pName + ' missing from the portal' : ''}` +
                  `${c === undefined ? cName + ' missing from tokens.css' : ''}`);
    continue;
  }
  compared++;
  if (norm(p) !== norm(c)) {
    problems.push(`${role}: portal ${pName}=${p} but companion ${cName}=${c}`);
  }
}

if (problems.length) {
  console.error('[verify:tokens] FAILED - the two surfaces have drifted:');
  for (const p of problems) console.error('   ' + p);
  console.error('\n   Both surfaces are served by the device and users cross between');
  console.error('   them; a shared role must resolve to the same value on each.');
  process.exit(1);
}

console.log(`[verify:tokens] ${compared} shared token roles agree across the portal and the companion`);
