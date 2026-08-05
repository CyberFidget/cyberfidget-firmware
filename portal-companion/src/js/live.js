// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC
//
// Live listening session: connect to the device's live link, draw the
// waveform, optionally play the audio, and (once the transcription pack is
// ready) caption speech and send the captions back so they show on the
// device's screen.
//
// Link contract (mirrors the firmware's LiveLinkProtocol.h): binary frames
// are raw 16-bit mono samples at 16,000/s; text frames are JSON typed by
// "t" - we receive hello/err/stop and send time/caption.
//
// Test hook: window.__cfTestWS (a WebSocket-compatible constructor) lets
// the harness script the device side without hardware.

import { $, toast, askOverlay, notice } from './ui.js';
import { localNaiveEpochMs, todayISO } from './ui.js';
import * as engine from './engine.js';
import * as keepawake from './keepawake.js';
import { transcriptPut } from './db.js';

const SAMPLE_RATE = 16000;

let sock = null;
let wantSession = false;       // user intent (drives auto-retry)
let retries = 0;
let sessionStartMs = 0;
let helloFw = '';

// Audio playback ("Hear it") - OFF by default: with the phone next to the
// device this would feed back; captions are the headline use.
let hearing = false;
let audioCtx = null;
let playHead = 0;

// Waveform: a rolling buffer of recent peaks, drawn on animation frames.
const WAVE_POINTS = 240;
const wavePeaks = new Float32Array(WAVE_POINTS);
let waveWrite = 0;
let rafId = 0;

// Captions
let captionsOn = false;
// One IndexedDB record per caption session: the source key is fixed at
// caption start (not per commit), so successive commits overwrite one
// growing transcript instead of leaving overlapping copies that the Daily
// view would assemble twice.
let liveSource = '';
let capChunks = [];            // Float32Array chunks since the last commit
let capSamples = 0;
let inferBusy = false;
let lastInferSamples = 0;
let liveLineStart = '';        // committed text shown in the feed
const MAX_WINDOW_S = 10;       // inference window cap
const COMMIT_MAX_S = 8;        // force a final by this much pending audio
const MIN_COMMIT_S = 2;
const HOP_S = 1.0;             // new audio required between inferences
const SILENCE_RMS = 0.012;     // post-gain speech sits well above this
const SILENCE_TAIL_S = 0.6;
const MAX_PENDING_S = 15;      // hard cap on un-inferred backlog (bounds worst-case lag)

let sessionTimer = 0;

function deviceHost() {
  return window.location.host || '192.168.4.1';
}

function setSessionChip(text, ok = true) {
  const chip = $('chipSession');
  chip.className = 'chip ' + (ok ? 'on' : 'warn');
  $('chipSessionText').textContent = text;
}

// ── Waveform ──

function pushWavePeaks(f32) {
  // One peak per 10ms of audio keeps the scroll speed font-independent.
  const step = SAMPLE_RATE / 100;
  for (let i = 0; i + step <= f32.length; i += step) {
    let peak = 0;
    for (let j = i; j < i + step; j++) {
      const v = Math.abs(f32[j]);
      if (v > peak) peak = v;
    }
    wavePeaks[waveWrite % WAVE_POINTS] = peak;
    waveWrite++;
  }
}

function drawWave() {
  const canvas = $('wave');
  const ctx = canvas.getContext('2d');
  const w = canvas.width;
  const h = canvas.height;
  const css = getComputedStyle(document.documentElement);
  const fg = css.getPropertyValue('--cf-oled-fg').trim() || '#b8f1ff';
  const grid = css.getPropertyValue('--cf-ink-faint').trim() || '#3a3055';

  ctx.clearRect(0, 0, w, h);
  ctx.strokeStyle = grid;
  ctx.globalAlpha = 0.5;
  ctx.beginPath();
  ctx.moveTo(0, h / 2);
  ctx.lineTo(w, h / 2);
  ctx.stroke();
  ctx.globalAlpha = 1;

  ctx.strokeStyle = fg;
  ctx.lineWidth = 2;
  ctx.shadowColor = fg;
  ctx.shadowBlur = 8;
  ctx.beginPath();
  for (let i = 0; i < WAVE_POINTS; i++) {
    const p = wavePeaks[(waveWrite + i) % WAVE_POINTS];
    const x = (i / (WAVE_POINTS - 1)) * w;
    const y = h / 2 - p * (h / 2 - 6);
    if (i === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y);
  }
  for (let i = WAVE_POINTS - 1; i >= 0; i--) {
    const p = wavePeaks[(waveWrite + i) % WAVE_POINTS];
    const x = (i / (WAVE_POINTS - 1)) * w;
    ctx.lineTo(x, h / 2 + p * (h / 2 - 6));
  }
  ctx.stroke();
  ctx.shadowBlur = 0;

  if (wantSession) rafId = requestAnimationFrame(drawWave);
}

