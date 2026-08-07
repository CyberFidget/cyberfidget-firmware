// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC
//
// Notes - the merged view.
//
// The portal used to have a Voice notes page for browsing recordings and this
// document used to have a Transcripts view for turning them into text: two lists
// of the same files, which was the sharpest edge of the seam between the two
// surfaces. There is one destination now, and a transcript is a PROPERTY of a
// recording's row rather than a second place to visit - one right-hand slot
// carrying either the TRANSCRIBE action or the green TRANSCRIPT chip.
//
// Transcribing happens right here on the phone and the text is written back to
// the memory card beside each note, so the user's words live in one place they
// own. Playing, downloading and deleting all work with no transcription pack at
// all; only the actions that need the engine disappear behind the gate.

import { $, toast, notice, fmtBytes, ask } from './ui.js';
import * as engine from './engine.js';
import * as device from './device.js';
import { transcriptPut } from './db.js';
import { hasPack, renderGate, navigate } from './app.js';

let notes = [];             // from /api/recordings
let sidecars = new Set();   // basenames that already have a .txt on the card
let selected = new Set();   // basenames ticked for a bulk action
let openName = null;        // the one expanded row, if any
let openAudio = null;       // its <audio>, so leaving the row stops playback

function base(name) {
  const dot = name.lastIndexOf('.');
  return dot > 0 ? name.slice(0, dot) : name;
}

function fmtDur(s) {
  return Math.floor(s / 60) + ':' + String(s % 60).padStart(2, '0');
}

// "2026-08-05T14:23:11" -> "aug 5". Undated notes predate any clock being set.
const MONTHS = ['jan', 'feb', 'mar', 'apr', 'may', 'jun',
                'jul', 'aug', 'sep', 'oct', 'nov', 'dec'];
function fmtDate(ts) {
  if (!ts) return 'no date';
  const d = ts.slice(0, 10);
  const today = new Date().toISOString().slice(0, 10);
  if (d === today) return 'today';
  const [, m, day] = d.split('-');
  const mi = Number(m) - 1;
  return MONTHS[mi] ? MONTHS[mi] + ' ' + Number(day) : d;
}

const noteUrl = (name) => '/recordings/' + encodeURIComponent(name);

export async function loadNotes() {
  const list = $('notesList');
  notice('notesNotice', '');
  try {
    const data = await device.getRecordings();
    if (!data.sd) {
      notes = [];
      list.innerHTML = '<li class="empty">No memory card in the device.</li>';
      $('notesBulk').hidden = true;
      $('dailyEntry').hidden = true;
      return;
    }
    notes = (data.items || []).slice().sort((a, b) => b.name.localeCompare(a.name));

    sidecars = new Set();
    try {
      const browse = await device.getBrowse('/recordings');
      for (const e of (browse.entries || [])) {
        if (e.type === 'file' && e.name.toLowerCase().endsWith('.txt')) {
          sidecars.add(base(e.name));
        }
      }
    } catch { /* sidecar tags are a nicety, never block the list on them */ }

    render();
  } catch (e) {
    list.innerHTML = '';
    $('notesBulk').hidden = true;
    notice('notesNotice',
      'Could not reach the device. Are you still on its network?', 'err');
  }
}

function render() {
  const list = $('notesList');

  // Live listening works with nothing on the card; turning speech into text does
  // not. When the pack is missing the TRANSCRIBE actions and the Daily note
  // entry disappear and one gate explains why - never a page of greyed-out
  // buttons for a device that is working exactly as shipped. Transcript chips
  // stay: sidecars already on the card still open.
  renderGate('notesGate',
    "Transcription isn't set up on this memory card.",
    'Playing, downloading and deleting notes all work without it. To turn notes ' +
    'into text - and get live captions - add the transcription pack to the card.');
  // The Daily note entry goes with them: it is made FROM transcripts, so with no
  // engine there is nothing for it to gather.
  $('dailyEntry').hidden = !hasPack();

  const total = notes.reduce((s, n) => s + (n.bytes || 0), 0);
  $('pageStatus').textContent = notes.length
    ? notes.length + ' recording' + (notes.length === 1 ? '' : 's') + ' · ' + fmtBytes(total)
    : '';

  list.innerHTML = '';
  if (notes.length === 0) {
    $('notesBulk').hidden = true;
    list.innerHTML = '<li class="empty">No voice notes yet. Record one with the ' +
      'Voice Notes app on your Cyber Fidget.</li>';
    return;
  }
  $('notesBulk').hidden = false;

  for (const n of notes) {
    list.appendChild(row(n));
  }
  refreshSelection();
}

