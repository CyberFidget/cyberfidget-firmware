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

// The worker file, versioned by its content at build time so a new shell never
// runs a stale copy out of the browser's long-lived cache (the device serves
// /web/ files as immutable and ignores the query). Unbundled (tests), no query.
/* global __CF_WORKER_VERSION__ */
const WORKER_URL = '/web/engine.worker.js' +
  (typeof __CF_WORKER_VERSION__ !== 'undefined' ? '?v=' + __CF_WORKER_VERSION__ : '');

// A load that hears nothing from the worker for too long is treated as
// failed, so no failure mode can leave the Download button waiting forever.
// Every message restarts the clock; how long it allows depends on the phase:
//  - 'loading' (worker starting, library loading, a file downloading):
//    LOAD_IDLE_MS. Each of those reports back well within it.
//  - 'building' (library up, a file finished, preparing, warming up):
//    LOAD_BUSY_MS. Building the session and the warm-up compute for a long time
//    with no messages at all, and a slow phone must not be cut off mid-build.
// A worker that hasn't said hello (just started, or an older copy that never
// will) gets LOAD_BUSY_MS throughout: it can't announce its silent phases.
export const LOAD_IDLE_MS = 3 * 60 * 1000;
export const LOAD_BUSY_MS = 15 * 60 * 1000;

// ---- The engine's ONE state machine ----
// phase: 'idle'     no worker (nothing loaded yet, or the download was removed)
//        'queued'   a load waits behind transcriptions the worker is still
//                   running (they can take minutes)             (no deadline)
//        'loading'  a load is waiting on the worker            (short allowance)
//        'building' a long silent stretch may follow            (long allowance)
//        'ready'    the worker has a pipeline for S.readyFor
//        'failed'   the last load failed (the worker may still be usable)
// gen: bumped by removing the download; a load queued or running under an
// older gen can never mark anything ready.
// Only the functions in this section change S.
const S = {
  phase: 'idle',
  gen: 0,
  worker: null,
  protocol: 0,                    // from the worker's hello; 0 = not (yet) heard
  readyFor: null,
  device: null,                   // 'webgpu' | 'wasm' (reported by the worker)
  load: null,                     // the one load in flight (see loadNow)
  pending: new Map(),             // transcribe id -> {resolve, reject}
  timer: null,
};
let nextReqId = 1;
let loadChain = Promise.resolve();   // loads run one at a time

// Wrap a promise's settle functions so a second attempt is noticed: the
// promise would silently ignore it, but it means two paths both thought they
// owned the request - a lifecycle bug. Tests listen on __cfOnDoubleSettle.
function settleOnce(resolve, reject, what) {
  let done = false;
  const guard = (fn) => (value) => {
    if (done) {
      if (typeof window !== 'undefined' && window.__cfOnDoubleSettle) window.__cfOnDoubleSettle(what);
      return;
    }
    done = true;
    fn(value);
  };
  return { resolve: guard(resolve), reject: guard(reject) };
}
let storageChain = Promise.resolve(); // ready-flag writes vs. removal, in order

// These texts reach the user's screen. `kind` lets the Download button give
// the right advice: only 'download' (and an older worker's unclassified
// errors) get "check your internet".
function kindError(kind, message) {
  const err = new Error(message);
  err.kind = kind;
  return err;
}
const cardError = (file) => kindError('card',
  'The transcription files on the memory card are incomplete (' + file +
  ' is missing). Copy them to the card again, then try again.');
const connectionError = () => kindError('connection',
  "Couldn't reach your Cyber Fidget. Make sure your phone is still connected to it, then try again.");
const downloadError = () => kindError('download', 'the transcription pack could not be downloaded');
const downloadStalledError = () => kindError('download', 'the download stopped');
const startError = () => kindError('engine',
  'Transcription could not start in this browser. Reload the page and try again.');
const stalledError = () => kindError('engine',
  'Transcription setup stopped responding. Reload the page and try again.');
const crashError = () => kindError('engine',
  'Transcription stopped unexpectedly. Reload the page and try again.');
const transcribeError = () => kindError('engine', 'Transcription failed on this audio. Try again.');
const removedError = () => kindError('removed', 'The transcription download was removed.');

// A worker 'error' reply -> the error the caller sees.
function replyError(msg, forLoad) {
  if (msg.kind === 'card') return cardError(msg.file || 'a file');
  if (msg.kind === 'connection') return connectionError();
  if (msg.kind === 'download') return downloadError();
  if (msg.kind === 'engine' || S.protocol >= 2) return forLoad ? startError() : transcribeError();
  return new Error(msg.error);    // an older worker: keep its own words
}

