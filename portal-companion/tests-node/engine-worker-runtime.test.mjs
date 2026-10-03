// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC
//
// The worker side (src/js/engine.worker.js) with a stand-in transcription
// library: one serialized queue for loads and transcriptions (never two
// builds at once, never the wrong model's pipeline); a failed session build
// is diagnosed (missing runtime file, device unreachable, a failed internet
// download, or an engine problem); progress marks finished files so the page
// can allow for the silent build that follows the last one.
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

// Device answers per file; anything not listed is present. Internet URLs
// (other origin) answer per `internet`.
let answers = {};
let internet = 'ok';
const probed = [];
const posted = [];
globalThis.self = {
  fetch: async (url) => {
    url = String(url);
    if (/^https?:\/\/(?!device\.test)/.test(url)) {
      if (internet === 'offline') throw new TypeError('Failed to fetch');
      if (typeof internet === 'number') return new Response('', { status: internet });
      return new Response('weights', { status: 200 });
    }
    probed.push(url);
    if (url === '/api/status') {
      return new Response(JSON.stringify({ files: 3, totalBytes: 100, usedBytes: 50, clients: 1 }), { status: 200, headers: { 'content-type': 'application/json' } });
    }
    const a = answers[url] || 'ok';
    if (a === 'missing') return new Response('not found', { status: 404 });
    const type = url.endsWith('.wasm') ? 'application/wasm' : 'text/javascript';
    return new Response('', { status: 200, headers: { 'content-type': type } });
  },
  location: { href: 'http://device.test/web/', origin: 'http://device.test' },
  postMessage: (msg) => posted.push(msg),
};

await import('../src/js/engine.worker.js');

const tick = () => new Promise((r) => setImmediate(r));
async function until(pred) {
  for (let i = 0; i < 500 && !pred(); i++) await tick();
  assert.ok(pred(), 'timed out waiting');
}
const send = (msg) => self.onmessage({ data: msg });
const reply = (id) => posted.find((m) => m.id === id && (m.type === 'loaded' || m.type === 'error' || m.type === 'result'));

test('a session build that fails on a missing runtime file names it', async () => {
  globalThis.__pipeline = async () => { throw new Error('no available backend found'); };
  answers = { '/web/vendor/ort/ort-wasm-simd-threaded.wasm': 'missing' };
  posted.length = 0; probed.length = 0;
  send({ type: 'load', id: 1, modelId: 'model-a', english: true });
  await until(() => reply(1));
  assert.deepEqual(posted[0], { type: 'started', id: 1 }, 'the load says when its turn in the queue came');
  assert.equal(posted[1].type, 'building', 'the page is told a silent build may follow');
  const err = reply(1);
  assert.equal(err.type, 'error');
  assert.equal(err.kind, 'card');
  assert.equal(err.file, 'vendor/ort/ort-wasm-simd-threaded.wasm');
  assert.deepEqual(probed, [
    '/web/vendor/ort/ort-wasm-simd-threaded.mjs',
    '/web/vendor/ort/ort-wasm-simd-threaded.wasm',
    '/api/status',
  ], 'probes stop at the first bad file, then confirm it was the device');
});

test('a build that fails after an internet download failed is a download problem', async () => {
  answers = {};
  internet = 'offline';
  globalThis.__pipeline = async () => {
    await self.fetch('https://models.example/model.onnx');   // the library downloading
  };
  posted.length = 0;
  send({ type: 'load', id: 2, modelId: 'model-a', english: true });
  await until(() => reply(2));
  assert.equal(reply(2).kind, 'download');
  internet = 'ok';
});

test('a build that fails with the files present and no download failure is an engine problem', async () => {
  globalThis.__pipeline = async () => {
    await self.fetch('https://models.example/model.onnx');   // downloads fine
    throw new Error('no available backend found');
  };
  posted.length = 0; probed.length = 0;
  send({ type: 'load', id: 3, modelId: 'model-a', english: true });
  await until(() => reply(3));
  assert.equal(reply(3).kind, 'engine');
  assert.equal(probed.length, 4, 'every runtime file was checked first');
});

test('progress marks finished files, and "preparing" is announced again if the download resumes', async () => {
  globalThis.__pipeline = async (task, modelId, opts) => {
    const cb = opts.progress_callback;
    cb({ status: 'progress', file: 'config.json', loaded: 10, total: 10 });
    cb({ status: 'progress', file: 'model.onnx', loaded: 50, total: 100 });
    cb({ status: 'progress', file: 'model.onnx', loaded: 100, total: 100 });
    return async () => ({ text: '' });
  };
  posted.length = 0;
  send({ type: 'load', id: 4, modelId: 'model-a', english: true });
  await until(() => reply(4));
  const seq = posted.filter((m) => m.type === 'progress' || m.type === 'status')
    .map((m) => (m.type === 'progress' ? 'p' + m.pct + (m.fileDone ? 'done' : '') : m.phase));
  assert.deepEqual(seq, ['p100done', 'preparing', 'p55', 'p100done', 'preparing', 'warming']);
  assert.equal(reply(4).type, 'loaded');
  assert.equal(reply(4).device, 'wasm');
});

