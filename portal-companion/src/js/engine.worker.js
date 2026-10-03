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
//        {type:'transcribe', id, audio:Float32Array, modelId, english, longForm}
//   out: {type:'hello', protocol}            once, when the worker starts
//        {type:'building'}                   library up; a silent build may follow
//        {type:'progress', pct, label, fileDone}
//        {type:'status', phase}              'preparing' | 'warming' (for the UI)
//        {type:'loaded', id, device} | {type:'result', id, text}
//        {type:'error', id, error, kind?, file?}
//   `id` on every reply echoes its request. `kind` says what failed:
//   'card' (a device file is missing - `file` names it), 'connection' (the
//   device couldn't be reached), 'download' (the model download from the
//   internet failed), 'engine' (transcription itself failed in this browser).
//
// ONE job at a time: loads and transcriptions share a single queue, so there
// is never more than one pipeline being built, and a request is never handed
// a pipeline for a different model or one that is half-built.

import { modelFileCache, lengthCheckedFetch, settingSet } from './db.js';
import { RUNTIME_FILES, probeDeviceFiles } from './pack.js';

const PROTOCOL = 2;
const LIBRARY = '/web/vendor/transformers.min.js';

let current = null;               // { modelId, pipe, device } once built
let building = null;              // the build in progress: { downloadFailed }

function isInternet(input) {
  const url = typeof input === 'string' ? input : (input && input.url) || String(input);
  try {
    return new URL(url, self.location.href).origin !== self.location.origin;
  } catch {
    return false;
  }
}

function markDownloadFailed() { if (building) building.downloadFailed = true; }

// transformers.js downloads with the global fetch. Make a download that ends
// early throw instead of being cached zero-padded (lengthCheckedFetch), and
// note when an INTERNET download fails, so a failed build can be blamed on
// the download only when the download really did fail. (A 404 is not noted:
// the library asks for optional files that legitimately don't exist.)
const deviceFetch = self.fetch.bind(self);
const checkedFetch = lengthCheckedFetch(deviceFetch);
self.fetch = async (input, init) => {
  if (!isInternet(input)) return checkedFetch(input, init);
  let res;
  try {
    res = await checkedFetch(input, init);
  } catch (err) {
    markDownloadFailed();
    throw err;
  }
  if (res.status >= 500) markDownloadFailed();
  if (!res.body) return res;
  const reader = res.body.getReader();
  const body = new ReadableStream({
    async pull(controller) {
      try {
        const { done, value } = await reader.read();
        if (done) controller.close();
        else controller.enqueue(value);
      } catch (err) {
        markDownloadFailed();
        controller.error(err);
      }
    },
    cancel(reason) { return reader.cancel(reason); },
  });
  return new Response(body, { status: res.status, statusText: res.statusText, headers: res.headers });
};

function tagged(err, kind, file) {
  const e = err instanceof Error ? err : new Error(String(err));
  e.kind = kind;
  if (file) e.file = file;
  return e;
}

// After a failed step, ask the device about the files that step needed, then
// decide what to tell the user.
async function classify(err, urls, downloadFailed) {
  const found = await probeDeviceFiles(urls, deviceFetch);
  if (found.state === 'missing') return tagged(err, 'card', found.file);
  if (found.state === 'unreachable') return tagged(err, 'connection');
  return tagged(err, downloadFailed ? 'download' : 'engine');
}

async function build(modelId, english, useGpu) {
  let mod;
  try {
    mod = await import(LIBRARY);
  } catch (err) {
    throw await classify(err, [LIBRARY], false);
  }
  // The library is up. What follows is the model download (progress events)
  // and/or building the session, which can compute silently for a long time -
  // tell the page so it allows for that.
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
  building = { downloadFailed: false };
  let p;
  try {
    p = await pipeline('automatic-speech-recognition', modelId, {
      dtype: 'q4',
      device,
      progress_callback: (ev) => {
        if (ev.status === 'progress' && ev.total) {
          perFile.set(ev.file, { loaded: ev.loaded, total: ev.total });
          let loaded = 0; let total = 0;
          for (const f of perFile.values()) { loaded += f.loaded; total += f.total; }
          const pct = Math.round((loaded / total) * 100);
          // fileDone: this file has fully arrived. Files are discovered one at
          // a time, so the overall % can touch 100 early and drop back; what
          // follows the LAST finished file is the silent session build, and
          // the page gives every finished file the long allowance.
          self.postMessage({ type: 'progress', pct, label: ev.file, fileDone: ev.loaded >= ev.total });
          // Tell the UI it isn't frozen at "downloading 100%" - and again if a
          // later file made the download resume and finish again.
          if (pct >= 100 && !preparingPosted) {
            preparingPosted = true;
            self.postMessage({ type: 'status', phase: 'preparing' });
          } else if (pct < 100) {
            preparingPosted = false;
          }
        }
      },
    });
  } catch (err) {
    // A bad card copy (the runtime comes from the card), a device we can't
    // reach, a failed internet download, or none of those.
    throw await classify(err, RUNTIME_FILES, building.downloadFailed);
  } finally {
    building = null;
  }
  await settingSet('engineReadyFor:' + modelId, true);

  // Warm up: the FIRST real inference compiles graph/shaders and is slow. Run
  // one silent second now (under a "warming up" status) so the user's first
  // actual caption is fast, not a multi-second stall.
  self.postMessage({ type: 'status', phase: 'warming' });
  try {
    const warm = new Float32Array(16000);  // 1s of silence
    await p(warm, english ? {} : { task: 'transcribe', language: 'english' });
  } catch (e) { /* warmup is best-effort */ }

  return { modelId, pipe: p, device };
}

// The pipeline for modelId, building it (and releasing any other) if needed.
// Only ever called from inside the queue, so builds never overlap.
async function pipelineFor(modelId, english, useGpu) {
  if (current && current.modelId === modelId) return current.pipe;
  const old = current;
  current = null;
  if (old && old.pipe && typeof old.pipe.dispose === 'function') {
    try { await old.pipe.dispose(); } catch { /* released either way */ }
  }
  current = await build(modelId, english, useGpu);
  return current.pipe;
}

async function handle(msg) {
  try {
    if (msg.type === 'load') {
      await pipelineFor(msg.modelId, msg.english, msg.useGpu);
      self.postMessage({ type: 'loaded', id: msg.id, device: current.device });
      return;
    }
    if (msg.type === 'transcribe') {
      const p = await pipelineFor(msg.modelId, msg.english, msg.useGpu);
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
      let out;
      try {
        out = await p(msg.audio, opts);
      } catch (err) {
        throw tagged(err, 'engine');
      }
      self.postMessage({ type: 'result', id: msg.id, text: (out && out.text ? out.text : '').trim() });
    }
  } catch (err) {
    self.postMessage({
      type: 'error',
      id: msg && msg.id,
      error: String(err && err.message ? err.message : err),
      kind: (err && err.kind) || undefined,
      file: (err && err.file) || undefined,
    });
  }
}

// The single queue. handle() never throws, so one failed job never blocks
// the next.
let queue = Promise.resolve();
self.onmessage = (e) => {
  const msg = e.data;
  queue = queue.then(() => handle(msg));
};

self.postMessage({ type: 'hello', protocol: PROTOCOL });
