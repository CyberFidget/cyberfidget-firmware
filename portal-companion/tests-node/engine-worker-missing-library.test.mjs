// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC
//
// The worker side (src/js/engine.worker.js): when the transcription library
// can't be imported, the worker asks the device about it and says which it
// was - the card copy is missing (`kind: 'card'` + `file`), the device
// couldn't be reached (`kind: 'connection'`), or neither (`kind: 'engine'`).
// Under Node the absolute '/web/vendor/...' import resolves to a file that
// does not exist, so the import always fails; the fake device answer decides
// the diagnosis.
//
//   npm test

import { test } from 'node:test';
import assert from 'node:assert/strict';

let deviceAnswer = 'missing';
const probed = [];
const posted = [];
globalThis.self = {
  fetch: async (url) => {
    url = String(url);
    probed.push(url);
    if (deviceAnswer === 'offline') throw new TypeError('Failed to fetch');
    if (deviceAnswer === 'missing') return new Response('not found', { status: 404 });
    if (deviceAnswer === 'portal') return new Response('<html>', { status: 200, headers: { 'content-type': 'text/html' } });
    if (deviceAnswer === 'old-firmware') {
      // Answered a missing file with its HTML page, but it IS the device.
      return url === '/api/status'
        ? new Response('{}', { status: 200, headers: { 'content-type': 'application/json' } })
        : new Response('<html>', { status: 200, headers: { 'content-type': 'text/html' } });
    }
    return new Response('', { status: 200, headers: { 'content-type': 'text/javascript' } });
  },
  location: { href: 'http://device.test/web/', origin: 'http://device.test' },
  postMessage: (msg) => posted.push(msg),
};

await import('../src/js/engine.worker.js');

test('the worker says hello with its protocol when it starts', () => {
  assert.deepEqual(posted[0], { type: 'hello', protocol: 2 });
});

async function loadOnce(id) {
  posted.length = 0;
  probed.length = 0;
  self.onmessage({ data: { type: 'load', id, modelId: 'm', english: true, useGpu: false } });
  for (let i = 0; i < 400 && !posted.some((m) => m.type === 'error'); i++) {
    await new Promise((r) => setTimeout(r, 25));
  }
  const errors = posted.filter((m) => m.type === 'error');
  assert.equal(errors.length, 1);
  return errors[0];
}

test('a library the device says is missing is reported as a card problem', async () => {
  deviceAnswer = 'missing';
  const err = await loadOnce(7);
  assert.equal(err.id, 7, 'the load request id is echoed');
  assert.equal(err.kind, 'card');
  assert.equal(err.file, 'vendor/transformers.min.js');
  assert.deepEqual(probed, ['/web/vendor/transformers.min.js']);
});

test('older firmware answering with its own page is still a card problem', async () => {
  deviceAnswer = 'old-firmware';
  const err = await loadOnce(8);
  assert.equal(err.kind, 'card');
  assert.deepEqual(probed, ['/web/vendor/transformers.min.js', '/api/status']);
});

test('a captive portal answering instead of the device is not blamed on the card', async () => {
  deviceAnswer = 'portal';
  const err = await loadOnce(9);
  assert.equal(err.kind, 'connection');
  assert.equal(err.file, undefined);
});

test('a device that never answers is "could not reach", after retries', async () => {
  deviceAnswer = 'offline';
  const err = await loadOnce(10);
  assert.equal(err.kind, 'connection');
  assert.equal(probed.length, 3);
});

test('a library the device does have is an engine problem, not the card or connection', async () => {
  deviceAnswer = 'ok';
  const err = await loadOnce(11);
  assert.equal(err.kind, 'engine');
});
