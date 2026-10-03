// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC
//
// Speech-to-text engine FACADE. The heavy work (model load + every inference)
// runs in engine.worker.js, OFF the main thread, so transcription never janks
// the live waveform or caption UI. This file is the thin main-thread proxy:
// light IndexedDB/localStorage queries stay here; load()/transcribe() forward
// to the worker and await its reply.
//
// The model weights are the ONLY thing fetched from the internet, once, with
// explicit consent; they're cached in IndexedDB (shared with the worker) so
// offline use survives a cleared browser cache.
//
// Test hook: when a harness defines window.__cfTestEngine, it replaces the
// real engine wholesale so UI flows run without the worker or the download.

import {
  modelFilesClear, modelFilesList, settingDelete, settingGet, settingSet,
  settingsDeletePrefix, tryPersist,
} from './db.js';
import { probeDeviceFiles } from './pack.js';

// Captions run single-thread on the phone's CPU (WebGPU and multi-thread WASM
// both need a secure context, which the device's plain-http origin can't be),
// so SMALLER = faster live captions. Tiers are ordered fastest-first; the
// first entry is the default, chosen for live-caption responsiveness.
export const MODELS = [
  {
    id: 'onnx-community/moonshine-tiny-ONNX',
    label: 'Fastest - English, made for live captions (about 30 MB)',
    sizeMB: 30,
    english: true,
    live: true,
    notes: false,
  },
  {
    id: 'onnx-community/whisper-tiny.en',
    label: 'Quick - English, works for captions and saved notes (about 40 MB)',
    sizeMB: 40,
    english: true,
    live: true,
    notes: true,
  },
  {
    id: 'onnx-community/whisper-base',
    label: 'Balanced - more accurate, handles other languages (about 60 MB)',
    sizeMB: 60,
    english: false,
    live: true,
    notes: true,
  },
  {
    id: 'onnx-community/distil-small.en',
    label: 'Most accurate - English, slower (best for saved notes) (about 90 MB)',
    sizeMB: 90,
    english: true,
    live: false,
    notes: true,
  },
];

const MODEL_SETTINGS = { live: 'engineModelLive', notes: 'engineModelNotes' };
const MODEL_DEFAULTS = {
  live: 'onnx-community/moonshine-tiny-ONNX',
  notes: 'onnx-community/whisper-tiny.en',
};
const LEGACY_MODEL_SETTING = 'engineModel';
const LEGACY_READY_SETTING = 'engineReadyFor';
const READY_PREFIX = 'engineReadyFor:';
let modelMigration = null;

let worker = null;
let workerReadyFor = null;        // modelId the worker has built a pipeline for
let activeDevice = null;          // 'webgpu' | 'wasm' (reported by the worker)
let onProgressCb = null;
let nextReqId = 1;
const pending = new Map();        // transcribe id -> {resolve, reject}
const loadWaiters = new Map();    // load request id -> {resolve, reject}
let loadTimer = null;             // fires when a load goes quiet for too long
let loadChain = Promise.resolve(); // loads run one at a time (see load())

// A load that hears nothing from the worker for too long is treated as
// failed, so no failure mode can leave the Download button waiting forever.
// Every message restarts the clock, and the allowance depends on what the
// worker said last:
//  - nothing yet, or download progress: LOAD_IDLE_MS. Starting the worker and
//    each step of a download report back well within this.
//  - 'building' or a status ('preparing', 'warming'): LOAD_BUSY_MS. Building the
//    session and the warm-up run compute for a long time with no messages at
//    all, and a slow phone must not be cut off mid-build.
export const LOAD_IDLE_MS = 3 * 60 * 1000;
export const LOAD_BUSY_MS = 15 * 60 * 1000;

// These texts reach the user's screen. `kind` tells the Download button not to
// add "check your internet" to failures that have nothing to do with it.
function cardError(file) {
  const err = new Error('The transcription files on the memory card are incomplete (' + file +
    ' is missing or unreadable). Copy them to the card again, then try again.');
  err.kind = 'card';
  return err;
}

function connectionError() {
  const err = new Error('Lost the connection to your Cyber Fidget. Make sure your phone is ' +
    'still connected to it, then try again.');
  err.kind = 'connection';
  return err;
}

function startError() {
  const err = new Error('Transcription could not start. Reload the page and try again.');
  err.kind = 'engine';
  return err;
}

// A worker error message -> the error the caller sees.
function workerError(msg) {
  if (msg.missing) return cardError(msg.missing);
  if (msg.lost) return connectionError();
  return new Error(msg.error);
}

function clearLoadTimer() {
  if (loadTimer) { clearTimeout(loadTimer); loadTimer = null; }
}

