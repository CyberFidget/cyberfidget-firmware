// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC
//
// Simulation test of the engine facade (src/js/engine.js) against a WELL-
// BEHAVED worker that follows the real worker's protocol (engine.worker.js):
// it says hello, runs ONE job at a time from a queue (so a load can wait
// minutes behind a long transcription), announces each load's turn with
// 'started', and stays within the documented silences - download progress
// at least every 2 minutes, each file finished by a fileDone progress (which
// the real worker also sends for the library's 'done' event when the last
// count fell short), silent build / warm-up stretches under 14 minutes, each
// announced. Storage answers slowly and interleaves. Random user actions:
// load, transcribe, remove, "is it downloaded?" (with an old-style flag to
// migrate), a crashing worker; the worker also fails downloads now and then.
// Checked throughout:
//   - a well-behaved worker's load is never timed out
//   - no request is settled twice (not even attempted) and all settle
//   - nothing asked for before a removal becomes ready after it; a removal
//     never restarts a queued load
//   - after a removal (and nothing since that could legitimately mark one),
//     no ready flag is left in storage
// Seeded, so a failure names the seed that reproduces it.
//
//   npm test

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { workers, stores, storage, storageIdle, doubleSettles, watch, settle, flush } from './support/page-fakes.mjs';

const engine = await import('../src/js/engine.js');
const MODELS = engine.listModels().map((m) => m.id).slice(0, 2);
const SEC = 1000;
const MIN = 60 * SEC;

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

// The simulated worker behind one FakeWorker.
class Sim {
  constructor(w, now, rand) {
    this.w = w;
    this.rand = rand;
    this.seen = 0;
    this.events = [];             // { at, msg } in time order
    this.freeAt = now;            // when the current job (if any) ends
    this.model = null;            // the pipeline it holds
    this.say(now, { type: 'hello', protocol: 2 });
  }
  say(at, msg) { this.events.push({ at, msg }); }
  within(ms) { return Math.max(1, Math.floor(this.rand() * ms)); }
  // A model build as the real worker reports it.
  build(t) {
    this.say(t, { type: 'building' });
    const files = 1 + Math.floor(this.rand() * 3);
    for (let f = 0; f < files; f++) {
      const chunks = 1 + Math.floor(this.rand() * 3);
      for (let c = 0; c < chunks; c++) {
        t += this.within(2 * MIN);
        this.say(t, { type: 'progress', pct: 50, label: 'file' + f, fileDone: false });
      }
      t += this.within(2 * MIN);
      // The last count may fall short; the library's 'done' then finishes it.
      this.say(t, { type: 'progress', pct: 100, label: 'file' + f, fileDone: true });
    }
    t += this.within(14 * MIN);                   // silent session build
    this.say(t, { type: 'status', phase: 'warming' });
    t += this.within(14 * MIN);                   // silent warm-up
    return t;
  }
  // Pick up newly posted requests; queue their events behind the current job.
  intake(now) {
    while (this.seen < this.w.posted.length) {
      const msg = this.w.posted[this.seen++];
      let t = Math.max(now, this.freeAt);
      if (msg.type === 'load') {
        this.say(t, { type: 'started', id: msg.id });
        if (this.model !== msg.modelId) {
          t += this.within(1 * MIN);              // the library loading from the card
          if (this.rand() < 0.15) {
            this.say(t, { type: 'building' });
            t += this.within(2 * MIN);
            this.model = null;
            this.say(t, { type: 'error', id: msg.id, error: 'x', kind: 'download' });
            this.freeAt = t;
            continue;
          }
          t = this.build(t);
          this.model = msg.modelId;
        }
        t += 1;
        this.say(t, { type: 'loaded', id: msg.id, device: 'wasm' });
      } else if (msg.type === 'transcribe') {
        if (this.model !== msg.modelId) { t = this.build(t + this.within(MIN)); this.model = msg.modelId; }
        t += this.within(20 * MIN);               // a long recording, silently
        if (this.rand() < 0.1) this.say(t, { type: 'error', id: msg.id, error: 'x', kind: 'engine' });
        else this.say(t, { type: 'result', id: msg.id, text: 'hi' });
      }
      this.freeAt = t;
    }
  }
  next() { return this.events.length ? this.events[0].at : Infinity; }
}

const SEEDS = 100;
const STEPS = 40;
const STALL = /stopped responding|download stopped/;

