// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC
//
// The worker side (src/js/engine.worker.js): when the transcription library
// can't be imported, the worker asks the device about it and says which of
// these it was - the card copy is missing/bad (`missing`), the device stopped
// answering (`lost`), or neither. Under Node the absolute '/web/vendor/...'
// import resolves to a file that does not exist, so the import always fails;
// the fake device answer decides the diagnosis.
//
//   npm test

import { test } from 'node:test';
import assert from 'node:assert/strict';

let deviceAnswer = 'missing';
const probed = [];
const posted = [];
globalThis.self = {
  fetch: async (url) => {
    probed.push(String(url));
    if (deviceAnswer === 'offline') throw new TypeError('Failed to fetch');
    if (deviceAnswer === 'missing') return new Response('not found', { status: 404 });
    if (deviceAnswer === 'html') return new Response('<html>', { status: 200, headers: { 'content-type': 'text/html' } });
    return new Response('', { status: 200, headers: { 'content-type': 'text/javascript' } });
  },
  location: { href: 'http://device.test/web/', origin: 'http://device.test' },
  postMessage: (msg) => posted.push(msg),
};

await import('../src/js/engine.worker.js');

async function loadOnce(id) {
  posted.length = 0;
  probed.length = 0;
  await self.onmessage({ data: { type: 'load', id, modelId: 'm', english: true, useGpu: false } });
  const errors = posted.filter((m) => m.type === 'error');
  assert.equal(errors.length, 1);
  return errors[0];
}

test('a library the device does not have is posted back as missing', async () => {
  deviceAnswer = 'missing';
  const err = await loadOnce(7);
  assert.equal(err.id, 7, 'the load request id is echoed');
  assert.equal(err.missing, 'vendor/transformers.min.js');
  assert.equal(err.lost, undefined);
  assert.ok(err.error, 'the original error text is kept too');
  assert.deepEqual(probed, ['/web/vendor/transformers.min.js']);
});

test('a library served as the wrong type is posted back as missing', async () => {
  deviceAnswer = 'html';
  const err = await loadOnce(8);
  assert.equal(err.missing, 'vendor/transformers.min.js');
});

test('a device that stops answering is posted back as lost, not as a bad card', async () => {
  deviceAnswer = 'offline';
  const err = await loadOnce(9);
  assert.equal(err.lost, true);
  assert.equal(err.missing, undefined);
});

test('a library the device does have is not blamed on the card or the connection', async () => {
  deviceAnswer = 'ok';
  const err = await loadOnce(10);
  assert.equal(err.missing, undefined);
  assert.equal(err.lost, undefined);
});
