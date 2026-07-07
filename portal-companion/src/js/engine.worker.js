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
//   in : {type:'load', modelId, english, useGpu}
//        {type:'transcribe', id, audio:Float32Array, english, longForm}
//   out: {type:'progress', pct, label} | {type:'loaded', device}
//        {type:'result', id, text} | {type:'error', id?, error}

import { modelFileGet, modelFilePut, settingSet } from './db.js';

let pipelinePromise = null;
let activeModel = null;
let activeDevice = null;

async function buildPipeline(modelId, english, useGpu) {
  const mod = await import('/web/vendor/transformers.min.js');
  const { env, pipeline } = mod;
  env.allowLocalModels = false;
  env.useBrowserCache = false;
  env.useCustomCache = true;
  env.customCache = {
    match: async (request) => {
      const url = typeof request === 'string' ? request : request.url;
      const blob = await modelFileGet(url);
      return blob ? new Response(blob) : undefined;
    },
    put: async (request, response) => {
      const url = typeof request === 'string' ? request : request.url;
      const blob = await response.blob();
      await modelFilePut(url, blob);
    },
  };
  if (env.backends && env.backends.onnx && env.backends.onnx.wasm) {
    env.backends.onnx.wasm.wasmPaths = '/web/vendor/ort/';
    env.backends.onnx.wasm.numThreads = 1;  // no cross-origin isolation on http
  }

  const device = useGpu ? 'webgpu' : 'wasm';
  const perFile = new Map();
  let preparingPosted = false;
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
  });
  activeModel = modelId;
  activeDevice = device;
  await settingSet('engineReadyFor', modelId);

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
  if (pipelinePromise && activeModel === modelId) return pipelinePromise;
  pipelinePromise = buildPipeline(modelId, english, useGpu);
  return pipelinePromise;
}

self.onmessage = async (e) => {
  const msg = e.data;
  try {
    if (msg.type === 'load') {
      await getPipeline(msg.modelId, msg.english, msg.useGpu);
      self.postMessage({ type: 'loaded', device: activeDevice });
      return;
    }
    if (msg.type === 'transcribe') {
      const p = await getPipeline(msg.modelId, msg.english, msg.useGpu);
      // english-only models (e.g. distil-small.en) REJECT `task`/`language`;
      // only multilingual models take them (to force English output).
      const opts = {};
      if (!msg.english) {
        opts.task = 'transcribe';
        opts.language = 'english';
      }
      if (msg.longForm) {
        opts.chunk_length_s = 30;
        opts.stride_length_s = 5;
      }
      const out = await p(msg.audio, opts);
      self.postMessage({ type: 'result', id: msg.id, text: (out && out.text ? out.text : '').trim() });
      return;
    }
  } catch (err) {
    self.postMessage({ type: 'error', id: msg && msg.id, error: String(err && err.message ? err.message : err) });
  }
};