function armLoadTimer(ms) {
  clearLoadTimer();
  if (!loadWaiters.size) return;
  loadTimer = setTimeout(() => {
    loadTimer = null;
    failWorker(new Error('transcription setup stopped responding'));
  }, ms);
}

function settleLoad(id, fn) {
  const w = loadWaiters.get(id);
  if (!w) return;
  loadWaiters.delete(id);
  if (!loadWaiters.size) clearLoadTimer();
  fn(w);
}

function settleAllLoads(fn) {
  [...loadWaiters.keys()].forEach((id) => settleLoad(id, fn));
}

// The worker is unusable (or being thrown away): drop it at once, so the next
// load() starts a fresh one instead of posting to a dead worker, and fail
// everything that was waiting on it. `why` may be a promise of the error, for
// failures that need a moment to diagnose; the waiters are taken now either way.
function failWorker(why) {
  clearLoadTimer();
  const loads = [...loadWaiters.values()]; loadWaiters.clear();
  const reqs = [...pending.values()]; pending.clear();
  if (worker) { worker.terminate(); worker = null; }
  workerReadyFor = null;
  activeDevice = null;
  return Promise.resolve(why).then((err) => {
    loads.forEach((w) => w.reject(err));
    reqs.forEach((p) => p.reject(err));
  });
}

// The worker script itself failed to load. Ask the device about it: a missing
// or wrongly-typed file is a bad card copy; no answer is a lost connection.
async function workerLoadError() {
  const found = await probeDeviceFiles(['/web/engine.worker.js']);
  if (found.state === 'missing') return cardError(found.file);
  if (found.state === 'offline') return connectionError();
  return startError();
}

function ensureWorker() {
  if (worker) return worker;
  // Separate on-demand file (NOT inlined in the shell) so page load stays a
  // single request; the worker is fetched only when captions/transcription
  // start. Module worker so it can `import()` the device-served library.
  const w = new Worker('/web/engine.worker.js', { type: 'module' });
  worker = w;
  w.onmessage = (e) => {
    if (worker !== w) return;     // a dropped worker's late message
    const msg = e.data;
    if (msg.type === 'progress') {
      armLoadTimer(LOAD_IDLE_MS);
      if (onProgressCb) onProgressCb({ pct: msg.pct, label: msg.label });
    } else if (msg.type === 'building') {
      // The library is up; a long silent build may follow. Only moves the
      // timer - the UI has nothing to show for it. (Its own message type, not
      // a 'status' phase, so an older page that forwards every status to the
      // progress display just ignores it.)
      armLoadTimer(LOAD_BUSY_MS);
    } else if (msg.type === 'status') {
      armLoadTimer(LOAD_BUSY_MS);
      // 'preparing' (compiling the session) / 'warming' (warmup inference) —
      // the post-download phases that have no % so the UI doesn't look frozen.
      if (onProgressCb) onProgressCb({ phase: msg.phase });
    } else if (msg.type === 'loaded') {
      activeDevice = msg.device;
      if (msg.id != null) settleLoad(msg.id, (x) => x.resolve());
      else settleAllLoads((x) => x.resolve());
    } else if (msg.type === 'result') {
      const p = pending.get(msg.id);
      if (p) { pending.delete(msg.id); p.resolve(msg.text); }
    } else if (msg.type === 'error') {
      const err = workerError(msg);
      if (msg.id != null && pending.has(msg.id)) {
        const p = pending.get(msg.id); pending.delete(msg.id); p.reject(err);
      } else if (msg.id != null && loadWaiters.has(msg.id)) {
        settleLoad(msg.id, (x) => x.reject(err));
      } else if (msg.id == null) {
        settleAllLoads((x) => x.reject(err));
      }
    }
  };
  w.onerror = (e) => {
    if (worker !== w) return;
    // A script that fails to load (missing, wrong type, or the device stopped
    // answering) fires a bare error event with no message; a crash inside a
    // running worker carries one.
    failWorker(e && e.message
      ? new Error('transcription stopped unexpectedly: ' + e.message)
      : workerLoadError());
  };
  return w;
}

export function listModels() { return MODELS; }

function validFor(slot, id) {
  return MODELS.some((m) => m.id === id && m[slot]);
}

async function migrateModelSettings() {
  if (!modelMigration) modelMigration = (async () => {
    const legacy = await settingGet(LEGACY_MODEL_SETTING, null);
    if (legacy === null) return;
    const live = await settingGet(MODEL_SETTINGS.live, null);
    const notes = await settingGet(MODEL_SETTINGS.notes, null);
    if (live === null) await settingSet(MODEL_SETTINGS.live, MODEL_DEFAULTS.live);
    if (notes === null) {
      await settingSet(MODEL_SETTINGS.notes,
        validFor('notes', legacy) ? legacy : MODEL_DEFAULTS.notes);
    }
    await settingDelete(LEGACY_MODEL_SETTING);
  })();
  return modelMigration;
}