function el(tag, cls, text) {
  const n = document.createElement(tag);
  if (cls) n.className = cls;
  if (text != null) n.textContent = text;
  return n;
}

function row(n) {
  const name = base(n.name);
  const li = el('li');
  if (openName === name) li.classList.add('open');

  const ck = el('span', 'ck' + (selected.has(name) ? ' on' : ''));
  ck.setAttribute('role', 'checkbox');
  ck.tabIndex = 0;
  ck.onclick = (ev) => { ev.stopPropagation(); toggleSelect(name); };
  li.appendChild(ck);

  const fx = el('div', 'fx');
  fx.appendChild(el('div', 'name', name));
  fx.appendChild(el('div', 'meta',
    [fmtDate(n.timestamp), fmtDur(n.duration), fmtBytes(n.bytes)].join(' · ')));
  fx.onclick = () => toggleOpen(name);
  li.appendChild(fx);

  const play = el('button', 'rowact');
  play.type = 'button';
  play.title = 'Play';
  play.appendChild(el('i', 'k-tri-r sm'));
  play.onclick = (ev) => { ev.stopPropagation(); toggleOpen(name, true); };
  li.appendChild(play);

  // One slot, two states: the action when there is no transcript, the chip when
  // there is. The action needs the engine, so it is absent behind the gate; the
  // chip is not, so it stays.
  if (sidecars.has(name)) {
    li.appendChild(el('span', 'k-chip gn', 'transcript'));
  } else if (hasPack()) {
    const b = el('button', 'rowact', 'Transcribe');
    b.type = 'button';
    b.onclick = (ev) => { ev.stopPropagation(); transcribe(n, b); };
    li.appendChild(b);
  }

  if (openName === name) li.appendChild(detail(n));
  return li;
}

// The expanded row: the kit's inline player, the transcript on a phosphor panel,
// then the row's own actions. Built lazily, so a list of fifty notes is fifty
// rows and not fifty audio elements.
function detail(n) {
  const name = base(n.name);
  const wrap = el('div', 'note-detail');

  const audio = document.createElement('audio');
  audio.preload = 'metadata';
  audio.src = noteUrl(n.name);
  openAudio = audio;
  wrap.appendChild(audio);

  const mount = el('div');
  wrap.appendChild(mount);
  CFK.player(mount, audio, 'inline');

  const panel = el('div', 'phosphor');
  const term = el('div', 'terminal', 'Loading the transcript...');
  panel.appendChild(term);
  if (sidecars.has(name)) {
    wrap.appendChild(panel);
    device.fetchText('/recordings/' + encodeURIComponent(name) + '.txt')
      .then((t) => { term.textContent = t === null ? 'Could not read that transcript from the card.' : t; })
      .catch(() => { term.textContent = 'Could not read that transcript from the card.'; });
  }

  const acts = el('div', 'note-actions');
  const dl = document.createElement('a');
  dl.className = 'rowact';
  dl.href = noteUrl(n.name);
  dl.download = n.name;
  dl.textContent = 'Download';
  acts.appendChild(dl);

  if (hasPack()) {
    const again = el('button', 'rowact', sidecars.has(name) ? 'Transcribe again' : 'Transcribe');
    again.type = 'button';
    again.onclick = () => transcribe(n, again);
    acts.appendChild(again);
  }

  // Renaming came across with the rest of the portal's voice page. Recordings
  // are born as timestamps, so this is how "Note 048" becomes "Standup recap".
  const ren = el('button', 'rowact', 'Rename');
  ren.type = 'button';
  ren.onclick = () => renameNote(n);
  acts.appendChild(ren);

  const del = el('button', 'rowact red', 'Delete');
  del.type = 'button';
  del.onclick = () => removeNotes([n.name]);
  acts.appendChild(del);
  wrap.appendChild(acts);

  return wrap;
}

function toggleOpen(name, andPlay) {
  if (openAudio) { try { openAudio.pause(); } catch { /* already gone */ } }
  openAudio = null;
  openName = (openName === name && !andPlay) ? null : name;
  render();
  if (andPlay && openAudio) openAudio.play().catch(() => {});
}

// ── Multi-select ──

function toggleSelect(name) {
  if (selected.has(name)) selected.delete(name); else selected.add(name);
  render();
}

function refreshSelection() {
  const n = selected.size;
  $('notesCount').textContent = n + ' selected';
  $('notesDownload').disabled = n === 0;
  $('notesDelete').disabled = n === 0;
  $('notesSelAll').classList.toggle('on', n > 0 && n === notes.length);
}

