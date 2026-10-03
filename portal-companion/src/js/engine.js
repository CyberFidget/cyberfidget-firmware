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
let loadWaiters = [];             // resolvers waiting on 'loaded'
let loadTimer = null;             // fires when a load goes quiet for too long

// A load that hears nothing from the worker for this long is treated as
// failed, so no failure mode can leave the Download button waiting forever.
// Every progress/status message restarts the clock; the longest legitimate
// silence is building the session after the download (no progress events).
export const LOAD_IDLE_MS = 3 * 60 * 1000;

// Shown when the worker script itself can't be loaded from the device: the
// device serves it from the memory card, so the copy there is incomplete.
// This text reaches the user's screen.
const CARD_INCOMPLETE = (file) =>
  'The transcription files on the memory card are incomplete (' + file +
  ' could not be loaded). Copy them to the card again, then try again.';

function cardError(file) {
  const err = new Error(CARD_INCOMPLETE(file));
  err.cardIncomplete = true;
  return err;
}

function clearLoadTimer() {
  if (loadTimer) { clearTimeout(loadTimer); loadTimer = null; }
}

function armLoadTimer() {
  clearLoadTimer();
  if (!loadWaiters.length) return;
  loadTimer = setTimeout(() => {
    loadTimer = null;
    failWorker(new Error('transcription setup stopped responding'));
  }, LOAD_IDLE_MS);
}

function settleLoadWaiters(fn) {
  clearLoadTimer();
  const ws = loadWaiters; loadWaiters = [];
  ws.forEach(fn);
}

// The worker is unusable: fail everything waiting on it and drop it, so the
// next load() starts a fresh one instead of posting to a dead worker.
function failWorker(err) {
  settleLoadWaiters((w) => w.reject(err));
  pending.forEach((p) => p.reject(err));
  pending.clear();
  if (worker) { worker.terminate(); worker = null; }
  workerReadyFor = null;
  activeDevice = null;
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
      armLoadTimer();
      if (onProgressCb) onProgressCb({ pct: msg.pct, label: msg.label });
    } else if (msg.type === 'status') {
      armLoadTimer();
      // 'preparing' (compiling the session) / 'warming' (warmup inference) —
      // the post-download phases that have no % so the UI doesn't look frozen.
      if (onProgressCb) onProgressCb({ phase: msg.phase });
    } else if (msg.type === 'loaded') {
      activeDevice = msg.device;
      settleLoadWaiters((x) => x.resolve());
    } else if (msg.type === 'result') {
      const p = pending.get(msg.id);
      if (p) { pending.delete(msg.id); p.resolve(msg.text); }
    } else if (msg.type === 'error') {
      // msg.missing: a file the worker could not load from the device.
      const err = msg.missing ? cardError(msg.missing) : new Error(msg.error);
      if (msg.id != null && pending.has(msg.id)) {
        const p = pending.get(msg.id); pending.delete(msg.id); p.reject(err);
      } else {
        settleLoadWaiters((x) => x.reject(err));
      }
    }
  };
  w.onerror = (e) => {
    if (worker !== w) return;
    // A script that fails to load (missing, or served as the wrong type)
    // fires a bare error event with no message; a crash inside a running
    // worker carries one.
    failWorker(e && e.message
      ? new Error('transcription stopped unexpectedly: ' + e.message)
      : cardError('engine.worker.js'));
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
  await modelFilesClear();
  await settingsDeletePrefix(READY_PREFIX);
  await settingDelete(LEGACY_READY_SETTING);
  // Drop the worker so the next load rebuilds cleanly.
  if (worker) { worker.terminate(); worker = null; workerReadyFor = null; activeDevice = null; }
}

export function gpuAvailable() {
  return typeof navigator !== 'undefined' && 'gpu' in navigator;
}

// Which backend the worker actually built on ('webgpu' | 'wasm' | null).
export function backend() { return activeDevice; }

// Build (or reuse) the recognition pipeline in the worker. onProgress({pct,label})
// fires during the model download. Resolves when the worker is ready.
export async function load(modelId, onProgress) {
  if (window.__cfTestEngine) {
    if (window.__cfTestEngine.load) await window.__cfTestEngine.load(modelId);
    if (onProgress) onProgress({ pct: 100, label: 'test engine' });
    return;
  }
  if (!MODELS.some((m) => m.id === modelId)) throw new Error('unknown transcription model');
  if (workerReadyFor === modelId && worker) return;

  onProgressCb = onProgress || null;
  const model = MODELS.find((m) => m.id === modelId);
  ensureWorker().postMessage({
    type: 'load',
    modelId,
    english: !!(model && model.english),
    useGpu: gpuAvailable(),
  });
  await new Promise((resolve, reject) => {
    loadWaiters.push({ resolve, reject });
    armLoadTimer();
  });
  workerReadyFor = modelId;
  onProgressCb = null;
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
