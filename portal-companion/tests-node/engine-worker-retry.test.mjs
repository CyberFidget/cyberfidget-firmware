// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { FakeWorker, workers, device, doubleSettles, watch, flush } from './support/page-fakes.mjs';

const engine = await import('../src/js/engine.js');
const [MODEL_A, MODEL_B] = engine.listModels().map((m) => m.id);
const RUNTIME_FILE = 'vendor/ort/ort-wasm-simd-threaded.wasm';
const last = () => workers[workers.length - 1];

test('Download succeeds in a new worker after a missing runtime file is restored', async (t) => {
  t.mock.timers.enable({ apis: ['setTimeout'] });
  await engine.dropDownload();
  // Like ONNX, this stand-in remembers aborted initialization for the life
  // of the worker, even after the required file becomes available again.
  class RuntimeWorker extends FakeWorker {
    postMessage(msg) {
      super.postMessage(msg);
      if (msg.type !== 'load') return;
      Promise.resolve().then(async () => {
        this.hello();
        this.started();
        const response = await fetch('/web/' + RUNTIME_FILE);
        if (!response.ok) this.aborted = true;
        if (this.aborted) {
          this.emit({ type: 'error', id: msg.id, kind: response.ok ? 'engine' : 'card', file: RUNTIME_FILE });
        } else this.loaded();
      });
    }
  }
  globalThis.Worker = RuntimeWorker;
  t.after(() => { globalThis.Worker = FakeWorker; });
  device.answer = 'missing';
  const failed = watch(engine.load(MODEL_A));
  await flush();
  const dead = last();
  assert.equal(failed.state, 'rejected');
  assert.equal(failed.value.kind, 'card');
  assert.match(failed.value.message, /ort-wasm-simd-threaded\.wasm/);
  assert.equal(dead.aborted, true);

  device.answer = 'ok'; // The person copies the missing file back to the card.
  const count = workers.length;
  const retry = watch(engine.load(MODEL_A)); // Download again, without a page reload.
  await flush();
  assert.equal(workers.length, count + 1, 'Download creates a new worker');
  assert.notEqual(last(), dead);
  assert.equal(dead.terminated, true);
  assert.equal(retry.state, 'resolved');
  assert.equal(engine.loaded(), true);
  assert.equal(engine.backend(), 'wasm');
  assert.equal(await engine.isDownloaded(MODEL_A), true);
  assert.equal(failed.settles, 1);
  assert.equal(retry.settles, 1);
  assert.deepEqual(doubleSettles, []);
  await engine.dropDownload();
});

for (const kind of ['card', 'connection', 'download', 'engine', undefined]) {
  test(`a ${kind || 'legacy'} load error drops the worker and settles dependent requests once`, async (t) => {
    t.mock.timers.enable({ apis: ['setTimeout'] });
    await engine.dropDownload();
    const first = watch(engine.load(MODEL_A));
    await flush();
    const dead = last();
    if (kind) dead.hello();
    dead.loaded();
    await flush();
    assert.equal(first.state, 'resolved');

    const failed = watch(engine.load(MODEL_B));
    await flush();
    dead.started();
    const dependent = watch(engine.transcribe(new Float32Array(160), MODEL_B));
    const trId = dead.posted.at(-1).id;
    const error = { type: 'error', id: dead.lastLoad().id, kind, file: RUNTIME_FILE, error: 'failed build' };
    dead.emit(error);
    assert.equal(dead.terminated, true, 'a failed build is discarded immediately');
    assert.equal(engine.loaded(), false);
    assert.equal(engine.backend(), null);
    await flush();
    assert.equal(failed.state, 'rejected');
    assert.equal(dependent.state, 'rejected');
    assert.equal(dependent.value, failed.value, 'both requests receive the teardown error');

    const count = workers.length;
    const retry = watch(engine.load(MODEL_B));
    await flush();
    assert.equal(workers.length, count + 1);
    // Stale messages cannot settle a dependent request twice or answer the retry.
    dead.emit(error);
    dead.emit({ type: 'result', id: trId, text: 'late' });
    dead.emit({ type: 'loaded', id: last().lastLoad().id, device: 'webgpu' });
    dead.fail({ message: 'late crash' });
    await flush();
    assert.equal(retry.state, 'pending');
    last().hello();
    last().loaded();
    await flush();
    assert.equal(retry.state, 'resolved');
    t.mock.timers.tick(engine.LOAD_BUSY_MS * 2);
    await flush();
    assert.equal(last().terminated, false, 'no failed-load timer survives to kill the retry');
    for (const request of [first, failed, dependent, retry]) assert.equal(request.settles, 1);
    assert.deepEqual(doubleSettles, []);
    await engine.dropDownload();
  });
}
