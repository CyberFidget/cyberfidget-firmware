// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC
//
// src/js/pack.js: only the DEVICE saying a file isn't there blames the memory
// card. A busy device is retried, a redirect or page from something else (a
// captive portal) and a device that never answers both mean "couldn't reach".
//
//   npm test

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { probeDeviceFile, probeDeviceFiles } from '../src/js/pack.js';

const NO_WAIT = [0, 0];
const FILE = '/web/vendor/transformers.min.js';

// A fake device: `script` maps a URL to the answers it gives, in order (the
// last one repeats). Answers: 'ok', 404, 503, 'redirect', 'opaque', 'html',
// 'json' (some other JSON), 'device' (the device's own status), 'offline'.
// /api/status answers 'device' unless scripted.
function device(script) {
  const calls = [];
  const fetchImpl = async (url, init) => {
    calls.push({ url, init });
    const answers = script[url] || (url === '/api/status' ? ['device'] : ['ok']);
    const n = calls.filter((c) => c.url === url).length;
    const a = answers[Math.min(n, answers.length) - 1];
    if (a === 'offline') throw new TypeError('Failed to fetch');
    if (a === 'opaque') return { type: 'opaqueredirect', status: 0, ok: false, headers: new Headers() };
    if (a === 'redirect') return new Response('', { status: 302, headers: { location: 'http://portal.example/' } });
    if (typeof a === 'number') return new Response('', { status: a });
    if (a === 'html') return new Response('<html>', { status: 200, headers: { 'content-type': 'text/html' } });
    if (a === 'json') return new Response('{"ok":true}', { status: 200, headers: { 'content-type': 'application/json' } });
    if (a === 'device') {
      return new Response(JSON.stringify({ files: 3, totalBytes: 100, usedBytes: 50, clients: 1 }), { status: 200, headers: { 'content-type': 'application/json' } });
    }
    const type = url.endsWith('.wasm') ? 'application/wasm' : 'text/javascript';
    return new Response('', { status: 200, headers: { 'content-type': type } });
  };
  return { fetchImpl, calls };
}

test('a 404 from the device means the card copy is missing', async () => {
  const d = device({ [FILE]: [404] });
  assert.equal(await probeDeviceFile(FILE, d.fetchImpl, NO_WAIT), 'missing');
  assert.deepEqual(d.calls.map((c) => c.url), [FILE, '/api/status'], 'not retried; confirmed with the device');
  assert.equal(d.calls[0].init.redirect, 'manual', 'redirects are not followed');
  assert.equal(d.calls[1].init.redirect, 'manual');
});

test('a 404 from something that is not the device does not blame the card', async () => {
  // e.g. a captive portal intercepting every request
  assert.equal(await probeDeviceFile(FILE, device({ [FILE]: [404], '/api/status': [404] }).fetchImpl, NO_WAIT), 'unreachable');
  assert.equal(await probeDeviceFile(FILE, device({ [FILE]: [404], '/api/status': ['json'] }).fetchImpl, NO_WAIT), 'unreachable',
    "JSON alone is not enough: it must be the device's own status");
  assert.equal(await probeDeviceFile(FILE, device({ [FILE]: [404], '/api/status': ['redirect'] }).fetchImpl, NO_WAIT), 'unreachable');
  assert.equal(await probeDeviceFile(FILE, device({ [FILE]: [404], '/api/status': ['offline'] }).fetchImpl, NO_WAIT), 'unreachable');
});

test('a busy status answer is retried before deciding', async () => {
  const d = device({ [FILE]: [404], '/api/status': [503, 'device'] });
  assert.equal(await probeDeviceFile(FILE, d.fetchImpl, NO_WAIT), 'missing');
});

test('a present file of the right type is fine', async () => {
  const d = device({});
  assert.equal(await probeDeviceFile(FILE, d.fetchImpl, NO_WAIT), 'ok');
});

test('a busy device is retried, and an answer after that counts', async () => {
  const d = device({ [FILE]: [503, 503, 'ok'] });
  assert.equal(await probeDeviceFile(FILE, d.fetchImpl, NO_WAIT), 'ok');
  assert.equal(d.calls.length, 3);
});

test('a device that stays busy is "unreachable", not an incomplete card', async () => {
  const d = device({ [FILE]: [503] });
  assert.equal(await probeDeviceFile(FILE, d.fetchImpl, NO_WAIT), 'unreachable');
  assert.equal(d.calls.length, 3, 'three tries');
});

test('no answer at all is retried, then "unreachable"', async () => {
  const d = device({ [FILE]: ['offline', 'offline', 404] });
  assert.equal(await probeDeviceFile(FILE, d.fetchImpl, NO_WAIT), 'missing', 'a late real answer still counts');
  const d2 = device({ [FILE]: ['offline'] });
  assert.equal(await probeDeviceFile(FILE, d2.fetchImpl, NO_WAIT), 'unreachable');
});

test('a redirect (a captive portal or another network) is "unreachable"', async () => {
  assert.equal(await probeDeviceFile(FILE, device({ [FILE]: ['redirect'] }).fetchImpl, NO_WAIT), 'unreachable');
  assert.equal(await probeDeviceFile(FILE, device({ [FILE]: ['opaque'] }).fetchImpl, NO_WAIT), 'unreachable');
});

test('a page of the wrong type blames the card only if the device itself sent it', async () => {
  // Older firmware answered a missing file with its HTML page: the device's
  // own status answer confirms it was the device.
  const fromDevice = device({ [FILE]: ['html'], '/api/status': ['device'] });
  assert.equal(await probeDeviceFile(FILE, fromDevice.fetchImpl, NO_WAIT), 'missing');
  // A captive portal answers everything with its own page - or its own JSON.
  const portal = device({ [FILE]: ['html'], '/api/status': ['html'] });
  assert.equal(await probeDeviceFile(FILE, portal.fetchImpl, NO_WAIT), 'unreachable');
  const portalJson = device({ [FILE]: ['html'], '/api/status': ['json'] });
  assert.equal(await probeDeviceFile(FILE, portalJson.fetchImpl, NO_WAIT), 'unreachable');
});

test('files are probed one at a time and the first bad one is named', async () => {
  let inFlight = 0; let max = 0;
  const d = device({ '/web/b.wasm': [404] });
  const slow = async (url, init) => {
    inFlight++; max = Math.max(max, inFlight);
    await new Promise((r) => setImmediate(r));
    inFlight--;
    return d.fetchImpl(url, init);
  };
  const found = await probeDeviceFiles(['/web/a.mjs', '/web/b.wasm', '/web/c.mjs'], slow, NO_WAIT);
  assert.deepEqual(found, { state: 'missing', file: 'b.wasm' });
  assert.deepEqual(d.calls.map((c) => c.url), ['/web/a.mjs', '/web/b.wasm', '/api/status']);
  assert.equal(max, 1);
});
