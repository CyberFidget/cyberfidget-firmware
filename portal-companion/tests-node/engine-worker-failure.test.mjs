// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC
//
// The transcription engine facade (src/js/engine.js) against a fake Worker:
// a worker that fails to load is diagnosed (incomplete card copy vs a device
// we can't reach) and dropped so the next load starts fresh; a quiet load
// times out but a long legitimate build does not; overlapping loads don't
// answer each other; removing the download fails everything waiting and
// never lets a queued load mark anything ready; every failure gets plain
// words, and only a real download failure is blamed on the internet.
//
//   npm test

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { workers, device, dbWrites, stores, storage, doubleSettles, watch, flush, settle } from './support/page-fakes.mjs';

const engine = await import('../src/js/engine.js');
const [MODEL_A, MODEL_B] = engine.listModels().map((m) => m.id);
const MIN = 60 * 1000;
const last = () => workers[workers.length - 1];

test('a worker script the device says is missing reports the card copy as incomplete', async () => {
  device.answer = 'missing';
  const p = watch(engine.load(MODEL_A));
  await flush();
  assert.equal(workers.length, 1);
  assert.equal(last().lastLoad().modelId, MODEL_A);

  // A script load failure fires a bare error event: no message.
  last().fail({ type: 'error' });
  assert.equal(last().terminated, true, 'the dead worker is dropped at once');
  await settle(); await flush();

  assert.deepEqual(device.probed, ['/web/engine.worker.js', '/api/status'],
    'the device was asked about the worker file, and confirmed it was the device answering');
  assert.equal(p.state, 'rejected');
  assert.equal(p.value.kind, 'card');
  assert.match(p.value.message, /memory card/);
  assert.match(p.value.message, /engine\.worker\.js/);
  assert.doesNotMatch(p.value.message, /unknown|internet/i);
  assert.equal(engine.loaded(), false);
});

test('retrying after a failed worker starts a fresh worker and settles', async () => {
  const dead = last();
  const p = watch(engine.load(MODEL_A));
  await flush();
  assert.equal(workers.length, 2, 'a new worker is created, not the dead one reused');

  // Late events from the dropped worker must not touch the new attempt.
  dead.fail({ type: 'error' });
  dead.emit({ type: 'loaded', id: last().lastLoad().id, device: 'wasm' });
  await settle(); await flush();
  assert.equal(p.state, 'pending');

  last().hello();
  last().loaded();
  await flush();
  assert.equal(p.state, 'resolved');
  assert.equal(engine.loaded(), true);
  assert.equal(engine.backend(), 'wasm');
});

test('a worker script the device never answers for says the device could not be reached', async (t) => {
  t.mock.timers.enable({ apis: ['setTimeout'] });
  device.answer = 'offline';
  device.probed.length = 0;
  const p = watch(engine.load(MODEL_B));
  await flush();
  last().fail({ type: 'error' });
  // The probe retries a missing answer twice, with pauses, before giving up.
  for (let i = 0; i < 10 && p.state === 'pending'; i++) { await settle(); t.mock.timers.tick(5000); await flush(); }
  assert.equal(device.probed.length, 3, 'tried three times');
  assert.equal(p.state, 'rejected');
  assert.equal(p.value.kind, 'connection');
  assert.match(p.value.message, /reach your Cyber Fidget/);
  assert.doesNotMatch(p.value.message, /memory card|internet|lost/i);
});

test('a worker that fails although the device has its file says to reload', async () => {
  device.answer = 'ok';
  const p = watch(engine.load(MODEL_B));
  await flush();
  last().fail({ type: 'error' });
  await settle(); await flush();
  assert.equal(p.state, 'rejected');
  assert.equal(p.value.kind, 'engine');
  assert.match(p.value.message, /Reload/);
  assert.doesNotMatch(p.value.message, /memory card|internet/i);
});

test('a download that goes quiet times out as a download problem; the next load recovers', async (t) => {
  t.mock.timers.enable({ apis: ['setTimeout'] });
  const p = watch(engine.load(MODEL_B));
  await flush();
  const w = last();
  w.hello();
  w.started();
  // Progress keeps it alive: the clock restarts on every message.
  t.mock.timers.tick(engine.LOAD_IDLE_MS - 1);
  w.emit({ type: 'progress', pct: 40, label: 'model.onnx', fileDone: false });
  t.mock.timers.tick(engine.LOAD_IDLE_MS - 1);
  await flush();
  assert.equal(p.state, 'pending', 'still alive while the download keeps reporting');

  t.mock.timers.tick(1);
  await flush();
  assert.equal(p.state, 'rejected');
  assert.equal(p.value.kind, 'download', 'a stalled download keeps the internet advice');
  assert.equal(w.terminated, true);
  assert.equal(engine.loaded(), false);

  const again = watch(engine.load(MODEL_B));
  await flush();
  assert.notEqual(last(), w, 'the next load starts a fresh worker');
  last().hello();
  last().loaded();
  await flush();
  assert.equal(again.state, 'resolved');
});

