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

function open() {
  if (dbPromise) return dbPromise;
  dbPromise = new Promise((resolve, reject) => {
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
        // A database created elsewhere without our stores: bump the version
        // so onupgradeneeded above creates them.
        if (['settings', 'transcripts', 'modelfiles'].some((store) => !db.objectStoreNames.contains(store))) {
          const nextVersion = db.version + 1;
          db.close();
          openRequest(nextVersion);
          return;
        }
        db.onversionchange = () => db.close();
        resolve(db);
      };
      req.onerror = () => reject(req.error);
      // Only the repair reopen changes the version. Another open connection
      // that won't close (e.g. an older companion tab) would stall it
      // forever, so fail loudly instead of hanging.
      req.onblocked = () => reject(new Error('cf-companion storage repair is blocked by another open companion tab; close other tabs and reload'));
    };
    openRequest();
  });
  return dbPromise;
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
