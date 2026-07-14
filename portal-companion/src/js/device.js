// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC
//
// Thin client for the device's own web endpoints (same-origin; the device
// is the server). Everything voice-related stays on this origin.

async function getJSON(path) {
  const r = await fetch(path);
  if (!r.ok) throw new Error(`device said ${r.status} for ${path}`);
  return r.json();
}

export function getRecordings() { return getJSON('/api/recordings'); }
export function getBrowse(path) { return getJSON('/api/browse?path=' + encodeURIComponent(path)); }
export function getWifiStatus() { return getJSON('/api/wifi/status'); }
export function getStatus() { return getJSON('/api/status'); }

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
    await fetch('/api/time?ms=' + epochMs, { method: 'POST' });
  } catch { /* not fatal */ }
}
