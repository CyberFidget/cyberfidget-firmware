// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC
//
// Telling an incomplete transcription pack apart from a device we can't reach.
//
// The worker and its runtime are served by the device from the memory card;
// the model weights come from the internet. When loading fails, the browser's
// own error rarely says which of those broke, so after a failure we ask the
// device for the specific file and look only at the answer's headers:
//   'ok'          - the device has it, with the right type
//   'missing'     - the DEVICE says it isn't there: a 404, or (older firmware,
//                   which answered a missing file with its HTML page) a 200 of
//                   the wrong type from something confirmed to be the device
//   'unreachable' - no answer, a redirect or page from something that is not
//                   the device (a captive portal, another network), or a busy
//                   device that kept failing after retries
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
// Pauses before the 2nd and 3rd try of an answer that may be temporary
// (no answer at all, a busy device, a server error).
export const PROBE_RETRY_DELAYS_MS = [1000, 3000];

// What the device labels each kind of file (see webContentType in the
// firmware's portal server).
function expectedType(url) {
  if (/\.m?js$/.test(url)) return 'javascript';
  if (/\.wasm$/.test(url)) return 'wasm';
  if (/\/api\//.test(url)) return 'json';
  return '';
}

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// One request. Returns 'ok' | 'missing' | 'wrongtype' | 'elsewhere' | 'retry'.
async function attempt(url, fetchImpl) {
  const ctl = typeof AbortController !== 'undefined' ? new AbortController() : null;
  const timer = ctl ? setTimeout(() => ctl.abort(), PROBE_TIMEOUT_MS) : null;
  let res;
  try {
    res = await fetchImpl(url, {
      cache: 'no-store',
      redirect: 'manual',         // a redirect is itself the answer (see below)
      signal: ctl ? ctl.signal : undefined,
    });
  } catch {
    return 'retry';               // no answer, or too slow
  } finally {
    if (timer) clearTimeout(timer);
  }
  // The headers are the answer; don't pull a multi-megabyte file to get it.
  if (ctl) ctl.abort();
  // The device never redirects a companion file, so a redirect came from
  // something else in the way (a captive portal, the wrong network).
  if (res.type === 'opaqueredirect' || (res.status >= 300 && res.status < 400)) return 'elsewhere';
  if (res.status === 404) return 'missing';
  if (res.status === 408 || res.status === 429 || res.status >= 500) return 'retry';
  if (!res.ok) return 'elsewhere';
  const want = expectedType(url);
  const type = ((res.headers && res.headers.get('content-type')) || '').toLowerCase();
  return want && !type.includes(want) ? 'wrongtype' : 'ok';
}

async function attemptWithRetries(url, fetchImpl, delays) {
  let result = await attempt(url, fetchImpl);
  for (const ms of delays) {
    if (result !== 'retry') break;
    await sleep(ms);
    result = await attempt(url, fetchImpl);
  }
  return result;
}

export async function probeDeviceFile(url, fetchImpl = fetch, delays = PROBE_RETRY_DELAYS_MS) {
  const result = await attemptWithRetries(url, fetchImpl, delays);
  if (result === 'ok' || result === 'missing') return result;
  if (result === 'wrongtype') {
    // Only the device's own answer may blame the card: confirm it is the
    // device by asking for its status, which only the device answers in JSON.
    const status = await attemptWithRetries('/api/status', fetchImpl, delays);
    return status === 'ok' ? 'missing' : 'unreachable';
  }
  return 'unreachable';
}

// Probe files in order; the first one that isn't 'ok' decides.
// Returns { state: 'ok' } or { state: 'missing' | 'unreachable', file }.
export async function probeDeviceFiles(urls, fetchImpl = fetch, delays = PROBE_RETRY_DELAYS_MS) {
  for (const url of urls) {
    const state = await probeDeviceFile(url, fetchImpl, delays);
    if (state !== 'ok') return { state, file: url.replace(/^\/web\//, '').replace(/\?.*$/, '') };
  }
  return { state: 'ok' };
}
