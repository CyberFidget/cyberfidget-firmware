// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC
//
// IndexedDB wrapper. Three stores:
//   settings    - small key/value (engine pick, etc.)
//   transcripts - {id: "<YYYY-MM-DD>|<source>", date, source, text, when}
//                 source is "live <hh:mm>" or a note filename
//   modelfiles  - {url, blob, size, when} - the transcription pack cache. This is
//                 IndexedDB (not the browser's nicer offline caches) because
//                 the companion is served over plain http from the device,
//                 where those caches aren't available. IndexedDB is.

const DB_NAME = 'cf-companion';

let dbPromise = null;

// A failed open, or a connection the browser closed (another tab upgrading the
// database), clears dbPromise so the next call opens again instead of reusing
// a rejected promise or a dead connection.
function open() {
  if (dbPromise) return dbPromise;
  const attempt = new Promise((resolve, reject) => {
    let settled = false;
    const fail = (err) => {
      if (settled) return;
      settled = true;
      reject(err);
    };
    // Open at whatever version exists (a self-healed database may be past 1).
    const openRequest = (version) => {
      const req = version === undefined ? indexedDB.open(DB_NAME) : indexedDB.open(DB_NAME, version);
      req.onupgradeneeded = () => {
        const db = req.result;
        if (!db.objectStoreNames.contains('settings')) {
          db.createObjectStore('settings');
        }
        if (!db.objectStoreNames.contains('transcripts')) {
          const st = db.createObjectStore('transcripts', { keyPath: 'id' });
          st.createIndex('byDate', 'date');
        }
        if (!db.objectStoreNames.contains('modelfiles')) {
          db.createObjectStore('modelfiles', { keyPath: 'url' });
        }
      };
      req.onsuccess = () => {
        const db = req.result;
        // This attempt already failed (e.g. it was blocked and the other tab
        // closed later): don't leave its connection open.
        if (settled) {
          db.close();
          return;
        }
        // A database created elsewhere without our stores: bump the version
        // so onupgradeneeded above creates them.
        if (['settings', 'transcripts', 'modelfiles'].some((store) => !db.objectStoreNames.contains(store))) {
          const nextVersion = db.version + 1;
          db.close();
          openRequest(nextVersion);
          return;
        }
        const forget = () => {
          if (dbPromise === attempt) dbPromise = null;
        };
        db.onversionchange = () => {
          db.close();
          forget();
        };
        db.onclose = forget;
        settled = true;
        resolve(db);
      };
      req.onerror = () => fail(req.error);
      // Only the repair reopen changes the version. Another open connection
      // that won't close (e.g. an older companion tab) would stall it
      // forever, so fail loudly instead of hanging. This text reaches the
      // user's screen.
      req.onblocked = () => fail(new Error('Another companion tab is open. Close it and reload this page.'));
    };
    openRequest();
  });
  dbPromise = attempt;
  attempt.catch(() => {
    if (dbPromise === attempt) dbPromise = null;
  });
  return attempt;
}

function tx(db, store, mode, fn) {
  return new Promise((resolve, reject) => {
    const t = db.transaction(store, mode);
    const result = fn(t.objectStore(store));
    t.oncomplete = () => resolve(result && 'result' in result ? result.result : undefined);
    t.onerror = () => reject(t.error);
    t.onabort = () => reject(t.error);
  });
}

export async function settingGet(key, fallback = null) {
  const db = await open();
  return new Promise((resolve) => {
    const req = db.transaction('settings').objectStore('settings').get(key);
    req.onsuccess = () => resolve(req.result === undefined ? fallback : req.result);
    req.onerror = () => resolve(fallback);
  });
}

export async function settingSet(key, value) {
  const db = await open();
  return tx(db, 'settings', 'readwrite', (st) => st.put(value, key));
}

export async function settingDelete(key) {
  const db = await open();
  return tx(db, 'settings', 'readwrite', (st) => st.delete(key));
}

export async function settingsDeletePrefix(prefix) {
  const db = await open();
  return tx(db, 'settings', 'readwrite', (st) => {
    const req = st.openCursor();
    req.onsuccess = () => {
      const cur = req.result;
      if (!cur) return;
      if (String(cur.key).startsWith(prefix)) cur.delete();
      cur.continue();
    };
    return req;
  });
}

export async function transcriptPut(date, source, text) {
  const db = await open();
  const rec = { id: `${date}|${source}`, date, source, text, when: Date.now() };
  return tx(db, 'transcripts', 'readwrite', (st) => st.put(rec));
}

export async function transcriptsForDate(date) {
  const db = await open();
  return new Promise((resolve) => {
    const out = [];
    const idx = db.transaction('transcripts').objectStore('transcripts').index('byDate');
    const req = idx.openCursor(IDBKeyRange.only(date));
    req.onsuccess = () => {
      const cur = req.result;
      if (cur) { out.push(cur.value); cur.continue(); } else { resolve(out); }
    };
    req.onerror = () => resolve(out);
  });
}

export async function transcriptGet(date, source) {
  const db = await open();
  return new Promise((resolve) => {
    const req = db.transaction('transcripts').objectStore('transcripts').get(`${date}|${source}`);
    req.onsuccess = () => resolve(req.result || null);
    req.onerror = () => resolve(null);
  });
}