test('loads and transcriptions share one queue: one build at a time, always the right model', async () => {
  let active = 0; let maxActive = 0;
  const builds = [];
  globalThis.__pipeline = async (task, modelId) => {
    active++; maxActive = Math.max(maxActive, active);
    builds.push(modelId);
    await tick(); await tick();
    active--;
    return async () => ({ text: 'heard by ' + modelId });
  };
  posted.length = 0;
  // model-a is ready from the last test. Interleave without waiting.
  send({ type: 'load', id: 10, modelId: 'model-b', english: true });
  send({ type: 'transcribe', id: 11, modelId: 'model-a', english: true, audio: new Float32Array(16000) });
  send({ type: 'load', id: 12, modelId: 'model-b', english: true });
  send({ type: 'transcribe', id: 13, modelId: 'model-b', english: true, audio: new Float32Array(16000) });
  await until(() => [10, 11, 12, 13].every((id) => reply(id)));
  assert.equal(maxActive, 1, 'never two builds at once');
  assert.deepEqual(builds, ['model-b', 'model-a', 'model-b'], 'built in request order, reused when current');
  assert.equal(reply(11).text, 'heard by model-a');
  assert.equal(reply(13).text, 'heard by model-b');
  const order = posted.filter((m) => [10, 11, 12, 13].includes(m.id) && ['loaded', 'result', 'error'].includes(m.type)).map((m) => m.id);
  assert.deepEqual(order, [10, 11, 12, 13], 'answered in request order');
});

test('a failed job does not block the next one, and a failed build is not kept', async () => {
  const builds = [];
  globalThis.__pipeline = async (task, modelId) => {
    builds.push(modelId);
    if (builds.length === 1) throw new Error('Aborted()');
    return async () => ({ text: 'ok' });
  };
  posted.length = 0;
  send({ type: 'load', id: 20, modelId: 'model-c', english: true });
  send({ type: 'load', id: 21, modelId: 'model-c', english: true });
  await until(() => reply(20) && reply(21));
  assert.equal(reply(20).type, 'error');
  assert.equal(reply(21).type, 'loaded');
  assert.deepEqual(builds, ['model-c', 'model-c']);
});

test('a failing transcription is an engine problem', async () => {
  globalThis.__pipeline = async () => async () => { throw new Error('Aborted()'); };
  posted.length = 0;
  send({ type: 'load', id: 30, modelId: 'model-d', english: true });
  send({ type: 'transcribe', id: 31, modelId: 'model-d', english: true, audio: new Float32Array(16000) });
  await until(() => reply(31));
  assert.equal(reply(31).type, 'error');
  assert.equal(reply(31).kind, 'engine');
});

for (const status of [403, 408, 429, 503]) {
  test(`an internet answer of ${status} during the download makes a failed build a download problem`, async () => {
    answers = {};
    internet = status;
    globalThis.__pipeline = async () => {
      const res = await self.fetch('https://models.example/model.onnx');
      throw new Error('Could not load model: ' + res.status);
    };
    posted.length = 0;
    const id = 100 + status;
    send({ type: 'load', id, modelId: 'model-x', english: true });
    await until(() => reply(id));
    assert.equal(reply(id).kind, 'download');
    internet = 'ok';
  });
}

test('an optional file the internet does not have (404) is not a download failure', async () => {
  internet = 404;
  globalThis.__pipeline = async () => {
    await self.fetch('https://models.example/generation_config.json');   // optional, absent
    throw new Error('no available backend found');
  };
  posted.length = 0;
  send({ type: 'load', id: 120, modelId: 'model-x', english: true });
  await until(() => reply(120));
  assert.equal(reply(120).kind, 'engine');
  internet = 'ok';
});

test('a file the library calls done just short of its size counts as finished', async () => {
  globalThis.__pipeline = async (task, modelId, opts) => {
    const cb = opts.progress_callback;
    cb({ status: 'progress', file: 'model.onnx', loaded: 50, total: 100 });
    cb({ status: 'progress', file: 'model.onnx', loaded: 97, total: 100 });   // last count, a little short
    cb({ status: 'done', file: 'model.onnx' });
    cb({ status: 'done', file: 'tokenizer.json' });                          // never reported progress
    return async () => ({ text: '' });
  };
  posted.length = 0;
  send({ type: 'load', id: 130, modelId: 'model-y', english: true });
  await until(() => reply(130));
  const seq = posted.filter((m) => ['progress', 'status', 'building'].includes(m.type))
    .map((m) => (m.type === 'progress' ? 'p' + m.pct + (m.fileDone ? 'done' : '') : m.phase || m.type));
  assert.deepEqual(seq, ['building', 'p50', 'p97', 'p100done', 'preparing', 'building', 'warming']);
});

test('a load queued behind a transcription says "started" only when its turn comes', async () => {
  let release;
  let calls = 0;
  globalThis.__pipeline = async (task, modelId) => async () => {
    if (calls++ === 0) return { text: '' };              // the warm-up run
    await new Promise((r) => { release = r; });           // the long transcription
    return { text: 'long ' + modelId };
  };
  posted.length = 0;
  send({ type: 'load', id: 140, modelId: 'model-z', english: true });
  await until(() => reply(140));
  send({ type: 'transcribe', id: 141, modelId: 'model-z', english: true, audio: new Float32Array(16000) });
  send({ type: 'load', id: 142, modelId: 'model-z', english: true });
  await until(() => typeof release === 'function');    // the transcription is running
  for (let i = 0; i < 20; i++) await tick();
  assert.equal(posted.some((m) => m.type === 'started' && m.id === 142), false, 'still waiting behind the transcription');
  release();
  await until(() => reply(142));
  const idx = (pred) => posted.findIndex(pred);
  assert.ok(idx((m) => m.type === 'result' && m.id === 141) < idx((m) => m.type === 'started' && m.id === 142));
});