function allowance() {
  if (S.phase === 'building' || S.protocol < 2) return LOAD_BUSY_MS;
  return LOAD_IDLE_MS;
}

function clearTimer() {
  if (S.timer) { clearTimeout(S.timer); S.timer = null; }
}

// (Re)start the quiet-worker clock for the load in flight. The callback
// checks it still belongs to that load, so a timer can never act on a load
// that has already settled. A load still queued behind transcriptions has no
// deadline yet: the worker is busy, not quiet.
function arm() {
  clearTimer();
  const L = S.load;
  if (!L || S.phase === 'queued') return;
  S.timer = setTimeout(() => {
    S.timer = null;
    if (S.load !== L) return;
    teardown(S.phase === 'loading' && L.sawProgress ? downloadStalledError() : stalledError());
  }, allowance());
}

function setPhase(phase) {
  if (!S.load) return;
  S.phase = phase;
  arm();
}

// Settle the load in flight, exactly once.
function finishLoad(err, device) {
  const L = S.load;
  if (!L) return;
  S.load = null;
  clearTimer();
  if (err) {
    // The worker let go of its previous pipeline to build this one.
    S.phase = 'failed';
    S.readyFor = null;
    L.reject(err);
  } else {
    S.phase = 'ready';
    S.readyFor = L.modelId;
    S.device = device || null;
    L.resolve();
  }
}

// Throw the worker away and fail everything waiting on it, exactly once.
// `why` may be a promise of the error (a failure that needs a moment to
// diagnose); the waiters are detached now either way.
function teardown(why, phase = 'failed') {
  clearTimer();
  const L = S.load; S.load = null;
  const reqs = [...S.pending.values()]; S.pending.clear();
  if (S.worker) { S.worker.terminate(); S.worker = null; }
  S.protocol = 0;
  S.readyFor = null;
  S.device = null;
  S.phase = phase;
  return Promise.resolve(why).then((err) => {
    if (L) L.reject(err);
    reqs.forEach((p) => p.reject(err));
  });
}

function withStorage(fn) {
  const run = storageChain.then(fn);
  storageChain = run.catch(() => {});
  return run;
}

// The worker script itself failed to load. Ask the device about it: a file
// the device says is missing is a bad card copy; no proper answer from the
// device means we couldn't reach it.
async function workerLoadError() {
  const found = await probeDeviceFiles(['/web/engine.worker.js']);
  if (found.state === 'missing') return cardError(found.file);
  if (found.state === 'unreachable') return connectionError();
  return startError();
}

// A transcription the queued load was waiting behind has finished. Once none
// are left, the worker should reach the load promptly: start its clock.
function transcribeDone(id) {
  const L = S.load;
  if (!L || !L.ahead.delete(id)) return;
  if (S.phase === 'queued' && !L.ahead.size) setPhase('loading');
}

function onWorkerMessage(msg) {
  const L = S.load;
  const mine = L && (msg.id === L.id || (msg.id == null && S.protocol < 2));
  // Phase messages carry no id. A current worker only sends them for the job
  // it is running, so before this load has started they belong to a
  // transcription ahead of it (which may build a pipeline of its own).
  const phaseForLoad = L && (L.started || S.protocol < 2) ? L : null;
  switch (msg.type) {
    case 'hello':
      S.protocol = msg.protocol || 0;
      if (L && !L.started && L.ahead.size && S.protocol >= 2) setPhase('queued');
      else if (L) arm();
      break;
    case 'started':
      // The worker has reached this load in its queue: start the clock now.
      if (L && msg.id === L.id) {
        L.started = true;
        setPhase(S.phase === 'queued' ? 'loading' : S.phase);
      }
      break;
    case 'building':
      if (phaseForLoad) setPhase('building');
      break;
    case 'progress':
      if (!phaseForLoad) break;
      L.sawProgress = true;
      // A finished file may be the last one, and the silent build follows it.
      setPhase(msg.fileDone ? 'building' : 'loading');
      if (L.onProgress) L.onProgress({ pct: msg.pct, label: msg.label });
      break;
    case 'status':
      // 'preparing' (compiling the session) / 'warming' (warmup inference) —
      // the post-download phases that have no % so the UI doesn't look frozen.
      if (!phaseForLoad) break;
      setPhase('building');
      if (L.onProgress) L.onProgress({ phase: msg.phase });
      break;
    case 'loaded':
      if (mine) finishLoad(null, msg.device);
      break;
    case 'result': {
      const p = S.pending.get(msg.id);
      if (p) { S.pending.delete(msg.id); p.resolve(msg.text); transcribeDone(msg.id); }
      break;
    }
    case 'error': {
      const p = msg.id != null && S.pending.get(msg.id);
      if (p) { S.pending.delete(msg.id); p.reject(replyError(msg, false)); transcribeDone(msg.id); }
      else if (mine) finishLoad(replyError(msg, true));
      break;
    }
    default:
      break;
  }
}

