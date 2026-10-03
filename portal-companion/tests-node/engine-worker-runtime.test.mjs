// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC
//
// The worker side (src/js/engine.worker.js) with a stand-in transcription
// library: when the session build fails, the worker asks the device about the
// runtime files, one at a time, and reports a missing one, a lost connection,
// or neither (a model download problem). Also: the 'building' message that
// lets the page allow for a long silent build, load replies echoing their
// request id, and a failed build not being reused.
//
//   npm test

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { registerHooks } from 'node:module';

// Point the worker's library import at a stand-in whose pipeline() the tests
// control through globalThis.__pipeline.
const FAKE_LIBRARY = 'data:text/javascript,' + encodeURIComponent(
  'export const env = { backends: { onnx: { wasm: {} } } };\n' +
  'export const pipeline = (...a) => globalThis.__pipeline(...a);\n');
registerHooks({
  resolve(specifier, context, next) {
    if (specifier === '/web/vendor/transformers.min.js') return { url: FAKE_LIBRARY, shortCircuit: true };
    return next(specifier, context);
  },
});

// Minimal IndexedDB (the worker records a finished build).
const later = (fn) => Promise.resolve().then(fn);
globalThis.indexedDB = {
  open() {
    const req = {};
    later(() => {
      req.result = {
        version: 1,
        objectStoreNames: { contains: () => true },
        close() {},
        transaction() {
          const t = { objectStore: () => ({ put: () => ({}) }) };
          later(() => t.oncomplete && t.oncomplete());
          return t;
        },
      };
      req.onsuccess && req.onsuccess();
    });
    return req;
  },
};

// Device answers per file; anything not listed is present.
let answers = {};
let inFlight = 0;
let maxInFlight = 0;
const probed = [];
const posted = [];
globalThis.self = {
  fetch: async (url) => {
    url = String(url);
    probed.push(url);
    inFlight++; maxInFlight = Math.max(maxInFlight, inFlight);
    await new Promise((r) => setImmediate(r));
    inFlight--;
    const a = answers[url] || 'ok';
    if (a === 'offline') throw new TypeError('Failed to fetch');
    if (a === 'missing') return new Response('not found', { status: 404 });
    const type = url.endsWith('.wasm') ? 'application/wasm' : 'text/javascript';
    return new Response('', { status: 200, headers: { 'content-type': type } });
  },
  location: { href: 'http://device.test/web/', origin: 'http://device.test' },
  postMessage: (msg) => posted.push(msg),
};

await import('../src/js/engine.worker.js');

let builds = [];
async function load(id, modelId = 'model-a') {
  posted.length = 0;
  probed.length = 0;
  await self.onmessage({ data: { type: 'load', id, modelId, english: true, useGpu: false } });
  return posted;
}

test('a session build that fails on a missing runtime file names it', async () => {
  globalThis.__pipeline = async () => { throw new Error('no available backend found'); };
  answers = { '/web/vendor/ort/ort-wasm-simd-threaded.wasm': 'missing' };
  maxInFlight = 0;
  const out = await load(1);
  assert.equal(out[0].type, 'building', 'the page is told a silent build may follow');
  const err = out.find((m) => m.type === 'error');
  assert.equal(err.id, 1);
  assert.equal(err.missing, 'vendor/ort/ort-wasm-simd-threaded.wasm');
  assert.deepEqual(probed, [
    '/web/vendor/ort/ort-wasm-simd-threaded.mjs',
    '/web/vendor/ort/ort-wasm-simd-threaded.wasm',
  ], 'probes stop at the first bad file');
  assert.equal(maxInFlight, 1, 'one request to the device at a time');
});

test('a session build that fails while the device is unreachable reports a lost connection', async () => {
  answers = { '/web/vendor/ort/ort-wasm-simd-threaded.mjs': 'offline' };
  const err = (await load(2)).find((m) => m.type === 'error');
  assert.equal(err.lost, true);
  assert.equal(err.missing, undefined);
});

test('a session build that fails with every device file present is a download problem', async () => {
  answers = {};
  const err = (await load(3)).find((m) => m.type === 'error');
  assert.equal(err.missing, undefined);
  assert.equal(err.lost, undefined);
  assert.equal(err.error, 'no available backend found');
  assert.equal(probed.length, 4, 'every runtime file was checked');
});

test('a failed build is not reused, and a finished load echoes its request id', async () => {
  builds = [];
  globalThis.__pipeline = async (task, modelId) => {
    builds.push(modelId);
    return async () => ({ text: '' });
  };
  const out = await load(4);
  assert.deepEqual(builds, ['model-a'], 'the model is built again after the failures');
  const done = out.find((m) => m.type === 'loaded');
  assert.equal(done.id, 4);
});

test('a request for one model is never handed another model\'s build', async () => {
  builds = [];
  globalThis.__pipeline = async (task, modelId) => {
    builds.push(modelId);
    if (modelId === 'model-b') throw new Error('Failed to fetch');
    return async () => ({ text: '' });
  };
  const b = (await load(5, 'model-b')).find((m) => m.type === 'error');
  assert.equal(b.id, 5);
  const a = (await load(6, 'model-a')).find((m) => m.type === 'loaded');
  assert.equal(a.id, 6, 'model A loads after model B failed, not B\'s rejection');
  assert.deepEqual(builds, ['model-b', 'model-a']);
});

test('transcribing with the ready model while another builds uses the right model', async () => {
  builds = [];
  let finishB;
  globalThis.__pipeline = async (task, modelId) => {
    builds.push(modelId);
    if (modelId === 'model-b') await new Promise((r) => { finishB = r; });
    return async () => ({ text: 'heard by ' + modelId });
  };
  posted.length = 0;
  // model-a is ready (previous test). Start building model-b, and while that
  // is still going, ask model-a for a transcription.
  const loadB = self.onmessage({ data: { type: 'load', id: 20, modelId: 'model-b', english: true, useGpu: false } });
  await new Promise((r) => setImmediate(r));
  const tr = self.onmessage({ data: { type: 'transcribe', id: 21, modelId: 'model-a', english: true, audio: new Float32Array(16000) } });
  await new Promise((r) => setImmediate(r));
  finishB();
  await Promise.all([loadB, tr]);
  const result = posted.find((m) => m.type === 'result' && m.id === 21);
  assert.ok(result, 'the transcription finished');
  assert.equal(result.text, 'heard by model-a', 'not run on the model being built');
});