// ── Audio playback (linear resample to the context rate) ──

function playFrame(f32) {
  if (!hearing || !audioCtx) return;
  const ratio = audioCtx.sampleRate / SAMPLE_RATE;
  const outLen = Math.floor(f32.length * ratio);
  const buf = audioCtx.createBuffer(1, outLen, audioCtx.sampleRate);
  const out = buf.getChannelData(0);
  for (let i = 0; i < outLen; i++) {
    const pos = i / ratio;
    const i0 = Math.floor(pos);
    const i1 = Math.min(i0 + 1, f32.length - 1);
    out[i] = f32[i0] + (f32[i1] - f32[i0]) * (pos - i0);
  }
  const src = audioCtx.createBufferSource();
  src.buffer = buf;
  src.connect(audioCtx.destination);
  // Jitter buffer: schedule ~350ms ahead; re-seat after an underrun so a
  // WiFi power-save burst causes one gap, not permanent drift.
  const now = audioCtx.currentTime;
  if (playHead < now + 0.05) playHead = now + 0.35;
  src.start(playHead);
  playHead += buf.duration;
}

// ── Captions ──

function feedAppend(finalText, partialText) {
  const feed = $('captionFeed');
  feed.textContent = '';
  if (liveLineStart) {
    feed.append(liveLineStart.trim() + ' ');
  }
  if (partialText) {
    const span = document.createElement('span');
    span.className = 'partial';
    span.textContent = partialText.trim();
    feed.appendChild(span);
  }
  feed.scrollTop = feed.scrollHeight;
}

function sendCaption(text, isFinal) {
  if (!sock || sock.readyState !== 1) return;
  // The device shows a few lines; send a bounded tail so the message stays
  // small (the firmware truncates beyond 256 anyway).
  const bounded = text.length > 200 ? text.slice(-200) : text;
  sock.send(JSON.stringify({ t: 'caption', text: bounded, final: isFinal }));
}

function pendingSeconds() {
  return capSamples / SAMPLE_RATE;
}

function tailIsSilent() {
  const need = Math.floor(SILENCE_TAIL_S * SAMPLE_RATE);
  let have = 0;
  let sum = 0;
  for (let c = capChunks.length - 1; c >= 0 && have < need; c--) {
    const arr = capChunks[c];
    for (let i = arr.length - 1; i >= 0 && have < need; i--) {
      sum += arr[i] * arr[i];
      have++;
    }
  }
  if (have < need) return false;
  return Math.sqrt(sum / have) < SILENCE_RMS;
}

function takeWindow() {
  const maxSamples = MAX_WINDOW_S * SAMPLE_RATE;
  const total = Math.min(capSamples, maxSamples);
  const out = new Float32Array(total);
  let pos = total;
  for (let c = capChunks.length - 1; c >= 0 && pos > 0; c--) {
    const arr = capChunks[c];
    const take = Math.min(arr.length, pos);
    out.set(arr.subarray(arr.length - take), pos - take);
    pos -= take;
  }
  return out;
}

// Drop exactly `count` samples from the FRONT of the pending audio (the
// part a committed inference consumed) - anything that streamed in during
// the inference stays pending for the next window.
function dropPending(count) {
  while (count > 0 && capChunks.length > 0) {
    const head = capChunks[0];
    if (head.length <= count) {
      count -= head.length;
      capSamples -= head.length;
      capChunks.shift();
    } else {
      capChunks[0] = head.subarray(count);
      capSamples -= count;
      count = 0;
    }
  }
}