export async function pickedModel(slot) {
  if (!(slot in MODEL_SETTINGS)) throw new Error('unknown transcription slot');
  await migrateModelSettings();
  const id = await settingGet(MODEL_SETTINGS[slot], MODEL_DEFAULTS[slot]);
  return validFor(slot, id) ? id : MODEL_DEFAULTS[slot];
}

export async function pickModel(slot, id) {
  if (slot in MODEL_SETTINGS && validFor(slot, id)) {
    await settingSet(MODEL_SETTINGS[slot], id);
  }
}

// "Downloaded" = a full pipeline build previously completed for this model,
// so every file it needs is in IndexedDB.
export async function isDownloaded(id) {
  if (window.__cfTestEngine) {
    const answer = window.__cfTestEngine.downloaded;
    return typeof answer === 'object' ? !!answer[id] : !!answer;
  }
  if (await settingGet(READY_PREFIX + id, false)) return true;
  const legacy = await settingGet(LEGACY_READY_SETTING, '');
  if (!legacy) return false;
  await settingSet(READY_PREFIX + legacy, true);
  await settingDelete(LEGACY_READY_SETTING);
  return legacy === id;
}

export async function downloadedBytes() {
  const files = await modelFilesList();
  return files.reduce((a, f) => a + f.size, 0);
}

export async function dropDownload() {
  // Drop the worker first so the next load rebuilds cleanly, and so nothing
  // still waiting on it (a load, a transcription) is left hanging.
  failWorker(new Error('the transcription download was removed'));
  await modelFilesClear();
  await settingsDeletePrefix(READY_PREFIX);
  await settingDelete(LEGACY_READY_SETTING);
}

export function gpuAvailable() {
  return typeof navigator !== 'undefined' && 'gpu' in navigator;
}

// Which backend the worker actually built on ('webgpu' | 'wasm' | null).
export function backend() { return activeDevice; }

// Build (or reuse) the recognition pipeline in the worker. onProgress({pct,label})
// fires during the model download. Resolves when the worker is ready.
//
// Loads run one at a time: the worker holds ONE pipeline, so captions loading
// one model while notes load another would otherwise race, and the first to
// finish would mark the other as downloaded and ready.
export async function load(modelId, onProgress) {
  if (window.__cfTestEngine) {
    if (window.__cfTestEngine.load) await window.__cfTestEngine.load(modelId);
    if (onProgress) onProgress({ pct: 100, label: 'test engine' });
    return;
  }
  if (!MODELS.some((m) => m.id === modelId)) throw new Error('unknown transcription model');
  const run = loadChain.then(() => loadNow(modelId, onProgress));
  loadChain = run.catch(() => {});
  return run;
}

async function loadNow(modelId, onProgress) {
  if (workerReadyFor === modelId && worker) return;

  onProgressCb = onProgress || null;
  const model = MODELS.find((m) => m.id === modelId);
  const id = nextReqId++;
  try {
    ensureWorker().postMessage({
      type: 'load',
      id,
      modelId,
      english: !!(model && model.english),
      useGpu: gpuAvailable(),
    });
    await new Promise((resolve, reject) => {
      loadWaiters.set(id, { resolve, reject });
      armLoadTimer(LOAD_IDLE_MS);
    });
  } catch (err) {
    // The worker started replacing its pipeline and didn't finish: it is no
    // longer ready for the previous model either.
    workerReadyFor = null;
    throw err;
  } finally {
    onProgressCb = null;
  }
  workerReadyFor = modelId;
  await settingSet(READY_PREFIX + modelId, true);
  tryPersist();
}

export function loaded() {
  if (window.__cfTestEngine) return true;
  return workerReadyFor !== null;
}

// Transcribe mono Float32 PCM at 16,000 samples/second. Returns plain text.
// Runs in the worker; the audio buffer is transferred (zero-copy).
export async function transcribe(float32Audio, modelId) {
  if (window.__cfTestEngine) return window.__cfTestEngine.transcribe(float32Audio, modelId);
  if (!worker) throw new Error('engine not loaded');
  const model = MODELS.find((m) => m.id === modelId);
  const id = nextReqId++;
  const longForm = float32Audio.length > 16000 * 30;
  return new Promise((resolve, reject) => {
    pending.set(id, { resolve, reject });
    worker.postMessage({
      type: 'transcribe',
      id,
      audio: float32Audio,
      modelId,
      english: !!(model && model.english),
      useGpu: gpuAvailable(),
      longForm,
    }, [float32Audio.buffer]);
  });
}