function toggleAll() {
  if (selected.size === notes.length) selected.clear();
  else notes.forEach((n) => selected.add(base(n.name)));
  render();
}

function selectedItems() {
  return notes
    .filter((n) => selected.has(base(n.name)))
    .map((n) => ({
      url: noteUrl(n.name),
      name: n.name,
      // The device streams a WAV header ahead of the sample bytes it reports.
      bytes: (n.bytes || 0) + 44,
      type: 'audio/wav',
    }));
}

function downloadSelected() {
  const items = selectedItems();
  if (items.length) CFK.download(items, 'voice-notes.zip', 'voice notes');
}

async function deleteSelected() {
  const names = notes.filter((n) => selected.has(base(n.name))).map((n) => n.name);
  if (names.length) await removeNotes(names);
}

// The device rejects these outright, so catch them before the round trip and
// say which character is the problem rather than echoing a failure code.
const BAD_NAME = /[\\/:*?"<>|]/;

async function renameNote(n) {
  const from = base(n.name);
  const to = await ask({
    title: 'Rename this note',
    okLabel: 'Rename',
    value: from,
    validate: (v) => {
      const t = (v || '').trim();
      if (!t) return 'Give it a name.';
      if (BAD_NAME.test(t)) return 'A name cannot contain \\ / : * ? " < > |';
      return '';
    },
  });
  if (to === null || to === from) return;
  const ext = n.name.slice(from.length);   // keep .wav, whatever it is
  try {
    await device.move('/recordings/' + n.name, '/recordings/' + to + ext);
    // The transcript sidecar is renamed by the device alongside the recording,
    // so nothing here has to chase it.
    if (openName === from) openName = to;
    selected.delete(from);
    toast('Renamed.');
    loadNotes();
  } catch (e) {
    notice('notesNotice', 'Could not rename it: ' + (e && e.message ? e.message : e), 'err');
  }
}

// Deleting destroys a file on the card with no undo, which is exactly what the
// red treatment is reserved for - and why it asks first.
async function removeNotes(names) {
  const what = names.length === 1
    ? '"' + base(names[0]) + '"'
    : names.length + ' voice notes';
  const yes = await ask({
    title: 'Delete ' + (names.length === 1 ? 'this note?' : names.length + ' notes?'),
    body: 'This removes ' + what + ' from the memory card, along with any ' +
          'transcript saved beside it. There is no undo.',
    okLabel: 'Delete',
    danger: true,
  });
  if (!yes) return;
  let ok = 0;
  for (const n of names) {
    try {
      await device.deletePath('/recordings/' + n);
      selected.delete(base(n));
      if (openName === base(n)) openName = null;
      ok++;
    } catch { /* counted below */ }
  }
  toast(ok === names.length
    ? 'Deleted ' + ok + (ok === 1 ? ' note.' : ' notes.')
    : 'Deleted ' + ok + ' of ' + names.length + '.');
  loadNotes();
}

// ── Transcription ──

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

async function transcribe(n, btn) {
  const name = base(n.name);
  const modelId = await engine.pickedModel();
  if (!(await engine.isDownloaded(modelId))) {
    toast('Transcription needs the one-time download.');
    navigate('settings/transcription');
    return;
  }
  btn.disabled = true;
  const oldLabel = btn.textContent;
  try {
    btn.textContent = 'Reading...';
    const bytes = await device.fetchBytes(noteUrl(n.name));
    const audio = wavToFloat32_16k(bytes);
    btn.textContent = 'Working...';
    await engine.load();
    const text = (await engine.transcribe(audio)) || '(no speech found)';

    // Keep a copy on the phone, keyed by the note's recording date (today when
    // the note is undated) - this is what the Daily view gathers.
    const date = n.timestamp ? n.timestamp.slice(0, 10) : new Date().toISOString().slice(0, 10);
    transcriptPut(date, n.name, text).catch(() => {});

    // And write the sidecar back to the card - data lives with the device.
    await device.uploadText('/recordings', name + '.txt', text);
    sidecars.add(name);
    toast('Transcript saved to the device.');
    openName = name;
    render();
  } catch (e) {
    notice('notesNotice',
      'Could not transcribe ' + name + ': ' + (e && e.message ? e.message : e), 'err');
    btn.disabled = false;
    btn.textContent = oldLabel;
  }
}

export function wire() {
  $('notesSelAll').onclick = toggleAll;
  $('notesDownload').onclick = downloadSelected;
  $('notesDelete').onclick = deleteSelected;
}
