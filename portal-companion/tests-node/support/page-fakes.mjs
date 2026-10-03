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
  loads() { return this.posted.filter((m) => m.type === 'load'); }
  lastLoad() { const l = this.loads(); return l[l.length - 1]; }
  // Answer the most recent load request (the worker echoes its id).
  loaded() { this.emit({ type: 'loaded', id: this.lastLoad().id, device: 'wasm' }); }
}

// Every IndexedDB write the page makes, as [store, key] (deletes and clears too).
export const dbWrites = [];

// Microtasks only (not timers), so it keeps working while timers are mocked.
const later = (fn) => Promise.resolve().then(fn);
const fakeDb = {
  version: 1,
  objectStoreNames: { contains: () => true },
  close() {},
  transaction(store) {
    const t = {
      objectStore: () => ({
        put: (value, key) => { dbWrites.push(['put', store, key]); return { result: undefined }; },
        delete: (key) => { dbWrites.push(['delete', store, key]); return { result: undefined }; },
        clear: () => { dbWrites.push(['clear', store]); return { result: undefined }; },
        openCursor: () => { dbWrites.push(['cursor', store]); return {}; },
        get: () => { const r = {}; later(() => r.onsuccess && r.onsuccess()); return r; },
      }),
    };
    later(() => t.oncomplete && t.oncomplete());
    return t;
  },
};

// How the device answers a probe: 'missing' (404), 'offline' (no answer),
// 'ok' (the file, right type).
export const device = { answer: 'missing', probed: [] };

globalThis.indexedDB = {
  open() {
    const req = {};
    later(() => { req.result = fakeDb; req.onsuccess && req.onsuccess(); });
    return req;
  },
};
globalThis.window = {};
globalThis.Worker = FakeWorker;
globalThis.fetch = async (url) => {
  device.probed.push(String(url));
  if (device.answer === 'offline') throw new TypeError('Failed to fetch');
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
