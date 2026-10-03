// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC
//
// Speech-to-text WORKER. The model pipeline and every inference run here, OFF
// the main thread, so transcription never janks the live waveform or the
// caption UI (the symptom that drove this: smooth waveform when only
// listening, laggy waveform the moment captions ran -> inference was blocking
// the UI thread).
//
// The worker loads the vendored transformers.js from the device (/web/vendor/)
// and caches model weights in IndexedDB (shared with the page) so offline use
// survives a cleared browser cache. Messages:
//   in : {type:'load', id, modelId, english, useGpu}
//        {type:'transcribe', id, audio:Float32Array, english, longForm}
//   out: {type:'progress', pct, label} | {type:'status', phase} | {type:'building'}
//        {type:'loaded', id, device} | {type:'result', id, text}
//        {type:'error', id?, error, missing?, lost?}
//   (`id` on load replies echoes the load request, so overlapping loads can't
//   answer each other; `missing` names a device file that is absent or of the
//   wrong type; `lost` means the device stopped answering.)

import { modelFileCache, lengthCheckedFetch, settingSet } from './db.js';
import { RUNTIME_FILES, probeDeviceFiles } from './pack.js';

const LIBRARY = '/web/vendor/transformers.min.js';

// transformers.js downloads with the global fetch; make a download that ends
// early throw instead of being cached zero-padded.
const deviceFetch = self.fetch.bind(self);
self.fetch = lengthCheckedFetch(deviceFetch);

// After a failure, ask the device about the files that step needed, so the
// page can say "the card copy is incomplete" or "lost the connection" instead
// of guessing. Neither tag means the device files are fine (e.g. the model
// download from the internet failed).
async function diagnose(err, urls) {
  const e = err instanceof Error ? err : new Error(String(err));
  const found = await probeDeviceFiles(urls, deviceFetch);
  if (found.state === 'missing') e.missing = found.file;
  else if (found.state === 'offline') e.lost = true;
  return e;
}

let pipelinePromise = null;
let pipelineModel = null;         // the model pipelinePromise is building/built
let activeModel = null;
let activeDevice = null;

async function buildPipeline(modelId, english, useGpu) {
  let mod;
  try {
    mod = await import(LIBRARY);
  } catch (err) {
    throw await diagnose(err, [LIBRARY]);
  }
  // The library is up. What follows is either the model download (progress
  // events) or, for a model already saved, building the session with no
  // events at all - tell the page so it allows for that long quiet stretch.
  self.postMessage({ type: 'building' });
  const { env, pipeline } = mod;
  env.allowLocalModels = false;
  env.useBrowserCache = false;
  env.useCustomCache = true;
  env.customCache = modelFileCache;
  if (env.backends && env.backends.onnx && env.backends.onnx.wasm) {
    env.backends.onnx.wasm.wasmPaths = '/web/vendor/ort/';
    env.backends.onnx.wasm.numThreads = 1;  // no cross-origin isolation on http
  }

  const device = useGpu ? 'webgpu' : 'wasm';
  const perFile = new Map();
  let preparingPosted = false;
  // The session build fetches the runtime from the card; a failure here is a
  // bad card copy, a lost device connection, or (neither) a model download.
  const p = await pipeline('automatic-speech-recognition', modelId, {
    dtype: 'q4',
    device,
    progress_callback: (ev) => {
      if (ev.status === 'progress' && ev.total) {
        perFile.set(ev.file, { loaded: ev.loaded, total: ev.total });
        let loaded = 0; let total = 0;
        for (const f of perFile.values()) { loaded += f.loaded; total += f.total; }
        const pct = Math.round((loaded / total) * 100);
        self.postMessage({ type: 'progress', pct, label: ev.file });
        // Downloads done -> the pipeline is now building the inference session
        // (and on WebGPU, compiling shaders) with NO further progress events.
        // Tell the UI so it doesn't look frozen at "downloading 100%".
        if (pct >= 100 && !preparingPosted) {
          preparingPosted = true;
          self.postMessage({ type: 'status', phase: 'preparing' });
        }
      }
    },
  }).catch(async (err) => { throw await diagnose(err, RUNTIME_FILES); });
  activeModel = modelId;
  activeDevice = device;
  await settingSet('engineReadyFor:' + modelId, true);

  // Warm up: the FIRST real inference compiles graph/shaders and is slow. Run
  // one silent second now (under a "warming up" status) so the user's first
  // actual caption is fast, not a multi-second stall.
  self.postMessage({ type: 'status', phase: 'warming' });
  try {
    const warm = new Float32Array(16000);  // 1s of silence
    await p(warm, english ? {} : { task: 'transcribe', language: 'english' });
  } catch (e) { /* warmup is best-effort */ }

  return p;
}

async function getPipeline(modelId, english, useGpu) {
  // Keyed by the model being BUILT, not the last one that finished: otherwise
  // a request for the old model during (or after a failed) build of a new one
  // would be handed the new model's pipeline, or its rejection.
  if (pipelinePromise && pipelineModel === modelId) return pipelinePromise;
  const build = buildPipeline(modelId, english, useGpu);
  pipelinePromise = build;
  pipelineModel = modelId;
  // A failed build is not kept: the next request tries again.
  build.catch(() => {
    if (pipelinePromise === build) { pipelinePromise = null; pipelineModel = null; }
  });
  return build;
}

self.onmessage = async (e) => {
  const msg = e.data;
  try {
    if (msg.type === 'load') {
      await getPipeline(msg.modelId, msg.english, msg.useGpu);
      self.postMessage({ type: 'loaded', id: msg.id, device: activeDevice });
      return;
    }
    if (msg.type === 'transcribe') {
      const p = await getPipeline(msg.modelId, msg.english, msg.useGpu);
      // English-only models reject `task`/`language`;
      // only multilingual models take them (to force English output).
      const opts = {};
      if (!msg.english) {
        opts.task = 'transcribe';
        opts.language = 'english';
      }
      // Only the Whisper-family pipelines support chunked long recordings.
      if (msg.longForm && !msg.modelId.toLowerCase().includes('moonshine')) {
        opts.chunk_length_s = 30;
        opts.stride_length_s = 5;
      }
      const out = await p(msg.audio, opts);
      self.postMessage({ type: 'result', id: msg.id, text: (out && out.text ? out.text : '').trim() });
      return;
    }
  } catch (err) {
    self.postMessage({
      type: 'error',
      id: msg && msg.id,
      error: String(err && err.message ? err.message : err),
      missing: (err && err.missing) || undefined,
      lost: (err && err.lost) || undefined,
    });
  }
};