test('a worker that goes quiet before any download says setup stopped responding', async (t) => {
  t.mock.timers.enable({ apis: ['setTimeout'] });
  const p = watch(engine.load(MODEL_A));
  await flush();
  // Same (live) worker, which said hello long ago: the short allowance applies.
  t.mock.timers.tick(engine.LOAD_IDLE_MS);
  await flush();
  assert.equal(p.state, 'rejected');
  assert.equal(p.value.kind, 'engine');
  assert.match(p.value.message, /stopped responding/);
});

test('a long silent build after the last file finishes is not cut off', async (t) => {
  t.mock.timers.enable({ apis: ['setTimeout'] });
  const p = watch(engine.load(MODEL_A));
  await flush();
  const w = last();
  w.hello();
  w.started();
  w.emit({ type: 'building' });
  // An early file finishes, a later one downloads and then finishes too...
  w.emit({ type: 'progress', pct: 100, label: 'config.json', fileDone: true });
  w.emit({ type: 'progress', pct: 50, label: 'model.onnx', fileDone: false });
  t.mock.timers.tick(engine.LOAD_IDLE_MS - 1);
  w.emit({ type: 'progress', pct: 100, label: 'model.onnx', fileDone: true });
  // ...then the session build computes silently for a long time.
  t.mock.timers.tick(12 * MIN);
  await flush();
  assert.equal(p.state, 'pending', 'a slow phone building the session is left alone');
  assert.equal(w.terminated, false);
  w.emit({ type: 'status', phase: 'warming' });
  t.mock.timers.tick(12 * MIN);
  w.loaded();
  await flush();
  assert.equal(p.state, 'resolved');
});

test('an older worker that never says hello gets the long allowance throughout', async (t) => {
  t.mock.timers.enable({ apis: ['setTimeout'] });
  device.answer = 'ok';
  // Fail the current worker so the next load starts one that stays silent.
  last().fail({ type: 'error', message: 'boom' });
  await flush();
  const p = watch(engine.load(MODEL_B));
  await flush();
  const old = last();
  t.mock.timers.tick(10 * MIN);
  old.emit({ type: 'progress', pct: 30, label: 'model.onnx' });   // no fileDone either
  t.mock.timers.tick(engine.LOAD_BUSY_MS - 1);
  await flush();
  assert.equal(p.state, 'pending', 'an older worker cannot announce its silent build');
  // Its replies carry no kind: its own words, and the old internet advice.
  old.emit({ type: 'error', id: old.lastLoad().id, error: 'Failed to fetch' });
  await flush();
  assert.equal(p.state, 'rejected');
  assert.equal(p.value.kind, undefined);
  assert.equal(p.value.message, 'Failed to fetch');
});

test('a long silence still ends eventually', async (t) => {
  t.mock.timers.enable({ apis: ['setTimeout'] });
  const p = watch(engine.load(MODEL_B));
  await flush();
  const w = last();
  w.emit({ type: 'status', phase: 'warming' });
  t.mock.timers.tick(engine.LOAD_BUSY_MS - 1);
  await flush();
  assert.equal(p.state, 'pending');
  t.mock.timers.tick(1);
  await flush();
  assert.equal(p.state, 'rejected');
  assert.match(p.value.message, /stopped responding/);
  assert.equal(w.terminated, true);
});

test('a finished load leaves no timer behind', async (t) => {
  t.mock.timers.enable({ apis: ['setTimeout'] });
  const p = watch(engine.load(MODEL_A));
  await flush();
  const w = last();
  w.hello();
  w.loaded();
  await flush();
  assert.equal(p.state, 'resolved');
  t.mock.timers.tick(engine.LOAD_BUSY_MS * 2);
  await flush();
  assert.equal(w.terminated, false, 'a ready worker is not timed out');
  assert.equal(engine.loaded(), true);
});

test('overlapping loads run one at a time and only their own reply settles them', async () => {
  const w = last();
  const before = w.loads().length;
  const b = watch(engine.load(MODEL_B));
  const a = watch(engine.load(MODEL_A));
  await flush();
  assert.equal(w.loads().length, before + 1, 'the second load waits for the first');
  const bReq = w.lastLoad();
  assert.equal(bReq.modelId, MODEL_B);

  w.emit({ type: 'loaded', id: bReq.id + 1000, device: 'wasm' });
  await flush();
  assert.equal(b.state, 'pending', 'a reply for another request settles nothing');

  w.emit({ type: 'loaded', id: bReq.id, device: 'wasm' });
  await flush();
  assert.equal(b.state, 'resolved');
  assert.equal(a.state, 'pending', 'B finishing does not mark A ready');
  const aReq = w.lastLoad();
  assert.equal(aReq.modelId, MODEL_A);
  w.emit({ type: 'loaded', id: aReq.id, device: 'wasm' });
  await flush();
  assert.equal(a.state, 'resolved');
});