async function inferTick() {
  if (!captionsOn || inferBusy) return;
  if (capSamples - lastInferSamples < HOP_S * SAMPLE_RATE) return;
  if (pendingSeconds() < 1.0) return;

  inferBusy = true;
  const samplesAtStart = capSamples;
  lastInferSamples = capSamples;
  const commitAfter = pendingSeconds() >= COMMIT_MAX_S ||
    (pendingSeconds() >= MIN_COMMIT_S && tailIsSilent());
  const window = takeWindow();
  try {
    const text = await engine.transcribe(window);
    if (!captionsOn) return;
    if (commitAfter) {
      if (text) {
        sendCaption(text, true);
        liveLineStart += (liveLineStart ? ' ' : '') + text;
        feedAppend(liveLineStart, '');
        transcriptPut(todayISO(), liveSource, liveLineStart).catch(() => {});
      }
      dropPending(samplesAtStart);   // keep audio that arrived mid-inference
      lastInferSamples = Math.max(0, lastInferSamples - samplesAtStart);
    } else if (text) {
      sendCaption(text, false);
      feedAppend(liveLineStart, text);
    }
  } catch (e) {
    notice('sessionNotice', 'Caption trouble: ' + (e && e.message ? e.message : e), 'err');
  } finally {
    inferBusy = false;
    // Audio piles up while an inference runs (incoming frames see busy and
    // bail), and a network stall can stop frames entirely - so re-arm a
    // tick ourselves rather than relying on the next frame to drive it.
    if (captionsOn && capSamples - lastInferSamples >= HOP_S * SAMPLE_RATE) {
      setTimeout(inferTick, 250);
    }
  }
}

// ── Socket ──

function handleBinary(buf) {
  const i16 = new Int16Array(buf);
  const f32 = new Float32Array(i16.length);
  for (let i = 0; i < i16.length; i++) f32[i] = i16[i] / 32768;
  pushWavePeaks(f32);
  playFrame(f32);
  if (captionsOn) {
    capChunks.push(f32);
    capSamples += f32.length;
    // Bound memory + lag if inference can't keep up with real time: drop the
    // oldest pending audio past MAX_PENDING_S - skipping a moment of speech
    // beats captions falling minutes behind.
    const cap = MAX_PENDING_S * SAMPLE_RATE;
    while (capSamples > cap && capChunks.length > 1) {
      capSamples -= capChunks[0].length;
      capChunks.shift();
    }
    inferTick();
  }
}

function handleText(str) {
  let msg;
  try { msg = JSON.parse(str); } catch { return; }
  if (msg.t === 'hello') {
    helloFw = msg.fw || '';
    retries = 0;
    setSessionChip('live - ' + (msg.rate / 1000) + 'k');
    notice('sessionNotice', '');
    // Piggyback a clock sync so recordings made after this session carry
    // real timestamps (same semantics as the portal page's clock set).
    sock.send(JSON.stringify({ t: 'time', epoch: localNaiveEpochMs() }));
  } else if (msg.t === 'err') {
    wantSession = false;
    const why = msg.reason || 'the device said no';
    const friendly = /busy/i.test(why)
      ? 'Someone else is already listening to this device. One listener at a time.'
      : 'The device could not start listening: ' + why;
    endSession(friendly, 'err');
  } else if (msg.t === 'stop') {
    wantSession = false;
    endSession('The device ended the session (' + (msg.reason || 'stopped') + ').', '');
  }
}

function connect() {
  const WS = window.__cfTestWS || WebSocket;
  setSessionChip('connecting...', false);
  sock = new WS('ws://' + deviceHost() + '/ws/live');
  sock.binaryType = 'arraybuffer';
  sock.onmessage = (ev) => {
    if (typeof ev.data === 'string') handleText(ev.data);
    else handleBinary(ev.data);
  };
  sock.onclose = () => {
    sock = null;
    if (!wantSession) return;
    // Unexpected drop (AP power-save, range, device app exit): retry a few
    // times before handing control back to the user.
    if (retries < 3) {
      retries++;
      setSessionChip('reconnecting (' + retries + '/3)...', false);
      setTimeout(() => { if (wantSession) connect(); }, retries * 1200);
    } else {
      wantSession = false;
      endSession('Lost the device. Check you are still on its network, then start again.', 'err');
    }
  };
  sock.onerror = () => { /* onclose follows and handles it */ };
}

// ── Session lifecycle ──

