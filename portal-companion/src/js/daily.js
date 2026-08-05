// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC
//
// Daily note: gather one day's transcripts (live sessions kept on the
// phone + transcripts saved on the card), show EXACTLY what would be sent,
// then - with explicit per-action consent that names the provider - have
// the user's own provider account write a markdown daily note. The result
// downloads locally and can be saved to the device (/notes/). Nothing in
// this flow ever touches cyberfidget.com.

import { $, toast, notice, noticeWithLink, askOverlay, todayISO } from './ui.js';
import * as device from './device.js';
import * as providers from './providers.js';
import { transcriptsForDate } from './db.js';

let assembled = '';     // the exact text offered to the provider
let assembledDate = '';
let noteMarkdown = '';

const SYSTEM_PROMPT =
  'You turn a day of rough voice-note transcripts into one tidy daily note. ' +
  'Write markdown with a short summary paragraph, then sections for key points, ' +
  'decisions, and follow-ups (omit empty sections). Keep the writer\'s voice; ' +
  'do not invent facts that are not in the transcripts.';

async function assemble() {
  const date = $('dailyDate').value || todayISO();
  assembledDate = date;
  notice('dailyNotice', '');
  $('assembledCard').hidden = true;
  $('dailyResultCard').hidden = true;

  const parts = [];

  // Live-session transcripts and phone-side note transcripts (IndexedDB).
  const local = await transcriptsForDate(date);
  for (const t of local) {
    parts.push({ source: t.source, text: t.text });
  }

  // Transcript files on the card whose note carries this date.
  try {
    const data = await device.getRecordings();
    if (data.sd) {
      for (const n of (data.items || [])) {
        if (!n.timestamp || !n.timestamp.startsWith(date)) continue;
        if (parts.some((p) => p.source === n.name)) continue;  // already local
        const dot = n.name.lastIndexOf('.');
        const sidecar = (dot > 0 ? n.name.slice(0, dot) : n.name) + '.txt';
        const text = await device.fetchText('/recordings/' + encodeURIComponent(sidecar));
        if (text) parts.push({ source: n.name, text: text.trim() });
      }
    }
  } catch {
    notice('dailyNotice',
      'Could not check the device for transcripts - using what your browser has.', '');
  }

  if (parts.length === 0) {
    notice('dailyNotice',
      'Nothing transcribed for ' + date + ' yet. Caption a live session or ' +
      'transcribe a voice note first (Notes tab).', '');
    return;
  }

  assembled = 'Voice transcripts for ' + date + ':\n\n' +
    parts.map((p) => '## ' + p.source + '\n' + p.text).join('\n\n');
  $('assembledPreview').textContent = assembled;
  $('assembledCard').hidden = false;
}

async function summarize() {
  const prov = providers.getProvider();
  const provLabel = providers.PROVIDERS[prov].label;

  if (!providers.getKey(prov)) {
    // No key: stop at setup, with the provider's own key page one tap away.
    notice('dailyNotice', noticeWithLink(
      'No ' + provLabel + ' key saved yet. Add one in Setup - you can create a key at',
      'the ' + provLabel + ' console.', providers.consoleLink(prov)), 'err');
    document.querySelector('.nav button[data-view="Setup"]').click();
    return;
  }

  if (!navigator.onLine) {
    notice('dailyNotice',
      'Writing the note needs internet. In the portal\'s Settings, connect your ' +
      'Cyber Fidget to your home WiFi, stay on that WiFi yourself too, then retry.', 'err');
    return;
  }

  // Per-action consent, naming the provider. Nothing pre-checked, nothing
  // remembered - this gate runs every single time.
  $('summaryConsentText').textContent =
    'Send the transcripts for ' + assembledDate + ' (shown above, ' +
    assembled.length + ' characters) to ' + provLabel + ' to write your daily note?';
  const yes = await askOverlay('summaryOverlay', 'btnSummaryGo', 'btnSummaryNo');
  if (!yes) return;

  $('btnSummarize').disabled = true;
  $('btnSummarize').textContent = 'Writing...';
  try {
    noteMarkdown = await providers.generate(SYSTEM_PROMPT, assembled, 2000);
    $('dailyResult').textContent = noteMarkdown;
    $('dailyResultCard').hidden = false;
    $('dailyResultCard').scrollIntoView({ behavior: 'smooth' });
    notice('dailyNotice', '');
  } catch (e) {
    if (e && e.authProblem) {
      notice('dailyNotice', noticeWithLink(
        (e.message || 'The provider refused the key.') + ' Check it at',
        'the ' + provLabel + ' console.', providers.consoleLink(prov)), 'err');
    } else if (e instanceof TypeError) {
      // fetch network failure: phone has no route to the provider.
      notice('dailyNotice',
        'Could not reach ' + provLabel + '. If you are on the CyberFidget network, ' +
        'switch the device to your home WiFi (portal Settings) so you keep ' +
        'internet, then retry.', 'err');
    } else {
      notice('dailyNotice', (e && e.message) ? e.message : 'Something went wrong.', 'err');
    }
  } finally {
    $('btnSummarize').disabled = false;
    $('btnSummarize').textContent = 'Write my daily note...';
  }
}

function downloadNote() {
  if (!noteMarkdown) return;
  const a = document.createElement('a');
  a.href = URL.createObjectURL(new Blob([noteMarkdown], { type: 'text/markdown' }));
  a.download = assembledDate + '.md';
  document.body.appendChild(a);
  a.click();
  a.remove();
  setTimeout(() => URL.revokeObjectURL(a.href), 5000);
}

async function saveNoteToDevice() {
  if (!noteMarkdown) return;
  try {
    await device.mkdir('/notes');
    await device.uploadText('/notes', assembledDate + '.md', noteMarkdown);
    toast('Saved to the device as /notes/' + assembledDate + '.md');
  } catch (e) {
    notice('dailyNotice', 'Could not save to the device: ' + e.message, 'err');
  }
}

export function wire() {
  $('dailyDate').value = todayISO();
  $('btnAssemble').onclick = assemble;
  $('btnSummarize').onclick = summarize;
  $('btnDownloadNote').onclick = downloadNote;
  $('btnSaveNoteDevice').onclick = saveNoteToDevice;
}