test('a failed load does not block the next queued load', async () => {
  const w = last();
  const b = watch(engine.load(MODEL_B));
  const a = watch(engine.load(MODEL_A));
  await flush();
  w.emit({ type: 'error', id: w.lastLoad().id, error: 'x', kind: 'download' });
  await flush();
  assert.equal(b.state, 'rejected');
  assert.equal(w.lastLoad().modelId, MODEL_A, 'the queued load went ahead');
  w.loaded();
  await flush();
  assert.equal(a.state, 'resolved');
});

test('removing the download fails everything waiting and cancels queued loads', async () => {
  const w = last();
  const count = workers.length;
  const tr = watch(engine.transcribe(new Float32Array(16000), MODEL_A));
  const running = watch(engine.load(MODEL_B));
  const queued = watch(engine.load(MODEL_A));
  await flush();
  assert.equal(running.state, 'pending');

  await engine.dropDownload();
  await flush();
  assert.equal(w.terminated, true);
  assert.equal(tr.state, 'rejected', 'a running transcription is not left hanging');
  assert.equal(running.state, 'rejected', 'a running load is not left hanging');
  assert.equal(running.value.kind, 'removed');
  assert.equal(queued.state, 'rejected', 'a load queued before the removal does not download again');
  assert.equal(queued.value.kind, 'removed');
  assert.equal(workers.length, count, 'no new worker was started for it');
  assert.equal(engine.loaded(), false);

  // A load asked for AFTER the removal is a new request and goes ahead.
  const fresh = watch(engine.load(MODEL_B));
  await flush();
  assert.equal(workers.length, count + 1);
  w.emit({ type: 'loaded', id: last().lastLoad().id, device: 'wasm' });   // the dropped worker
  await flush();
  assert.equal(fresh.state, 'pending', 'the dropped worker cannot answer for the new one');
  last().hello();
  last().loaded();
  await flush();
  assert.equal(fresh.state, 'resolved');
});

test('a removal right as a load finishes leaves nothing marked ready', async () => {
  const p = watch(engine.load(MODEL_A));
  await flush();
  const w = last();
  dbWrites.length = 0;
  w.loaded();
  const removal = engine.dropDownload();     // before the load records itself
  await removal; await flush();
  assert.equal(p.state, 'rejected');
  assert.equal(p.value.kind, 'removed');
  assert.equal(engine.loaded(), false);
  const readyWrites = dbWrites.filter(([op, , key]) => op === 'put' && String(key).startsWith('engineReadyFor:'));
  assert.deepEqual(readyWrites, [], 'no ready flag written after the removal');
});

test('worker replies say what failed in plain words', async () => {
  let p = watch(engine.load(MODEL_A));
  await flush();
  const w = last();
  w.hello();
  w.emit({ type: 'error', id: w.lastLoad().id, error: 'x', kind: 'card', file: 'vendor/ort/ort-wasm-simd-threaded.wasm' });
  await flush();
  assert.equal(p.value.kind, 'card');
  assert.match(p.value.message, /vendor\/ort\/ort-wasm-simd-threaded\.wasm/);

  const cases = [
    ['connection', /reach your Cyber Fidget/],
    ['download', /could not be downloaded/],
    ['engine', /could not start in this browser/],
    [undefined, /could not start in this browser/],   // a current worker always means engine
  ];
  for (const [kind, words] of cases) {
    p = watch(engine.load(MODEL_A));
    await flush();
    w.emit({ type: 'error', id: w.lastLoad().id, error: 'no available backend found', kind });
    await flush();
    assert.equal(p.state, 'rejected');
    assert.equal(p.value.kind, kind || 'engine');
    assert.match(p.value.message, words);
    assert.doesNotMatch(p.value.message, /backend/, 'no internal error text');
  }
});

test('a failed transcription and a crashing worker get plain words too', async () => {
  let p = watch(engine.load(MODEL_A));
  await flush();
  const w = last();
  w.loaded();
  await flush();
  const tr = watch(engine.transcribe(new Float32Array(16000), MODEL_A));
  w.emit({ type: 'error', id: w.posted[w.posted.length - 1].id, error: 'Aborted()', kind: 'engine' });
  await flush();
  assert.equal(tr.state, 'rejected');
  assert.equal(tr.value.kind, 'engine');
  assert.match(tr.value.message, /Transcription failed/);

  p = watch(engine.load(MODEL_B));
  await flush();
  w.fail({ type: 'error', message: 'out of memory' });
  await flush();
  assert.equal(p.state, 'rejected');
  assert.equal(p.value.kind, 'engine');
  assert.match(p.value.message, /stopped unexpectedly/);
  assert.equal(w.terminated, true);
});

