// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC
//
// IndexedDB wrapper. Three stores:
//   settings    - small key/value (engine pick, etc.)
//   transcripts - {id: "<YYYY-MM-DD>|<source>", date, source, text, when}
//                 source is "live <hh:mm>" or a note filename
//   modelfiles  - {url, blob, when} - the transcription pack cache. This is
//                 IndexedDB (not the browser's nicer offline caches) because
//                 the companion is served over plain http from the device,
//                 where those caches aren't available. IndexedDB is.

const DB_NAME = 'cf-companion';
const DB_VER = 1;

let dbPromise = null;

function open() {
  if (dbPromise) return dbPromise;
  dbPromise = new Promise((resolve, reject) => {
    const req = indexedDB.open(DB_NAME, DB_VER);
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
    req.onsuccess = () => resolve(req.result);
    req.onerror = () => reject(req.error);
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
    const req = db.transaction('modelfiles').objectStore('modelfiles').get(url);
    req.onsuccess = () => resolve(req.result ? req.result.blob : null);
    req.onerror = () => resolve(null);
  });
}

export async function modelFilePut(url, blob) {
  const db = await open();
  return tx(db, 'modelfiles', 'readwrite', (st) => st.put({ url, blob, when: Date.now() }));
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
