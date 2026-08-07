// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC
//
// tools/portal-preview/serve.mjs
//
// Serves the device's web-portal pages in a desktop browser without a
// device. The pages live as PROGMEM raw-string literals inside the
// firmware headers, so this extracts the literal and answers the portal's
// /api/* calls with fixtures - enough for the page to render populated
// instead of sitting on "Loading...".
//
// Fixtures only. This is a rendering harness for design review and
// screenshots; it is NOT a device simulator and does not implement writes.

import { createServer } from 'node:http';
import { readFile } from 'node:fs/promises';
import { fileURLToPath } from 'node:url';
import { dirname, resolve } from 'node:path';

const HERE = dirname(fileURLToPath(import.meta.url));
const LIB = resolve(HERE, '../../lib/WebPortalApp');
const PORT = Number(process.env.PORT || 8099);

// Pull the text between R"rawliteral( and )rawliteral" out of a header.
async function extractLiteral(headerPath) {
  const src = await readFile(headerPath, 'utf8');
  const open = src.indexOf('R"rawliteral(');
  const close = src.lastIndexOf(')rawliteral"');
  if (open < 0 || close < 0) {
    throw new Error(`no PROGMEM raw literal found in ${headerPath}`);
  }
  return src.slice(open + 'R"rawliteral('.length, close);
}

// Pull the 0x.. byte array out of a generated header. Used for the gzipped
// companion shell so this harness can serve the SAME bytes the firmware does -
// a corrupt gzip stream then shows up here instead of as a blank page on
// hardware. (What this canNOT check is how ESPAsyncWebServer sets
// Content-Length for a PROGMEM buffer; that still needs a device.)
async function extractByteArray(headerPath) {
  const src = await readFile(headerPath, 'utf8');
  const open = src.indexOf('{', src.indexOf('PROGMEM'));
  const close = src.lastIndexOf('}');
  if (open < 0 || close < 0) {
    throw new Error(`no byte array found in ${headerPath}`);
  }
  const bytes = src.slice(open + 1, close)
    .split(',')
    .map((t) => t.trim())
    .filter((t) => t.length)
    .map((t) => Number.parseInt(t, 16));
  if (bytes.some((b) => !Number.isInteger(b) || b < 0 || b > 255)) {
    throw new Error(`malformed byte in ${headerPath}`);
  }
  return Buffer.from(bytes);
}

// ── Fixtures. Field names mirror what the portal's JS reads. ────────────
// `captions` says whether the transcription pack is on the card. Start the
// server with NOPACK=1 to flip it false and render the gated state from the
// `State _ No transcription pack` artboard without hunting for a bare card.
const NOPACK = process.env.NOPACK === '1';

const FIXTURES = {
  '/api/status': {
    files: 12, usedBytes: 268435456, totalBytes: 31914983424, clients: 1,
    version: '1.3.3+5f175fb', captions: !NOPACK, sd: true,
  },
  '/api/files': [
    { name: 'Albums', type: 'dir', children: [
      { name: 'Neon Horizon', type: 'dir', children: [
        { name: 'Chrome Sunrise.mp3', type: 'file', size: 7340032 },
        { name: 'Grid Runner.mp3', type: 'file', size: 6291456 },
      ] },
      { name: 'Pocket Static.mp3', type: 'file', size: 4194304 },
    ] },
    { name: 'Podcasts', type: 'dir', children: [
      { name: 'Bench Notes 014.mp3', type: 'file', size: 18874368 },
    ] },
    { name: 'Test Tone 1k.mp3', type: 'file', size: 524288 },
  ],
  '/api/tracks': [
    { path: '/Albums/Neon Horizon/Chrome Sunrise.mp3', title: 'Chrome Sunrise',
      artist: 'Vector Pilot', album: 'Neon Horizon', size: 7340032 },
    { path: '/Albums/Neon Horizon/Grid Runner.mp3', title: 'Grid Runner',
      artist: 'Vector Pilot', album: 'Neon Horizon', size: 6291456 },
    { path: '/Albums/Pocket Static.mp3', title: 'Pocket Static',
      artist: 'Dismo Industries', album: 'Singles', size: 4194304 },
    { path: '/Podcasts/Bench Notes 014.mp3', title: 'Bench Notes 014 - Slider Feel',
      artist: 'Bench Notes', album: 'Season 2', size: 18874368 },
    { path: '/Test Tone 1k.mp3', title: 'Test Tone 1k', artist: '', album: '',
      size: 524288 },
  ],
  '/api/playlists': [
    { name: 'Focus', tracks: 3 },
    { name: 'Bench Session', tracks: 2 },
  ],
  // timestamp is an ISO-ish STRING on the device ("2026-08-04T09:12:44"),
  // not epoch seconds - fmtNoteDate() calls .replace() on it.
  '/api/recordings': { sd: true, items: [
    { name: '2026-08-04_0912.wav', timestamp: '2026-08-04T09:12:44', duration: 42, bytes: 1352000 },
    { name: '2026-08-03_1744.wav', timestamp: '2026-08-03T17:44:03', duration: 8, bytes: 258000 },
    { name: '2026-08-01_2231.wav', timestamp: '', duration: 121, bytes: 3894000 },
  ] },
  '/api/wifi/status': {
    ap_ip: '192.168.4.1', connected: true, ssid: 'Bench-2G',
    ip: '192.168.1.84', mdns: 'cyberfidget.local',
  },
  '/api/wifi/scan': [
    { ssid: 'Bench-2G', rssi: -48, secure: true },
    { ssid: 'Workshop-Guest', rssi: -67, secure: false },
    { ssid: 'CF-Test-AP', rssi: -81, secure: true },
  ],
};

