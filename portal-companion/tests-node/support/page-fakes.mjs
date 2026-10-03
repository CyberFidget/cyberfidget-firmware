// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC
//
// Browser stand-ins for testing src/js/engine.js under Node: a scriptable
// Worker, a minimal IndexedDB, and a device that answers probes as told.
// Importing this installs them as globals.

export const workers = [];

export class FakeWorker {
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
  hello() { this.emit({ type: 'hello', protocol: 2 }); }
  // The worker reached the most recent load in its queue.
  started() { this.emit({ type: 'started', id: this.lastLoad().id }); }
  loads() { return this.posted.filter((m) => m.type === 'load'); }
  lastLoad() { const l = this.loads(); return l[l.length - 1]; }
  // Answer the most recent load request (the worker echoes its id).
  loaded() { this.emit({ type: 'loaded', id: this.lastLoad().id, device: 'wasm' }); }
}

// Every IndexedDB write the page makes, as [op, store, key].
export const dbWrites = [];

// The stored values, per object store.
export const stores = { settings: new Map(), transcripts: new Map(), modelfiles: new Map() };

// Transactions apply in the order they were created (as IndexedDB runs
// overlapping read-write transactions), each after `storage.hops()` macrotask
// hops, so separate async steps of different callers can interleave between
// transactions. hops() = 0 keeps everything on microtasks (works with mocked
// timers and flush()).
export const storage = { hops: () => 0 };
const hop = () => new Promise((r) => setImmediate(r));
let txChain = Promise.resolve();
// Resolves once every transaction created so far has been applied.
export const storageIdle = () => txChain;

// Microtasks only (not timers), so it keeps working while timers are mocked.
const later = (fn) => Promise.resolve().then(fn);
const fakeDb = {
  version: 1,
  objectStoreNames: { contains: () => true },
  close() {},
  transaction(store) {
    const m = stores[store];
    const ops = [];
    const t = {
      objectStore: () => ({
        put: (value, key) => { ops.push(() => { m.set(key, value); dbWrites.push(['put', store, key]); }); return {}; },
        delete: (key) => { ops.push(() => { m.delete(key); dbWrites.push(['delete', store, key]); }); return {}; },
        clear: () => { ops.push(() => { m.clear(); dbWrites.push(['clear', store]); }); return {}; },
        get: (key) => {
          const r = {};
          ops.push(() => { r.result = m.get(key); if (r.onsuccess) r.onsuccess(); });
          return r;
        },
        openCursor: () => {
          const r = {};
          ops.push(() => {
            const keys = [...m.keys()];
            let i = 0;
            const next = () => {
              if (i < keys.length) {
                const key = keys[i++];
                r.result = {
                  key,
                  value: m.get(key),
                  delete: () => { m.delete(key); dbWrites.push(['delete', store, key]); },
                  continue: () => next(),
                };
              } else {
                r.result = null;
              }
              if (r.onsuccess) r.onsuccess();
            };
            next();
          });
          return r;
        },
      }),
    };
    txChain = txChain.then(async () => {
      const n = storage.hops();
      for (let h = 0; h < n; h++) await hop();
      ops.forEach((op) => op());
      if (t.oncomplete) t.oncomplete();
    });
    return t;
  },
};

// How the device answers a probe: 'missing' (404), 'offline' (no answer),
// 'ok' (the file, right type). Its status answer has the device's own fields
// unless `status` is 'portal' (something else answering in JSON).
export const device = { answer: 'missing', status: 'device', probed: [] };
export const DEVICE_STATUS = { files: 3, totalBytes: 100, usedBytes: 50, clients: 1 };

globalThis.indexedDB = {
  open() {
    const req = {};
    later(() => { req.result = fakeDb; req.onsuccess && req.onsuccess(); });
    return req;
  },
};
globalThis.window = {};
// Every second attempt to settle the same request (the engine reports them).
export const doubleSettles = [];
globalThis.window.__cfOnDoubleSettle = (what) => doubleSettles.push(what);
globalThis.Worker = FakeWorker;
globalThis.fetch = async (url) => {
  device.probed.push(String(url));
  if (device.answer === 'offline') throw new TypeError('Failed to fetch');
  if (String(url) === '/api/status') {
    const body = device.status === 'device' ? DEVICE_STATUS : { ok: true };
    return new Response(JSON.stringify(body), { status: 200, headers: { 'content-type': 'application/json' } });
  }
  if (device.answer === 'missing') return new Response('not found', { status: 404, headers: { 'content-type': 'text/plain' } });
  return new Response('', { status: 200, headers: { 'content-type': 'text/javascript' } });
};

// Track a promise's state without awaiting it.
export function watch(p) {
  const s = { state: 'pending', value: undefined, settles: 0 };
  p.then((v) => { s.state = 'resolved'; s.value = v; s.settles++; },
    (e) => { s.state = 'rejected'; s.value = e; s.settles++; });
  return s;
}
export const flush = async () => { for (let i = 0; i < 60; i++) await Promise.resolve(); };
export const settle = () => new Promise((r) => setImmediate(r));
