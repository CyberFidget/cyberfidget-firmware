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
// This compares the VALUES the blocks share, keyed by role rather than by name
// (the portal uses short names to stay small in flash; the companion uses the
// website kit's names). A token missing on one side is not an error - the
// surfaces genuinely have different needs - but a shared role resolving to two
// different colours is.
//
// There is a THIRD spelling now: the shared kit (src/shared/chrome.css) declares
// its own --k-* literals, because that block has to be byte-identical in both
// documents and so cannot reference either one's token names. It is checked here
// too, or the thing that exists to stop drift would be free to drift itself.

import { readFileSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = dirname(fileURLToPath(import.meta.url));
const portalPath = join(root, '..', 'lib', 'WebPortalApp', 'portal_page.h');
const tokensPath = join(root, 'src', 'css', 'tokens.css');
const kitPath = join(root, 'src', 'shared', 'chrome.css');

// role -> [portal var name, companion var name, kit var name | null]
const SHARED = {
  'page background':   ['--bg-primary',    '--cf-bg-page',   '--k-page'],
  'card background':   ['--bg-secondary',  '--cf-bg-card',   '--k-card'],
  'deep background':   ['--bg-tertiary',   '--cf-bg-deep',   '--k-deep'],
  'elevated surface':  ['--bg-elev',       '--cf-bg-elev',   '--k-elev'],
  'sunk fill':         ['--bg-sunk',       '--cf-bg-sunk',   '--k-sunk'],
  'ink':               ['--text-primary',  '--cf-ink',       '--k-ink'],
  'ink dim':           ['--text-secondary', '--cf-ink-dim',  '--k-dim'],
  'ink muted':         ['--text-mute',     '--cf-ink-mute',  '--k-mute'],
  'cyan':              ['--accent',        '--cf-cyan',      '--k-cy'],
  'magenta':           ['--magenta',       '--cf-magenta',   '--k-mg'],
  'purple':            ['--purple',        '--cf-purple',    null],
  'green':             ['--success',       '--cf-green',     '--k-gn'],
  'yellow':            ['--warn',          '--cf-yellow',    '--k-yl'],
  'red':               ['--danger',        '--cf-red',       '--k-rd'],
  'hairline':          ['--border',        '--cf-bg-line',   '--k-line'],
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
const kit = readTokens(readFileSync(kitPath, 'utf8'));

const problems = [];
let compared = 0;
for (const [role, [pName, cName, kName]] of Object.entries(SHARED)) {
  // [where it is declared, the name there, the value] for each spelling in play.
  const sides = [['the portal', pName, portal.get(pName)],
                 ['tokens.css', cName, companion.get(cName)]];
  if (kName) sides.push(['the shared kit', kName, kit.get(kName)]);

  const missing = sides.filter(([, , v]) => v === undefined);
  if (missing.length) {
    for (const [where, name] of missing) problems.push(`${role}: ${name} missing from ${where}`);
    continue;
  }
  compared++;
  const [, , ref] = sides[0];
  for (const [where, name, v] of sides.slice(1)) {
    if (norm(v) !== norm(ref)) {
      problems.push(`${role}: portal ${pName}=${ref} but ${where} ${name}=${v}`);
    }
  }
}

if (problems.length) {
  console.error('[verify:tokens] FAILED - the surfaces have drifted:');
  for (const p of problems) console.error('   ' + p);
  console.error('\n   Both surfaces are served by the device and users cross between');
  console.error('   them; a shared role must resolve to the same value on each.');
  process.exit(1);
}

console.log(`[verify:tokens] ${compared} shared token roles agree across the portal, ` +
            `the companion and the shared kit`);
