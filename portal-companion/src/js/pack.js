// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC
//
// Telling an incomplete transcription pack apart from a lost connection.
//
// The worker and its runtime are served by the device from the memory card;
// the model weights come from the internet. When loading fails, the browser's
// own error rarely says which of those broke, so after a failure we ask the
// device for the specific file and look only at the answer's headers:
//   'ok'      - the device has it, with the right type
//   'missing' - not found, or served as the wrong type: the card copy is bad
//   'offline' - the device didn't answer: the connection to it was lost
// Used by both the page (engine.js) and the worker (engine.worker.js).

// The runtime files the transcription library fetches from the card while it
// builds a session. Probed one at a time (the device's card server wedges
// under concurrent reads), and only after something has already failed.
export const RUNTIME_FILES = [
  '/web/vendor/ort/ort-wasm-simd-threaded.mjs',
  '/web/vendor/ort/ort-wasm-simd-threaded.wasm',
  '/web/vendor/ort/ort-wasm-simd-threaded.jsep.mjs',
  '/web/vendor/ort/ort-wasm-simd-threaded.jsep.wasm',
];

export const PROBE_TIMEOUT_MS = 15000;

// What the device should label each kind of file (see webContentType in the
// firmware's portal server).
function expectedType(url) {
  if (/\.m?js$/.test(url)) return 'javascript';
  if (/\.wasm$/.test(url)) return 'wasm';
  return '';
}

export async function probeDeviceFile(url, fetchImpl = fetch) {
  const ctl = typeof AbortController !== 'undefined' ? new AbortController() : null;
  const timer = ctl ? setTimeout(() => ctl.abort(), PROBE_TIMEOUT_MS) : null;
  let res;
  try {
    res = await fetchImpl(url, { cache: 'no-store', signal: ctl ? ctl.signal : undefined });
  } catch {
    return 'offline';
  } finally {
    if (timer) clearTimeout(timer);
  }
  // The headers are the answer; don't pull a multi-megabyte file to get it.
  if (ctl) ctl.abort();
  if (!res.ok) return 'missing';
  const want = expectedType(url);
  const type = ((res.headers && res.headers.get('content-type')) || '').toLowerCase();
  return want && !type.includes(want) ? 'missing' : 'ok';
}

// Probe files in order; the first one that isn't 'ok' decides.
// Returns { state: 'ok' } or { state: 'missing' | 'offline', file }.
export async function probeDeviceFiles(urls, fetchImpl = fetch) {
  for (const url of urls) {
    const state = await probeDeviceFile(url, fetchImpl);
    if (state !== 'ok') return { state, file: url.replace(/^\/web\//, '') };
  }
  return { state: 'ok' };
}