const BROWSE = { sd: true, entries: [
  { name: 'Albums', type: 'dir', size: 0 },
  { name: 'Podcasts', type: 'dir', size: 0 },
  { name: 'recordings', type: 'dir', size: 0 },
  { name: 'Test Tone 1k.mp3', type: 'file', size: 524288 },
  { name: 'settings.json', type: 'file', size: 1240 },
] };

// The companion's Notes view reads this folder to learn which recordings already
// have a transcript sidecar beside them - that is what decides whether a row
// shows the TRANSCRIBE action or the green TRANSCRIPT chip, so the fixture needs
// both kinds present to exercise the two states.
const BROWSE_RECORDINGS = { sd: true, entries: [
  { name: '2026-08-04_0912.wav', type: 'file', size: 1352000, mtime: 1786000364 },
  { name: '2026-08-03_1744.wav', type: 'file', size: 258000, mtime: 1785908643 },
  { name: '2026-08-03_1744.txt', type: 'file', size: 412, mtime: 1785908700 },
  { name: '2026-08-01_2231.wav', type: 'file', size: 3894000, mtime: 1785774660 },
] };

const json = (res, body) => {
  res.writeHead(200, { 'Content-Type': 'application/json' });
  res.end(JSON.stringify(body));
};

const [portal, fallback, shellGz] = await Promise.all([
  extractLiteral(resolve(LIB, 'portal_page.h')),
  extractLiteral(resolve(LIB, 'companion_fallback_page.h')),
  extractByteArray(resolve(LIB, 'companion_shell_gz.h')),
]);

// The device serves the portal gzipped out of generated/portal_page_gz.h, which
// is a build artifact. Prefer it when a build has produced one, so this harness
// exercises the same bytes and the same headers the device does; fall back to the
// raw literal (still the editable source) when it has not.
let portalGz = null;
try {
  portalGz = await extractByteArray(resolve(HERE, '../../generated/portal_page_gz.h'));
} catch { /* no build yet - serve the literal uncompressed */ }

createServer((req, res) => {
  const url = new URL(req.url, 'http://localhost');
  const path = url.pathname;

  if (path === '/' || path === '/index.html') {
    if (portalGz) {
      res.writeHead(200, {
        'Content-Type': 'text/html; charset=utf-8',
        'Content-Encoding': 'gzip',
        'Content-Length': portalGz.length,
        'Cache-Control': 'no-cache',
      });
      return res.end(portalGz);
    }
    res.writeHead(200, { 'Content-Type': 'text/html; charset=utf-8' });
    return res.end(portal);
  }
  // /web/ serves the gzipped shell out of the firmware image, exactly as the
  // device does when the card has no copy of its own.
  if (path === '/web/' || path === '/web' || path === '/web/index.html') {
    res.writeHead(200, {
      'Content-Type': 'text/html; charset=utf-8',
      'Content-Encoding': 'gzip',
      'Content-Length': shellGz.length,
      'Cache-Control': 'no-cache',
    });
    return res.end(shellGz);
  }
  // The old not-on-the-card page. Unreachable on the device now; kept here so
  // its wording can still be reviewed while it is reused for a captions notice.
  if (path === '/fallback') {
    res.writeHead(200, { 'Content-Type': 'text/html; charset=utf-8' });
    return res.end(fallback);
  }
  if (path === '/api/browse') {
    return json(res, url.searchParams.get('path') === '/recordings'
      ? BROWSE_RECORDINGS : BROWSE);
  }
  // A voice note's transcript sidecar, so an expanded Notes row has something
  // to put on its phosphor panel.
  if (path.startsWith('/recordings/') && path.endsWith('.txt')) {
    res.writeHead(200, { 'Content-Type': 'text/plain; charset=utf-8' });
    return res.end('what if the six keys worked like a chorded keyboard - hold ' +
                   'two for symbols. check if the debounce window is long enough ' +
                   'for real chords.\n');
  }
  if (path in FIXTURES) return json(res, FIXTURES[path]);
  // Writes and media streams are out of scope for a rendering harness.
  if (path.startsWith('/api/')) return json(res, { ok: true });

  res.writeHead(404, { 'Content-Type': 'text/plain' });
  res.end('not found');
}).listen(PORT, () => {
  console.log(`[portal-preview] portal:   http://localhost:${PORT}/  ` +
              (portalGz ? `(gzipped from flash, ${portalGz.length} B)`
                        : '(raw literal - no build artifact found)'));
  console.log(`[portal-preview] shell:    http://localhost:${PORT}/web/  ` +
              `(gzipped from flash, ${shellGz.length} B)`);
  console.log(`[portal-preview] fallback: http://localhost:${PORT}/fallback  (retired page)`);
  console.log(`[portal-preview] transcription pack: ${NOPACK ? 'ABSENT (gated states)' : 'present'}` +
              `${NOPACK ? '' : '  - set NOPACK=1 to preview the gates'}`);
  console.log(`[portal-preview] fixtures only - no device, no writes`);
});
