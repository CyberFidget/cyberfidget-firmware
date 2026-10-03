// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC
//
// The worker side (src/js/engine.worker.js): when the transcription library
// can't be imported from the device, the error it posts back names the file,
// so the page can report the memory-card copy as incomplete. Under Node the
// absolute '/web/vendor/...' import resolves to a file that does not exist,
// which is exactly the missing-from-the-card case.
//
//   npm test

import { test } from 'node:test';
import assert from 'node:assert/strict';

const posted = [];
globalThis.self = {
  fetch: async () => { throw new Error('no network in this test'); },
  location: { href: 'http://device.test/web/', origin: 'http://device.test' },
  postMessage: (msg) => posted.push(msg),
};

await import('../src/js/engine.worker.js');

test('a missing library is posted back with the file it could not load', async () => {
  await self.onmessage({ data: { type: 'load', modelId: 'm', english: true, useGpu: false } });
  assert.equal(posted.length, 1);
  assert.equal(posted[0].type, 'error');
  assert.equal(posted[0].missing, 'vendor/transformers.min.js');
  assert.ok(posted[0].error, 'the original error text is kept too');
});