test('a well-behaved worker in random sessions keeps every invariant', async (t) => {
  t.mock.timers.enable({ apis: ['setTimeout'] });
  for (let seed = 1; seed <= SEEDS; seed++) {
    const rand = rng(seed);
    const pick = (arr) => arr[Math.floor(rand() * arr.length)];
    const where = (step, what) => `seed ${seed}, step ${step}: ${what}`;

    storage.hops = () => 0;
    await engine.dropDownload();
    await settle(); await flush();
    stores.settings.clear();
    storage.hops = () => Math.floor(rand() * 3);

    let now = 0;
    let removals = 0;
    let workersAtRemoval = workers.length;
    let loadsSinceRemoval = 0;
    let flagMayExist = false;      // something since the last removal may legitimately mark ready
    // The latest removal, the requests made before it, and whether anything
    // since could legitimately mark something ready again.
    let lastRemoval = null;
    const sims = new Map();
    const calls = [];

    const live = () => [...sims.values()].filter((s) => !s.w.terminated);
    const attach = () => {
      for (const w of workers) if (!w.terminated && !sims.has(w)) sims.set(w, new Sim(w, now, rand));
      for (const s of live()) s.intake(now);
    };
    // Let everything that is ready to run, run - including storage, which
    // answers after a few macrotask hops and may start further transactions.
    const quiet = async () => {
      for (let i = 0; i < 8; i++) { await settle(); await storageIdle(); await flush(); }
      attach();
    };

    const check = (step) => {
      assert.deepEqual(doubleSettles, [], where(step, 'a second attempt to settle a request'));
      for (const c of calls) {
        if (c.type === 'load' && c.w.state === 'rejected') {
          assert.doesNotMatch(c.w.value.message, STALL, where(step, 'a well-behaved worker\'s load was timed out'));
        }
        if (c.type === 'load' && c.gen < removals && c.w.state === 'resolved') {
          assert.ok(c.resolvedBefore, where(step, 'a load asked for before a removal became ready after it'));
        }
      }
      if (loadsSinceRemoval === 0) {
        assert.equal(workers.length, workersAtRemoval, where(step, 'a removal restarted a queued load'));
      }
      // Once a removal and everything asked before it have finished, with
      // nothing since that could mark a download ready, no flag may remain.
      if (lastRemoval && lastRemoval.clean && lastRemoval.w.state !== 'pending' &&
          lastRemoval.before.every((c) => c.w.state !== 'pending')) {
        const flags = [...stores.settings.keys()].filter((k) => String(k).startsWith('engineReadyFor'));
        assert.deepEqual(flags, [], where(step, 'a ready flag survived a removal'));
      }
    };

    // One user action. No waiting in here, so several can land back to back.
    const act = (r) => {
      if (r < 0.2) {
        calls.push({ type: 'load', gen: removals, w: watch(engine.load(pick(MODELS))) });
        loadsSinceRemoval++;
        flagMayExist = true;
        if (lastRemoval) lastRemoval.clean = false;
      } else if (r < 0.32) {
        calls.push({ type: 'transcribe', gen: removals, w: watch(engine.transcribe(new Float32Array(160), pick(MODELS))) });
      } else if (r < 0.38) {
        for (const c of calls) c.resolvedBefore = c.resolvedBefore || c.w.state === 'resolved';
        removals++;
        const before = calls.slice();
        calls.push({ type: 'remove', gen: removals, w: watch(engine.dropDownload()) });
        lastRemoval = { w: calls[calls.length - 1].w, before, clean: true };
        workersAtRemoval = workers.length;
        loadsSinceRemoval = 0;
        flagMayExist = false;
      } else if (r < 0.46) {
        if (rand() < 0.5) {
          // An installation from before per-model flags: the check migrates it.
          stores.settings.set('engineReadyFor', pick(MODELS));
          flagMayExist = true;
          if (lastRemoval) lastRemoval.clean = false;
        }
        calls.push({ type: 'check', gen: removals, w: watch(engine.isDownloaded(pick(MODELS))) });
      } else if (r < 0.5) {
        stores.settings.set('engineReadyFor', pick(MODELS));    // an old-style flag appears
        flagMayExist = true;
        if (lastRemoval) lastRemoval.clean = false;
      } else {
        const s = live()[0];
        if (s) s.w.fail({ type: 'error', message: 'out of memory' });
      }
    };
    // Something the user does in the same instant as a worker message
    // (remove the download as a load finishes, check while it migrates...).
    const actNow = () => act(pick([0.33, 0.35, 0.4, 0.42, 0.47, 0.1]));

    // Let `ms` of time pass, delivering the simulated worker's messages on time.
    const advance = async (ms, step) => {
      const until = now + ms;
      for (;;) {
        const s = live().sort((a, b) => a.next() - b.next())[0];
        const at = s ? s.next() : Infinity;
        if (at > until) break;
        if (at > now) { t.mock.timers.tick(at - now); now = at; await quiet(); }
        if (s.w.terminated) continue;
        const { msg } = s.events.shift();
        s.w.emit(msg);
        if (step !== 'drain' && rand() < 0.15) actNow();
        await quiet();
        check(step);
      }
      if (until > now) { t.mock.timers.tick(until - now); now = until; await quiet(); }
    };

    for (let step = 0; step < STEPS; step++) {
      const r = rand();
      if (r < 0.53) {
        // Sometimes a burst of actions with no pause between them.
        const n = rand() < 0.3 ? 2 + Math.floor(rand() * 2) : 1;
        act(r);
        // The rest of a burst leans on the actions that touch storage.
        for (let i = 1; i < n; i++) act(pick([0.34, 0.36, 0.4, 0.44, 0.48, 0.1, rand() * 0.53]));
      } else {
        await advance(Math.floor(rand() * 6 * MIN), step);
      }
      await quiet();
      check(step);
    }

    // Drain: let the worker finish everything.
    for (let round = 0; round < 40 && calls.some((c) => c.w.state === 'pending'); round++) {
      await advance(30 * MIN, 'drain');
    }
    for (const c of calls) {
      assert.notEqual(c.w.state, 'pending', where('end', `a ${c.type} never settled`));
      assert.equal(c.w.settles, 1, where('end', 'a request did not settle exactly once'));
    }
    check('end');
    if (!flagMayExist) {
      const flags = [...stores.settings.keys()].filter((k) => String(k).startsWith('engineReadyFor'));
      assert.deepEqual(flags, [], where('end', 'a ready flag survived the last removal'));
    }
  }
});
