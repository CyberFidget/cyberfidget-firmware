// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC
//
// Stored voice notes: list them from the device, transcribe on demand
// (right on the phone), show the result in the terminal view, and write
// the transcript back to the memory card next to the note - so the user's
// words live in one user-owned place.

import { $, toast, notice, fmtBytes } from './ui.js';
import * as engine from './engine.js';
import * as device from './device.js';
import { transcriptPut } from './db.js';

let notes = [];            // from /api/recordings
let sidecars = new Set();  // basenames that already have a .txt on the card
let current = null;        // { name, text } shown in the transcript card

function base(name) {
  const dot = name.lastIndexOf('.');
  return dot > 0 ? name.slice(0, dot) : name;
}

function fmtDur(s) {
  return Math.floor(s / 60) + ':' + String(s % 60).padStart(2, '0');
}

export async function loadNotes() {
  const list = $('notesList');
  notice('notesNotice', '');
  try {
    const data = await device.getRecordings();
    if (!data.sd) {
      list.innerHTML = '<li class="empty">No memory card in the device.</li>';
      return;
    }
    notes = (data.items || []).slice().sort((a, b) => b.name.localeCompare(a.name));

    // Live listening runs off the shell in the device's flash, so the memory
    // card is only needed for turning speech into text. Say so plainly here
    // instead of letting the first transcription attempt fail for no visible
    // reason. The device reports this; it cannot be probed over HTTP, because a
    // missing path under /web/ is an ordinary 404.
    try {
      const st = await device.getStatus();
      if (st && st.captions === false) {
        notice('notesNotice',
          'Turning notes into text needs the speech pack on the memory card. ' +
          'Live listening works without it. See the guide at docs.cyberfidget.com ' +
          '(look for Phone companion) to add it.');
      }
    } catch { /* the notice is a courtesy; never block the list on it */ }

    sidecars = new Set();
    try {
      const browse = await device.getBrowse('/recordings');
      for (const e of (browse.entries || [])) {
        if (e.type === 'file' && e.name.toLowerCase().endsWith('.txt')) {
          sidecars.add(base(e.name));
        }
      }
    } catch { /* sidecar tags are a nicety */ }

    render();
  } catch (e) {
    list.innerHTML = '';
    notice('notesNotice',
      'Could not reach the device. Are you still on its network?', 'err');
  }
}

function render() {
  const list = $('notesList');
  list.innerHTML = '';
  if (notes.length === 0) {
    list.innerHTML = '<li class="empty">No voice notes yet. Record one with the Voice Notes app.</li>';
    return;
  }
  for (const n of notes) {
    const li = document.createElement('li');
    const name = document.createElement('span');
    name.className = 'name';
    name.textContent = base(n.name);
    const meta = document.createElement('span');
    meta.className = 'meta';
    meta.textContent = (n.timestamp ? n.timestamp.slice(0, 10) + ' ' : '') +
      fmtDur(n.duration) + ' ' + fmtBytes(n.bytes);
    li.append(name, meta);

    if (sidecars.has(base(n.name))) {
      const tag = document.createElement('span');
      tag.className = 'tag';
      tag.textContent = 'transcript';
      li.appendChild(tag);
      const btnView = document.createElement('button');
      btnView.className = 'btn btn-ghost';
      btnView.textContent = 'View';
      btnView.onclick = () => viewSidecar(n);
      li.appendChild(btnView);
    } else {
      const btn = document.createElement('button');
      btn.className = 'btn';
      btn.textContent = 'Transcribe';
      btn.onclick = () => transcribeNote(n, btn);
      li.appendChild(btn);
    }
    list.appendChild(li);
  }
}

async function viewSidecar(n) {
  const text = await device.fetchText('/recordings/' + encodeURIComponent(base(n.name)) + '.txt');
  if (text === null) {
    toast('Could not read that transcript from the card.');
    return;
  }
  current = { name: n.name, text, fromCard: true };
  showTranscript();
}

