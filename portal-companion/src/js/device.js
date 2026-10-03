// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC
//
// Thin client for the device's own web endpoints (same-origin; the device
// is the server). Everything voice-related stays on this origin.

// The device serves one request at a time, so a busy moment can leave a request
// pending with no answer. Quick lookups give up after a while and fail like any
// other unreachable-device error, instead of leaving the page waiting forever.
// Card listings get longer (big folders are slow to read). Transfers, uploads
// and changes to files (delete, rename, new folder) get no limit: a transfer is
// long by nature, and timing out a change the device may still finish would
// report a failure that did not happen.
const QUICK_MS = 5000;
const LISTING_MS = 20000;

// AbortSignal.timeout is missing on some older phone browsers.
function timeoutSignal(ms) {
  if (typeof AbortSignal !== 'undefined' && typeof AbortSignal.timeout === 'function') {
    return AbortSignal.timeout(ms);
  }
  if (typeof AbortController === 'undefined') return undefined;
  const c = new AbortController();
  setTimeout(() => c.abort(), ms);
  return c.signal;
}

function isTimeout(e) {
  return !!e && (e.name === 'TimeoutError' || e.name === 'AbortError');
}

async function getJSON(path, ms) {
  try {
    const r = await fetch(path, { signal: timeoutSignal(ms) });
    if (!r.ok) throw new Error(`device said ${r.status} for ${path}`);
    return await r.json();
  } catch (e) {
    if (isTimeout(e)) throw new Error(`the device did not answer in time for ${path}`);
    throw e;
  }
}

export function getRecordings() { return getJSON('/api/recordings', LISTING_MS); }
export function getBrowse(path) { return getJSON('/api/browse?path=' + encodeURIComponent(path), LISTING_MS); }
export function getWifiStatus() { return getJSON('/api/wifi/status', QUICK_MS); }
export function getStatus() { return getJSON('/api/status', QUICK_MS); }

export async function fetchText(path) {
  const r = await fetch(path);
  if (!r.ok) return null;
  return r.text();
}

export async function fetchBytes(path) {
  const r = await fetch(path);
  if (!r.ok) throw new Error(`could not read ${path} (${r.status})`);
  return r.arrayBuffer();
}

// Delete a file on the card. The Notes view owns this now: browsing and deleting
// recordings used to live in the portal, and moved here when the two lists of
// the same files merged into one destination.
export async function deletePath(path) {
  const r = await fetch('/api/delete?path=' + encodeURIComponent(path), { method: 'POST' });
  if (!r.ok) throw new Error(`the device could not delete it (${r.status})`);
}

// Rename (the device's move, within one folder). It carries a recording's
// transcript sidecar along by itself - see handleMove in WebPortalApp.cpp.
export async function move(from, to) {
  const r = await fetch('/api/move?from=' + encodeURIComponent(from) +
                        '&to=' + encodeURIComponent(to), { method: 'POST' });
  if (!r.ok) throw new Error(`the device could not rename it (${r.status})`);
}

export async function mkdir(path) {
  // Best-effort: the device reports failure for an already-existing folder,
  // which is fine - the follow-up write is the real test.
  try {
    await fetch('/api/mkdir?path=' + encodeURIComponent(path), { method: 'POST' });
  } catch { /* offline against the device surfaces on the next call */ }
}

export async function uploadText(dir, filename, text) {
  const form = new FormData();
  form.append('file', new Blob([text], { type: 'text/plain' }), filename);
  const r = await fetch('/api/upload?dir=' + encodeURIComponent(dir), {
    method: 'POST',
    body: form,
  });
  if (!r.ok) {
    if (r.status === 507) throw new Error('the memory card is full');
    throw new Error(`the device could not save it (${r.status})`);
  }
}

// Set the device clock from this phone (same local-naive epoch the portal
// page sends). Harmless if it fails; recordings just stay undated.
export async function setClock(epochMs) {
  try {
    await fetch('/api/time?ms=' + epochMs, { method: 'POST', signal: timeoutSignal(QUICK_MS) });
  } catch { /* not fatal */ }
}