export async function startSession() {
  const yes = await askOverlay('consentOverlay', 'btnConsentGo', 'btnConsentNo');
  if (!yes) return;

  wantSession = true;
  retries = 0;
  liveLineStart = '';
  capChunks = [];
  capSamples = 0;
  sessionStartMs = Date.now();
  wavePeaks.fill(0);

  $('listenIntro').hidden = true;
  $('sessionCard').hidden = false;
  $('captionCard').hidden = true;
  notice('sessionNotice', '');

  // The consent tap is our user gesture: create the audio engine now so
  // "Hear it" can start instantly later.
  try {
    audioCtx = new (window.AudioContext || window.webkitAudioContext)();
  } catch { audioCtx = null; }

  const locked = await keepawake.hold();
  $('wakeLockNote').textContent = locked
    ? 'This page is keeping your screen awake.'
    : 'You may need to raise your screen timeout for long sessions.';

  drawWave();
  sessionTimer = setInterval(() => {
    if (!wantSession) return;
    const s = Math.floor((Date.now() - sessionStartMs) / 1000);
    if (sock && sock.readyState === 1) {
      setSessionChip('live ' + Math.floor(s / 60) + ':' + String(s % 60).padStart(2, '0'));
    }
  }, 1000);

  connect();
}

export function endSession(message, kind) {
  wantSession = false;
  captionsOn = false;
  if (sock) {
    try { sock.close(); } catch { /* already closed */ }
    sock = null;
  }
  clearInterval(sessionTimer);
  cancelAnimationFrame(rafId);
  keepawake.release();
  if (audioCtx) {
    try { audioCtx.close(); } catch { /* best effort */ }
    audioCtx = null;
  }
  hearing = false;
  $('btnHear').textContent = '\u{1F508} Hear it';
  $('btnCaptions').textContent = 'Start captions';
  $('chipCaptions').hidden = true;

  if (message) {
    $('sessionCard').hidden = false;
    $('listenIntro').hidden = false;
    notice('sessionNotice', message, kind || '');
    setSessionChip('stopped', false);
  } else {
    $('sessionCard').hidden = true;
    $('captionCard').hidden = true;
    $('listenIntro').hidden = false;
  }
}

async function toggleCaptions() {
  if (captionsOn) {
    captionsOn = false;
    $('btnCaptions').textContent = 'Start captions';
    $('chipCaptions').hidden = true;
    return;
  }
  const modelId = await engine.pickedModel();
  if (!(await engine.isDownloaded(modelId))) {
    toast('Captions need the one-time transcription download - see Setup.');
    document.querySelector('.nav button[data-view="Setup"]').click();
    return;
  }
  $('btnCaptions').disabled = true;
  $('btnCaptions').textContent = 'Loading...';
  try {
    // Even with the model cached, starting captions builds the inference
    // session + warms up; surface those phases so the button isn't a mystery.
    await engine.load((p) => {
      if (p.phase === 'preparing') $('btnCaptions').textContent = 'Preparing...';
      else if (p.phase === 'warming') $('btnCaptions').textContent = 'Warming up...';
      else if (p.pct != null) $('btnCaptions').textContent = 'Loading ' + p.pct + '%';
    });
  } catch (e) {
    notice('sessionNotice', 'Could not start transcription: ' + (e && e.message ? e.message : e), 'err');
    $('btnCaptions').disabled = false;
    $('btnCaptions').textContent = 'Start captions';
    return;
  }
  captionsOn = true;
  capChunks = [];
  capSamples = 0;
  lastInferSamples = 0;
  liveLineStart = '';
  liveSource = 'live ' + new Date().toTimeString().slice(0, 5);
  $('btnCaptions').disabled = false;
  $('btnCaptions').textContent = 'Stop captions';
  $('captionCard').hidden = false;
  $('chipCaptions').hidden = false;
  $('chipCaptions').className = 'chip on';
  // Show the ACTUAL backend the worker built on (GPU vs CPU) - if a phone
  // with WebGPU silently fell back to CPU, that explains slow captions.
  const dev = engine.backend();
  $('chipCaptionsText').textContent =
    dev === 'webgpu' ? 'captions - GPU' : dev === 'wasm' ? 'captions - CPU' : 'captions';
  feedAppend('', '');
  toast('Speak near the device - captions may lag a few seconds.');
}

function toggleHear() {
  hearing = !hearing;
  if (hearing && audioCtx && audioCtx.state === 'suspended') audioCtx.resume();
  playHead = 0;
  $('btnHear').textContent = hearing ? '\u{1F507} Mute' : '\u{1F508} Hear it';
  if (hearing) toast('Use earbuds or another room - the device can hear your speakers too.');
}

export function wire() {
  $('btnStartSession').onclick = startSession;
  $('btnStopSession').onclick = () => endSession(null);
  $('btnHear').onclick = toggleHear;
  $('btnCaptions').onclick = toggleCaptions;
}
