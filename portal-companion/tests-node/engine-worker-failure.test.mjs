// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC
//
// The transcription engine facade (src/js/engine.js) against a fake Worker:
// a worker that fails to load must report the memory-card copy as incomplete,
// must be dropped so the next load starts a fresh one, and a load that goes
// quiet must time out instead of hanging the caller forever.
//
//   npm test

import { test, mock } from 'node:test';
import assert from 'node:assert/strict';

// ---- browser stand-ins (just enough for engine.js + db.js) ----

const workers = [];
class FakeWorker {
  constructor(url, opts) {
    this.url = url;
    this.opts = opts;
    this.posted = [];
    this.terminated = false;
    this.onmessage = null;
    this.onerror = null;
    workers.push(this);
  }
  postMessage(msg) { this.posted.push(msg); }
  terminate() { this.terminated = true; }
  // Test drivers.
  emit(data) { this.onmessage && this.onmessage({ data }); }
  fail(event) { this.onerror && this.onerror(event); }
}

// Minimal IndexedDB: every open succeeds, every transaction completes. Uses
// microtasks (not timers) so it keeps working while timers are mocked.
const later = (fn) => Promise.resolve().then(fn);
const fakeDb = {
  version: 1,
  objectStoreNames: { contains: () => true },
  close() {},
  transaction() {
    const t = {
      objectStore: () => ({
        put: () => ({ result: undefined }),
        delete: () => ({ result: undefined }),
        get: () => { const r = {}; later(() => r.onsuccess && r.onsuccess()); return r; },
      }),
    };
    later(() => t.oncomplete && t.oncomplete());
    return t;
  },
};
globalThis.indexedDB = {
  open() {
    const req = {};
    later(() => { req.result = fakeDb; req.onsuccess && req.onsuccess(); });
    return req;
  },
};
globalThis.window = {};
globalThis.Worker = FakeWorker;

const engine = await import('../src/js/engine.js');
const [MODEL_A, MODEL_B] = engine.listModels().map((m) => m.id);

// Track a promise's state without awaiting it.
function watch(p) {
  const s = { state: 'pending', value: undefined };
  p.then((v) => { s.state = 'resolved'; s.value = v; },
    (e) => { s.state = 'rejected'; s.value = e; });
  return s;
}
const flush = async () => { for (let i = 0; i < 20; i++) await Promise.resolve(); };

test('a worker script that fails to load reports the card copy as incomplete', async () => {
  const p = watch(engine.load(MODEL_A));
  assert.equal(workers.length, 1);
  assert.equal(workers[0].posted[0].type, 'load');

  // A script load failure fires a bare error event: no message.
  workers[0].fail({ type: 'error' });
  await flush();

  assert.equal(p.state, 'rejected');
  assert.equal(p.value.cardIncomplete, true);
  assert.match(p.value.message, /memory card/);
  assert.match(p.value.message, /engine\.worker\.js/);
  assert.doesNotMatch(p.value.message, /unknown|internet/i);
  assert.equal(workers[0].terminated, true, 'the dead worker is terminated');
  assert.equal(engine.loaded(), false);
});

test('retrying after a failed worker starts a fresh worker and settles', async () => {
  const p = watch(engine.load(MODEL_A));
  assert.equal(workers.length, 2, 'a new worker is created, not the dead one reused');
  assert.equal(workers[1].posted[0].type, 'load');

  // A late event from the dropped worker must not touch the new attempt.
  workers[0].fail({ type: 'error' });
  workers[0].emit({ type: 'error', error: 'stale' });
  await flush();
  assert.equal(p.state, 'pending');

  workers[1].emit({ type: 'loaded', device: 'wasm' });
  await flush();
  assert.equal(p.state, 'resolved');
  assert.equal(engine.loaded(), true);
  assert.equal(engine.backend(), 'wasm');
});

test('a load that hears nothing times out, drops the worker, and the next load recovers', async (t) => {
  t.mock.timers.enable({ apis: ['setTimeout'] });
  // MODEL_B is not ready yet, so this reuses the live worker from the last test.
  const p = watch(engine.load(MODEL_B));
  assert.equal(workers.length, 2);
  await flush();

  // Progress keeps it alive: the clock restarts on every message.
  t.mock.timers.tick(engine.LOAD_IDLE_MS - 1);
  workers[1].emit({ type: 'progress', pct: 40, label: 'model.onnx' });
  t.mock.timers.tick(engine.LOAD_IDLE_MS - 1);
  workers[1].emit({ type: 'status', phase: 'preparing' });
  t.mock.timers.tick(engine.LOAD_IDLE_MS - 1);
  await flush();
  assert.equal(p.state, 'pending', 'still alive while the worker keeps talking');

  // Silence for the full window: the waiter is rejected, not left hanging.
  t.mock.timers.tick(1);
  await flush();
  assert.equal(p.state, 'rejected');
  assert.match(p.value.message, /stopped responding/);
  assert.notEqual(p.value.cardIncomplete, true);
  assert.equal(workers[1].terminated, true);
  assert.equal(engine.loaded(), false);

  const again = watch(engine.load(MODEL_B));
  assert.equal(workers.length, 3, 'the next load starts a fresh worker');
  workers[2].emit({ type: 'loaded', device: 'wasm' });
  await flush();
  assert.equal(again.state, 'resolved');
});

test('a finished load leaves no timer behind', async (t) => {
  t.mock.timers.enable({ apis: ['setTimeout'] });
  const p = watch(engine.load(MODEL_A));
  assert.equal(workers.length, 3);
  workers[2].emit({ type: 'loaded', device: 'wasm' });
  await flush();
  assert.equal(p.state, 'resolved');
  t.mock.timers.tick(engine.LOAD_IDLE_MS * 2);
  await flush();
  assert.equal(workers[2].terminated, false, 'a ready worker is not timed out');
  assert.equal(engine.loaded(), true);
});

test('a library the worker could not load from the card is reported as incomplete', async () => {
  const p = watch(engine.load(MODEL_B));
  workers[2].emit({
    type: 'error',
    error: 'Failed to fetch dynamically imported module',
    missing: 'vendor/transformers.min.js',
  });
  await flush();
  assert.equal(p.state, 'rejected');
  assert.equal(p.value.cardIncomplete, true);
  assert.match(p.value.message, /memory card/);
  assert.match(p.value.message, /vendor\/transformers\.min\.js/);
});

test('an ordinary download error is not blamed on the card', async () => {
  const p = watch(engine.load(MODEL_B));
  workers[2].emit({ type: 'error', error: 'Failed to fetch' });
  await flush();
  assert.equal(p.state, 'rejected');
  assert.notEqual(p.value.cardIncomplete, true);
  assert.equal(p.value.message, 'Failed to fetch');
});

test('a crash inside a running worker keeps its message and drops the worker', async () => {
  const p = watch(engine.load(MODEL_B));
  workers[2].fail({ type: 'error', message: 'out of memory' });
  await flush();
  assert.equal(p.state, 'rejected');
  assert.notEqual(p.value.cardIncomplete, true);
  assert.match(p.value.message, /out of memory/);
  assert.equal(workers[2].terminated, true);

  const again = watch(engine.load(MODEL_B));
  assert.equal(workers.length, 4);
  workers[3].emit({ type: 'loaded', device: 'wasm' });
  await flush();
  assert.equal(again.state, 'resolved');
});