test('a load queued behind a long transcription is not timed out while it waits', async (t) => {
  t.mock.timers.enable({ apis: ['setTimeout'] });
  const first = watch(engine.load(MODEL_A));
  await flush();
  const w = last();
  w.hello();
  w.emit({ type: 'started', id: w.lastLoad().id });
  w.loaded();
  await flush();
  assert.equal(first.state, 'resolved');

  // A long transcription keeps the worker busy; a load for another model
  // waits behind it in the worker's queue.
  const tr = watch(engine.transcribe(new Float32Array(16000 * 600), MODEL_A));
  const trId = w.posted[w.posted.length - 1].id;
  const p = watch(engine.load(MODEL_B));
  await flush();
  t.mock.timers.tick(25 * MIN);
  await flush();
  assert.equal(p.state, 'pending', 'waiting in the queue is not silence');
  assert.equal(tr.state, 'pending');
  assert.equal(w.terminated, false, 'the busy worker is not thrown away');

  // The transcription finishes, the worker reaches the load and says so.
  w.emit({ type: 'result', id: trId, text: 'a long note' });
  w.emit({ type: 'started', id: w.lastLoad().id });
  t.mock.timers.tick(engine.LOAD_IDLE_MS - 1);
  w.loaded();
  await flush();
  assert.equal(tr.state, 'resolved');
  assert.equal(p.state, 'resolved');
});

test('a queued load whose turn never comes after the queue empties still times out', async (t) => {
  t.mock.timers.enable({ apis: ['setTimeout'] });
  const w = last();
  const tr = watch(engine.transcribe(new Float32Array(16000), MODEL_B));
  const trId = w.posted[w.posted.length - 1].id;
  const p = watch(engine.load(MODEL_A));
  await flush();
  w.emit({ type: 'result', id: trId, text: 'hi' });
  t.mock.timers.tick(engine.LOAD_IDLE_MS);
  await flush();
  assert.equal(tr.state, 'resolved');
  assert.equal(p.state, 'rejected');
  assert.match(p.value.message, /stopped responding/);
});

test('checking for an old-style download at the moment it is removed leaves nothing ready', async () => {
  stores.settings.clear();
  stores.settings.set('engineReadyFor', MODEL_A);    // the legacy flag
  const check = engine.isDownloaded(MODEL_A);        // migrates the legacy flag...
  const removal = engine.dropDownload();             // ...while the download is removed
  await Promise.all([check, removal]);
  await flush();
  const left = [...stores.settings.keys()].filter((k) => String(k).startsWith('engineReadyFor'));
  assert.deepEqual(left, [], 'no ready flag survives the removal');
  assert.equal(await engine.isDownloaded(MODEL_A), false);
});

test('the same, with storage answering slowly', async () => {
  storage.hops = () => 2;
  try {
    stores.settings.clear();
    stores.settings.set('engineReadyFor', MODEL_B);
    const removal = engine.dropDownload();
    const check = engine.isDownloaded(MODEL_B);      // asked after the removal began
    assert.equal(await check, false);
    await removal;
    const left = [...stores.settings.keys()].filter((k) => String(k).startsWith('engineReadyFor'));
    assert.deepEqual(left, []);
  } finally {
    storage.hops = () => 0;
  }
});

test('no request was ever settled twice in this file', () => {
  assert.deepEqual(doubleSettles, []);
});

test('progress from a transcription ahead of a queued load does not start the load\'s clock', async (t) => {
  t.mock.timers.enable({ apis: ['setTimeout'] });
  const first = watch(engine.load(MODEL_A));
  await flush();
  const w = last();
  w.hello(); w.started(); w.loaded();
  await flush();
  assert.equal(first.state, 'resolved');
  // A transcription for another model makes the worker build that model,
  // reporting progress - for the transcription, not for the load behind it.
  const tr = watch(engine.transcribe(new Float32Array(16000), MODEL_B));
  const trId = w.posted[w.posted.length - 1].id;
  const p = watch(engine.load(MODEL_B));
  await flush();
  w.emit({ type: 'building' });
  w.emit({ type: 'progress', pct: 40, label: 'model.onnx', fileDone: false });
  t.mock.timers.tick(20 * MIN);                       // then a long silent inference
  await flush();
  assert.equal(p.state, 'pending', 'the queued load has no deadline yet');
  w.emit({ type: 'result', id: trId, text: 'hi' });
  w.started(); w.loaded();
  await flush();
  assert.equal(tr.state, 'resolved');
  assert.equal(p.state, 'resolved');
});
