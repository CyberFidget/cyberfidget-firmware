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

import { modelFilesClear, modelFilesList, settingGet, settingSet, tryPersist } from './db.js';

// Captions run single-thread on the phone's CPU (WebGPU and multi-thread WASM
// both need a secure context, which the device's plain-http origin can't be),
// so SMALLER = faster live captions. Tiers are ordered fastest-first; the
// first entry is the default, chosen for live-caption responsiveness.
export const MODELS = [
  {
    id: 'onnx-community/whisper-tiny.en',
    label: 'Fastest - English, best for live captions (about 40 MB)',
    sizeMB: 40,
    english: true,
  },
  {
    id: 'onnx-community/whisper-base',
    label: 'Balanced - more accurate, handles other languages (about 60 MB)',
    sizeMB: 60,
    english: false,
  },
  {
    id: 'onnx-community/distil-small.en',
    label: 'Most accurate - English, slower (best for saved notes) (about 90 MB)',
    sizeMB: 90,
    english: true,
  },
];

const SETTING_MODEL = 'engineModel';
const SETTING_READY = 'engineReadyFor';

let worker = null;
let workerReadyFor = null;        // modelId the worker has built a pipeline for
let activeDevice = null;          // 'webgpu' | 'wasm' (reported by the worker)
let onProgressCb = null;
let nextReqId = 1;
const pending = new Map();        // transcribe id -> {resolve, reject}
let loadWaiters = [];             // resolvers waiting on 'loaded'

function ensureWorker() {
  if (worker) return worker;
  // Separate on-demand file (NOT inlined in the shell) so page load stays a
  // single request; the worker is fetched only when captions/transcription
  // start. Module worker so it can `import()` the device-served library.
  worker = new Worker('/web/engine.worker.js', { type: 'module' });
  worker.onmessage = (e) => {
    const msg = e.data;
    if (msg.type === 'progress') {
      if (onProgressCb) onProgressCb({ pct: msg.pct, label: msg.label });
    } else if (msg.type === 'status') {
      // 'preparing' (compiling the session) / 'warming' (warmup inference) —
      // the post-download phases that have no % so the UI doesn't look frozen.
      if (onProgressCb) onProgressCb({ phase: msg.phase });
    } else if (msg.type === 'loaded') {
      activeDevice = msg.device;
      const ws = loadWaiters; loadWaiters = [];
      ws.forEach((w) => w.resolve());
    } else if (msg.type === 'result') {
      const p = pending.get(msg.id);
      if (p) { pending.delete(msg.id); p.resolve(msg.text); }
    } else if (msg.type === 'error') {
      if (msg.id != null && pending.has(msg.id)) {
        const p = pending.get(msg.id); pending.delete(msg.id); p.reject(new Error(msg.error));
      } else {
        const ws = loadWaiters; loadWaiters = [];
        ws.forEach((w) => w.reject(new Error(msg.error)));
      }
    }
  };
  worker.onerror = (e) => {
    const err = new Error('transcription worker failed: ' + (e.message || 'unknown'));
    const ws = loadWaiters; loadWaiters = [];
    ws.forEach((w) => w.reject(err));
    pending.forEach((p) => p.reject(err));
    pending.clear();
  };
  return worker;
}

export function listModels() { return MODELS; }

export async function pickedModel() {
  const id = await settingGet(SETTING_MODEL, MODELS[0].id);
  return MODELS.some((m) => m.id === id) ? id : MODELS[0].id;
}

export async function pickModel(id) {
  if (MODELS.some((m) => m.id === id)) await settingSet(SETTING_MODEL, id);
}

// "Downloaded" = a full pipeline build previously completed for this model,
// so every file it needs is in IndexedDB.
export async function isDownloaded(id) {
  if (window.__cfTestEngine) return !!window.__cfTestEngine.downloaded;
  const ready = await settingGet(SETTING_READY, '');
  return ready === id;
}

export async function downloadedBytes() {
  const files = await modelFilesList();
  return files.reduce((a, f) => a + f.size, 0);
}

export async function dropDownload() {
  await modelFilesClear();
  await settingSet(SETTING_READY, '');
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
export async function load(onProgress) {
  if (window.__cfTestEngine) {
    if (onProgress) onProgress({ pct: 100, label: 'test engine' });
    return;
  }
  const modelId = await pickedModel();
  if (workerReadyFor === modelId && worker) return;

  onProgressCb = onProgress || null;
  const model = MODELS.find((m) => m.id === modelId);
  ensureWorker().postMessage({
    type: 'load',
    modelId,
    english: !!(model && model.english),
    useGpu: gpuAvailable(),
  });
  await new Promise((resolve, reject) => loadWaiters.push({ resolve, reject }));
  workerReadyFor = modelId;
  onProgressCb = null;
  await settingSet(SETTING_READY, modelId);
  tryPersist();
}

export function loaded() {
  if (window.__cfTestEngine) return true;
  return workerReadyFor !== null;
}

// Transcribe mono Float32 PCM at 16,000 samples/second. Returns plain text.
// Runs in the worker; the audio buffer is transferred (zero-copy).
export async function transcribe(float32Audio) {
  if (window.__cfTestEngine) return window.__cfTestEngine.transcribe(float32Audio);
  const modelId = workerReadyFor || (await pickedModel());
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