function ensureWorker() {
  if (S.worker) return S.worker;
  // Separate on-demand file (NOT inlined in the shell) so page load stays a
  // single request; the worker is fetched only when captions/transcription
  // start. Module worker so it can `import()` the device-served library.
  const w = new Worker(WORKER_URL, { type: 'module' });
  S.worker = w;
  S.protocol = 0;
  w.onmessage = (e) => {
    if (S.worker === w) onWorkerMessage(e.data);   // else: a dropped worker
  };
  w.onerror = (e) => {
    if (S.worker !== w) return;
    // A script that fails to load (missing, wrong type, or the device didn't
    // answer) fires a bare error event with no message; a crash inside a
    // running worker carries one.
    teardown(e && e.message ? crashError() : workerLoadError());
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
  // In turn with removal (see dropDownload): otherwise the legacy flag could
  // be read before a removal and re-written as a new flag after it.
  const gen = S.gen;
  return withStorage(async () => {
    if (gen !== S.gen) return false;       // removed since it was asked
    if (await settingGet(READY_PREFIX + id, false)) return true;
    const legacy = await settingGet(LEGACY_READY_SETTING, '');
    if (!legacy) return false;
    await settingSet(READY_PREFIX + legacy, true);
    await settingDelete(LEGACY_READY_SETTING);
    return legacy === id;
  });
}

export async function downloadedBytes() {
  const files = await modelFilesList();
  return files.reduce((a, f) => a + f.size, 0);
}

export async function dropDownload() {
  // Invalidate every load queued or running so far, drop the worker (so the
  // next load rebuilds cleanly) and fail anything still waiting on it. Then
  // clear storage - in turn with any ready flag a finished load is writing.
  S.gen++;
  teardown(removedError(), 'idle');
  await withStorage(async () => {
    await modelFilesClear();
    await settingsDeletePrefix(READY_PREFIX);
    await settingDelete(LEGACY_READY_SETTING);
  });
}

export function gpuAvailable() {
  return typeof navigator !== 'undefined' && 'gpu' in navigator;
}

// Which backend the worker actually built on ('webgpu' | 'wasm' | null).
export function backend() { return S.device; }

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
  const gen = S.gen;
  const run = loadChain.then(() => loadNow(gen, modelId, onProgress));
  loadChain = run.catch(() => {});
  return run;
}

async function loadNow(gen, modelId, onProgress) {
  // Queued before the download was removed: don't quietly download it again.
  if (gen !== S.gen) throw removedError();
  if (S.readyFor === modelId && S.worker) return;

  const model = MODELS.find((m) => m.id === modelId);
  const id = nextReqId++;
  const w = ensureWorker();
  const done = new Promise((resolve, reject) => {
    S.load = {
      id, gen, modelId,
      onProgress: onProgress || null,
      sawProgress: false,
      started: false,
      // Transcriptions the worker will run before reaching this load.
      ahead: new Set(S.pending.keys()),
      ...settleOnce(resolve, reject, 'load'),
    };
  });
  S.phase = S.protocol >= 2 && S.load.ahead.size ? 'queued' : 'loading';
  arm();
  w.postMessage({
    type: 'load',
    id,
    modelId,
    english: !!(model && model.english),
    useGpu: gpuAvailable(),
  });
  await done;
  // Record it as downloaded - unless the download was removed meanwhile.
  await withStorage(async () => {
    if (gen !== S.gen) throw removedError();
    await settingSet(READY_PREFIX + modelId, true);
  });
  tryPersist();
}

export function loaded() {
  if (window.__cfTestEngine) return true;
  return S.readyFor !== null;
}

// Transcribe mono Float32 PCM at 16,000 samples/second. Returns plain text.
// Runs in the worker; the audio buffer is transferred (zero-copy).
export async function transcribe(float32Audio, modelId) {
  if (window.__cfTestEngine) return window.__cfTestEngine.transcribe(float32Audio, modelId);
  if (!S.worker) throw new Error('engine not loaded');
  const model = MODELS.find((m) => m.id === modelId);
  const id = nextReqId++;
  const longForm = float32Audio.length > 16000 * 30;
  return new Promise((resolve, reject) => {
    S.pending.set(id, settleOnce(resolve, reject, 'transcribe'));
    S.worker.postMessage({
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