export async function modelFileGet(url) {
  const db = await open();
  return new Promise((resolve) => {
    const t = db.transaction('modelfiles', 'readwrite');
    const store = t.objectStore('modelfiles');
    const req = store.get(url);
    let blob = null;
    req.onsuccess = () => {
      const rec = req.result;
      if (!rec) return;
      if (rec.size !== undefined && rec.blob.size !== rec.size) {
        store.delete(url);
      } else {
        blob = rec.blob;
      }
    };
    t.oncomplete = () => resolve(blob);
    t.onerror = () => resolve(null);
    t.onabort = () => resolve(null);
  });
}

// contentLength is the response header (or null). It is the size on the wire:
// for a compressed transfer the decoded blob is LARGER than it, so only a
// blob SHORTER than the header (a cut-off download) is refused. The stored
// size is the length actually stored, so a later read can spot a blob that
// comes back a different length.
export async function modelFilePut(url, blob, contentLength = null) {
  if (contentLength !== null && blob.size < contentLength) return;
  const db = await open();
  return tx(db, 'modelfiles', 'readwrite', (st) => st.put({ url, blob, size: blob.size, when: Date.now() }));
}

export async function modelFileMatch(url) {
  const blob = await modelFileGet(url);
  return blob ? new Response(blob, { headers: { 'content-length': String(blob.size) } }) : undefined;
}

export async function modelFileCachePut(url, response) {
  const length = response.headers.get('content-length');
  const blob = await response.blob();
  const expected = length === null ? NaN : Number(length);
  await modelFilePut(url, blob, Number.isFinite(expected) ? expected : null);
}

// The cache object the engine worker hands to transformers.js (env.customCache).
export const modelFileCache = {
  match: async (request) => modelFileMatch(typeof request === 'string' ? request : request.url),
  put: async (request, response) => modelFileCachePut(typeof request === 'string' ? request : request.url, response),
};

// Wraps fetch so a large pack download that ends early FAILS instead of
// finishing quietly. transformers.js sizes its read buffer from
// content-length and passes that zero-padded buffer to cache.put, so by the
// time put sees it a short download already has the full length. The only
// reliable place to catch it is while the body is read: count the bytes that
// really arrive and error the body if it ends well short, so the download
// throws and nothing is cached. The counter is a pass-through stream; nothing
// is buffered. Only other-origin requests (the pack downloads) are wrapped;
// the device's own files pass through untouched.
//
// Only large files (content-length of 1 MiB or more: the model weights, which
// is what broke in the field) are checked, and only a LARGE shortfall counts
// as a cut-off: fewer than 95% of the announced bytes. content-length is the
// size on the wire and the body we read is the decoded file, so a small
// shortfall can come from compression framing; a dropped connection loses a
// large fraction far more often than a sliver, and compressed transfers of
// large binary model files don't shrink below 95% of their compressed size
// when decoded. Smaller files and smaller shortfalls pass through unchanged
// (the library's own behaviour). Anything this misses is still caught by the
// empty-caption warning, which tells the owner to download the pack again.
const LARGE_FILE_BYTES = 1 << 20;
const MIN_ARRIVED_FRACTION = 0.95;

export function lengthCheckedFetch(fetchImpl) {
  return async (input, init) => {
    const res = await fetchImpl(input, init);
    const url = typeof input === 'string' ? input : (input && input.url) || String(input);
    let otherOrigin = true;
    try {
      otherOrigin = new URL(url, self.location.href).origin !== self.location.origin;
    } catch { /* keep wrapping */ }
    const expected = Number(res.headers.get('content-length'));
    if (!otherOrigin || res.status !== 200 || !res.body || !(expected >= LARGE_FILE_BYTES)) return res;
    let received = 0;
    const counted = res.body.pipeThrough(new TransformStream({
      transform(chunk, controller) {
        received += chunk.byteLength;
        controller.enqueue(chunk);
      },
      flush(controller) {
        if (received < expected * MIN_ARRIVED_FRACTION) {
          controller.error(new Error('the connection dropped before the file finished'));
        }
      },
    }));
    return new Response(counted, { status: res.status, statusText: res.statusText, headers: res.headers });
  };
}

export async function modelFilesClear() {
  const db = await open();
  return tx(db, 'modelfiles', 'readwrite', (st) => st.clear());
}

export async function modelFilesList() {
  const db = await open();
  return new Promise((resolve) => {
    const out = [];
    const req = db.transaction('modelfiles').objectStore('modelfiles').openCursor();
    req.onsuccess = () => {
      const cur = req.result;
      if (cur) { out.push({ url: cur.value.url, size: cur.value.blob.size }); cur.continue(); }
      else resolve(out);
    };
    req.onerror = () => resolve(out);
  });
}

// Ask the browser to keep our storage around (best-effort; not available on
// plain-http device pages, where re-download is the recovery path anyway).
export async function tryPersist() {
  try {
    if (navigator.storage && navigator.storage.persist) {
      return await navigator.storage.persist();
    }
  } catch { /* ignore */ }
  return false;
}
