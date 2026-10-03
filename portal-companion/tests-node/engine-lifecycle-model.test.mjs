// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC
//
// Model-based test of the engine facade's lifecycle (src/js/engine.js): drive
// random sequences of load / transcribe / remove / worker failure / worker
// replies / clock ticks against a fake worker, and check after every step:
//   - every request settles at most once, and all of them settle by the end
//   - nothing asked for before a removal becomes ready after it, and a
//     removal never restarts a queued load (no new worker without a new load)
//   - at most one load is outstanding in the worker at any time
//   - a load is only timed out after a full allowance of silence: the long
//     one after the worker announced a silent phase (or never said hello)
//   - once everything has settled, no timer fires (a ready worker survives)
// Seeded, so a failure names the seed that reproduces it.
//
//   npm test

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { workers, device, watch, flush, settle } from './support/page-fakes.mjs';

const engine = await import('../src/js/engine.js');
const MODELS = engine.listModels().map((m) => m.id).slice(0, 2);
const { LOAD_IDLE_MS, LOAD_BUSY_MS } = engine;

function rng(seed) {
  let a = seed >>> 0;
  return () => {
    a = (a + 0x6d2b79f5) >>> 0;
    let t = a;
    t = Math.imul(t ^ (t >>> 15), t | 1);
    t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}

const SEEDS = 150;
const STEPS = 40;

test('random lifecycles keep the invariants', async (t) => {
  t.mock.timers.enable({ apis: ['setTimeout'] });
  device.answer = 'ok';

  for (let seed = 1; seed <= SEEDS; seed++) {
    const rand = rng(seed);
    const pick = (arr) => arr[Math.floor(rand() * arr.length)];
    const where = (step, what) => `seed ${seed}, step ${step}: ${what}`;

    // Start every run from a clean engine.
    await engine.dropDownload();
    await flush();

    let removals = 0;
    let now = 0;
    const calls = [];                 // { type, gen, w, resolvedAtRemoval }
    const answered = new Set();       // request ids we have replied to
    const hello = new WeakSet();      // workers that said hello
    // The silence model, per load in flight in the current worker.
    let track = null;                 // { worker, id, lastAt, phase }
    let workersAtRemoval = workers.length;
    let loadsSinceRemoval = 0;

    const cur = () => {
      const w = workers[workers.length - 1];
      return w && !w.terminated ? w : null;
    };
    const outstandingLoads = (w) => w.loads().filter((m) => !answered.has(m.id));
    const outstandingTranscribes = (w) => w.posted.filter((m) => m.type === 'transcribe' && !answered.has(m.id));
    const signal = (w, phase) => {
      if (track && track.worker === w) { track.lastAt = now; if (phase) track.phase = phase; }
    };

    const check = (step) => {
      for (const c of calls) assert.ok(c.w.settles <= 1, where(step, 'a request settled twice'));
      for (const c of calls) {
        if (c.type === 'load' && c.gen < removals && c.w.state === 'resolved') {
          assert.ok(c.resolvedAtRemoval, where(step, 'a load asked for before a removal became ready after it'));
        }
      }
      if (loadsSinceRemoval === 0) {
        assert.equal(workers.length, workersAtRemoval, where(step, 'a removal restarted a queued load'));
        assert.equal(engine.loaded(), false, where(step, 'ready after a removal with no new load'));
      }
      const w = cur();
      if (w) {
        const out = outstandingLoads(w);
        assert.ok(out.length <= 1, where(step, 'two loads outstanding in the worker'));
        // Start tracking silence for a newly sent load.
        if (out.length === 1 && (!track || track.id !== out[0].id)) {
          track = { worker: w, id: out[0].id, lastAt: now, phase: 'loading' };
        }
      }
    };

    // A stalled load was rejected: it must have been silent for a full allowance.
    const checkStall = (step, c) => {
      if (c.type !== 'load' || c.w.state !== 'rejected' || c.stallChecked) return;
      c.stallChecked = true;
      if (!/stopped responding|download stopped/.test(c.w.value && c.w.value.message)) return;
      assert.ok(track, where(step, 'a stall with no load in flight'));
      const longOne = track.phase === 'building' || !hello.has(track.worker);
      const allowed = longOne ? LOAD_BUSY_MS : LOAD_IDLE_MS;
      assert.ok(now - track.lastAt >= allowed,
        where(step, `timed out after ${now - track.lastAt} ms of silence; the allowance was ${allowed} ms`));
    };

    for (let step = 0; step < STEPS; step++) {
      const w = cur();
      const r = rand();
      if (r < 0.16) {
        calls.push({ type: 'load', gen: removals, w: watch(engine.load(pick(MODELS))) });
        loadsSinceRemoval++;
      } else if (r < 0.24) {
        calls.push({ type: 'transcribe', gen: removals, w: watch(engine.transcribe(new Float32Array(160), pick(MODELS))) });
      } else if (r < 0.30) {
        for (const c of calls) c.resolvedAtRemoval = c.resolvedAtRemoval || c.w.state === 'resolved';
        removals++;
        engine.dropDownload();
        await flush();
        for (const c of calls) c.resolvedAtRemoval = c.resolvedAtRemoval || (c.w.state === 'resolved' && c.gen === removals);
        workersAtRemoval = workers.length;
        loadsSinceRemoval = 0;
        track = null;
      } else if (r < 0.38 && w && !hello.has(w)) {
        hello.add(w);
        w.hello();
        signal(w);
      } else if (r < 0.52 && w && outstandingLoads(w).length) {
        const m = pick(['building', 'progress', 'progress', 'status']);
        if (m === 'building') { w.emit({ type: 'building' }); signal(w, 'building'); }
        else if (m === 'status') { w.emit({ type: 'status', phase: 'preparing' }); signal(w, 'building'); }
        else {
          const done = rand() < 0.5;
          w.emit({ type: 'progress', pct: Math.floor(rand() * 100), label: 'f', fileDone: done });
          signal(w, done ? 'building' : 'loading');
        }
      } else if (r < 0.64 && w && outstandingLoads(w).length) {
        const m = outstandingLoads(w)[0];
        answered.add(m.id);
        if (rand() < 0.75) w.emit({ type: 'loaded', id: m.id, device: 'wasm' });
        else w.emit({ type: 'error', id: m.id, error: 'x', kind: pick(['card', 'connection', 'download', 'engine']) });
      } else if (r < 0.72 && w && outstandingTranscribes(w).length) {
        const m = pick(outstandingTranscribes(w));
        answered.add(m.id);
        if (rand() < 0.8) w.emit({ type: 'result', id: m.id, text: 'hi' });
        else w.emit({ type: 'error', id: m.id, error: 'x', kind: 'engine' });
      } else if (r < 0.77 && w) {
        if (rand() < 0.5) w.fail({ type: 'error' });
        else w.fail({ type: 'error', message: 'out of memory' });
      } else if (r < 0.80 && workers.length > 1) {
        // A dropped worker's late reply.
        const old = pick(workers.filter((x) => x.terminated));
        if (old) old.emit({ type: 'loaded', id: (old.lastLoad() || { id: 0 }).id, device: 'wasm' });
      } else {
        const ms = rand() < 0.6
          ? Math.floor(rand() * 2 * 60 * 1000)
          : LOAD_IDLE_MS + Math.floor(rand() * LOAD_BUSY_MS);
        // Tick in pieces so a stall is seen at the moment it happens.
        let left = ms;
        while (left > 0) {
          const d = Math.min(left, 30 * 1000);
          now += d; left -= d;
          t.mock.timers.tick(d);
          await flush();
          for (const c of calls) checkStall(step, c);
        }
      }
      await settle(); await flush();
      for (const c of calls) checkStall(step, c);
      check(step);
    }

    // Drain: answer everything the live worker is waiting on, then let time
    // pass, until every request has settled.
    for (let round = 0; round < 30 && calls.some((c) => c.w.state === 'pending'); round++) {
      const w = cur();
      if (w) {
        if (!hello.has(w)) { hello.add(w); w.hello(); }
        for (const m of outstandingLoads(w)) { answered.add(m.id); w.emit({ type: 'loaded', id: m.id, device: 'wasm' }); }
        for (const m of outstandingTranscribes(w)) { answered.add(m.id); w.emit({ type: 'result', id: m.id, text: 'hi' }); }
      }
      await settle(); await flush();
      if (round % 3 === 2) { now += LOAD_BUSY_MS; t.mock.timers.tick(LOAD_BUSY_MS); await settle(); await flush(); }
      check('drain');
    }
    for (const c of calls) {
      assert.notEqual(c.w.state, 'pending', where('end', `a ${c.type} never settled`));
      assert.equal(c.w.settles, 1, where('end', 'a request did not settle exactly once'));
    }

    // Everything settled: no timer may fire now.
    const w = cur();
    if (w && engine.loaded()) {
      t.mock.timers.tick(LOAD_BUSY_MS * 3);
      await flush();
      assert.equal(w.terminated, false, where('end', 'a timer fired after everything settled'));
      assert.equal(engine.loaded(), true, where('end', 'ready state lost with nothing happening'));
    }
  }
});