// Minimal WAV reader for the device's own files: 16-bit mono PCM at either
// 16,000 (Standard) or 48,000 (High). Returns Float32 at 16,000.
function wavToFloat32_16k(buf) {
  const dv = new DataView(buf);
  if (dv.getUint32(0, false) !== 0x52494646) throw new Error('not a sound file');
  let pos = 12;
  let rate = 0;
  let bits = 0;
  let channels = 0;
  let dataOff = -1;
  let dataLen = 0;
  while (pos + 8 <= dv.byteLength) {
    const id = dv.getUint32(pos, false);
    const len = dv.getUint32(pos + 4, true);
    if (id === 0x666d7420) {           // 'fmt '
      channels = dv.getUint16(pos + 10, true);
      rate = dv.getUint32(pos + 12, true);
      bits = dv.getUint16(pos + 22, true);
    } else if (id === 0x64617461) {    // 'data'
      dataOff = pos + 8;
      dataLen = Math.min(len, dv.byteLength - dataOff);
    }
    pos += 8 + len + (len & 1);
  }
  if (dataOff < 0 || bits !== 16 || channels !== 1 || (rate !== 16000 && rate !== 48000)) {
    throw new Error('unexpected recording format');
  }
  const i16 = new Int16Array(buf, dataOff, Math.floor(dataLen / 2));
  if (rate === 16000) {
    const f = new Float32Array(i16.length);
    for (let i = 0; i < i16.length; i++) f[i] = i16[i] / 32768;
    return f;
  }
  // 48k -> 16k is an exact 3:1; average each triple (cheap anti-alias).
  const outLen = Math.floor(i16.length / 3);
  const f = new Float32Array(outLen);
  for (let i = 0; i < outLen; i++) {
    f[i] = (i16[i * 3] + i16[i * 3 + 1] + i16[i * 3 + 2]) / (3 * 32768);
  }
  return f;
}

async function transcribeNote(n, btn) {
  const modelId = await engine.pickedModel();
  if (!(await engine.isDownloaded(modelId))) {
    toast('Transcription needs the one-time download - see Setup.');
    document.querySelector('.nav button[data-view="Setup"]').click();
    return;
  }
  btn.disabled = true;
  const oldLabel = btn.textContent;
  try {
    btn.textContent = 'Reading...';
    const bytes = await device.fetchBytes('/recordings/' + encodeURIComponent(n.name));
    const audio = wavToFloat32_16k(bytes);
    btn.textContent = 'Working...';
    await engine.load();
    const text = await engine.transcribe(audio);
    current = { name: n.name, text: text || '(no speech found)', fromCard: false };
    showTranscript();

    // Keep a copy on the phone, keyed by the note's recording date (today
    // when the note is undated) - this is what the Daily view gathers.
    const date = n.timestamp ? n.timestamp.slice(0, 10) : new Date().toISOString().slice(0, 10);
    transcriptPut(date, n.name, current.text).catch(() => {});

    // And write the sidecar back to the card automatically - data lives
    // with the device. The button stays for a retry if this fails.
    await saveSidecar();
  } catch (e) {
    notice('notesNotice',
      'Could not transcribe ' + base(n.name) + ': ' + (e && e.message ? e.message : e), 'err');
  } finally {
    btn.disabled = false;
    btn.textContent = oldLabel;
  }
}

function showTranscript() {
  $('transcriptTitle').textContent = base(current.name);
  $('transcriptView').textContent = current.text;
  $('transcriptCard').hidden = false;
  $('btnSaveSidecar').hidden = !!current.fromCard;
  $('transcriptCard').scrollIntoView({ behavior: 'smooth' });
}

async function saveSidecar() {
  if (!current) return;
  try {
    await device.uploadText('/recordings', base(current.name) + '.txt', current.text);
    sidecars.add(base(current.name));
    toast('Transcript saved to the device.');
    render();
  } catch (e) {
    notice('notesNotice',
      'Transcribed, but saving to the device failed: ' + e.message +
      '. Tap "Save to device" to retry.', 'err');
  }
}

export function wire() {
  $('btnSaveSidecar').onclick = saveSidecar;
  $('btnCloseTranscript').onclick = () => { $('transcriptCard').hidden = true; };
}
