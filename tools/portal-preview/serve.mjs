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

// ── Fixtures. Field names mirror what the portal's JS reads. ────────────
const FIXTURES = {
  '/api/status': {
    files: 12, usedBytes: 268435456, totalBytes: 31914983424, clients: 1,
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

const json = (res, body) => {
  res.writeHead(200, { 'Content-Type': 'application/json' });
  res.end(JSON.stringify(body));
};

const [portal, fallback] = await Promise.all([
  extractLiteral(resolve(LIB, 'portal_page.h')),
  extractLiteral(resolve(LIB, 'companion_fallback_page.h')),
]);

createServer((req, res) => {
  const url = new URL(req.url, 'http://localhost');
  const path = url.pathname;

  if (path === '/' || path === '/index.html') {
    res.writeHead(200, { 'Content-Type': 'text/html; charset=utf-8' });
    return res.end(portal);
  }
  // The fallback page is what /web/ serves when the card has no companion.
  if (path === '/web/' || path === '/web' || path === '/fallback') {
    res.writeHead(200, { 'Content-Type': 'text/html; charset=utf-8' });
    return res.end(fallback);
  }
  if (path === '/api/browse') return json(res, BROWSE);
  if (path in FIXTURES) return json(res, FIXTURES[path]);
  // Writes and media streams are out of scope for a rendering harness.
  if (path.startsWith('/api/')) return json(res, { ok: true });

  res.writeHead(404, { 'Content-Type': 'text/plain' });
  res.end('not found');
}).listen(PORT, () => {
  console.log(`[portal-preview] portal:   http://localhost:${PORT}/`);
  console.log(`[portal-preview] fallback: http://localhost:${PORT}/web/`);
  console.log(`[portal-preview] fixtures only - no device, no writes`);
});
