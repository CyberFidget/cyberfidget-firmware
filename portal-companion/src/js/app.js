// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC
//
// Companion bootstrap: navigation, status chips, the Setup view, and the
// (secure-context-only) offline shell registration. The feature views live
// in live.js / notes.js / daily.js.

import { $, toast, notice, askOverlay, fmtBytes, localNaiveEpochMs } from './ui.js';
import * as live from './live.js';
import * as notes from './notes.js';
import * as daily from './daily.js';
import * as engine from './engine.js';
import * as providers from './providers.js';
import * as device from './device.js';

export const COMPANION_VERSION = '0.1.0';

// ── Navigation ──

const VIEWS = ['Listen', 'Notes', 'Daily', 'Setup'];

function showView(name) {
  for (const v of VIEWS) $('view' + v).hidden = (v !== name);
  document.querySelectorAll('.nav button').forEach((b) =>
    b.classList.toggle('active', b.dataset.view === name));
  if (name === 'Notes') notes.loadNotes();
  if (name === 'Setup') refreshSetup();
}

document.querySelectorAll('.nav button').forEach((b) => {
  b.addEventListener('click', () => showView(b.dataset.view));
});

// ── Status chips ──

async function refreshDeviceChip() {
  const chip = $('chipDevice');
  try {
    const st = await device.getWifiStatus();
    chip.className = 'chip on';
    $('chipDeviceText').textContent = st.connected ? (st.ssid || 'home WiFi') : 'device AP';
    $('aboutDevice').textContent = window.location.host +
      (st.connected ? ' + ' + (st.ssid || 'WiFi') : ' (its own network)');
  } catch {
    chip.className = 'chip warn';
    $('chipDeviceText').textContent = 'device?';
    $('aboutDevice').textContent = 'not reachable';
  }
}

function refreshNetChip() {
  const chip = $('chipNet');
  if (navigator.onLine) {
    chip.className = 'chip on';
    $('chipNetText').textContent = 'online';
  } else {
    chip.className = 'chip';
    $('chipNetText').textContent = 'offline';
  }
}
window.addEventListener('online', refreshNetChip);
window.addEventListener('offline', refreshNetChip);

// ── Setup view ──

async function refreshSetup() {
  // Transcription pack
  const pick = $('modelPick');
  if (pick.options.length === 0) {
    for (const m of engine.listModels()) {
      const opt = document.createElement('option');
      opt.value = m.id;
      opt.textContent = m.label;
      pick.appendChild(opt);
    }
  }
  pick.value = await engine.pickedModel();
  const has = await engine.isDownloaded(pick.value);
  const bytes = await engine.downloadedBytes();
  $('engineStatus').textContent = has
    ? 'ready (' + fmtBytes(bytes) + ' in your browser)'
    : 'not downloaded yet';
  $('btnGetModel').textContent = has ? 'Re-download' : 'Download';
  $('btnDropModel').hidden = !has;

  // Provider
  const prov = providers.getProvider();
  $('provPick').value = prov;
  refreshProviderFields();

  $('aboutVersion').textContent = COMPANION_VERSION;
}

function refreshProviderFields() {
  const prov = $('provPick').value;
  const info = providers.PROVIDERS[prov];
  const keyEl = $('provKey');
  keyEl.value = '';
  keyEl.placeholder = providers.getKey(prov) ? 'saved - paste to replace' : 'paste your key';
  const help = $('provKeyHelp');
  help.textContent = 'Keys come from your account: ';
  const a = document.createElement('a');
  a.href = info.consoleUrl;
  a.target = '_blank';
  a.rel = 'noopener';
  a.textContent = 'open the ' + info.label + ' key page';
  help.appendChild(a);
  help.append('. It stays in this browser, for this device only.');

  const modelSel = $('provModel');
  modelSel.innerHTML = '';
  for (const m of info.models) {
    const opt = document.createElement('option');
    opt.value = m.id;
    opt.textContent = m.label;
    modelSel.appendChild(opt);
  }
  modelSel.value = providers.getModel(prov);
}

async function downloadModel() {
  const id = $('modelPick').value;
  await engine.pickModel(id);
  const m = engine.listModels().find((x) => x.id === id);

  $('modelOverlayText').textContent =
    'The transcription pack is about ' + m.sizeMB + ' MB and downloads once. ' +
    'Best on WiFi.';
  const conn = navigator.connection;
  $('modelCellWarn').hidden = !(conn && (conn.type === 'cellular' ||
    /2g|3g/.test(conn.effectiveType || '')));
  const yes = await askOverlay('modelOverlay', 'btnModelGo', 'btnModelNo');
  if (!yes) return;

  $('btnGetModel').disabled = true;
  $('modelBar').hidden = false;
  notice('engineNotice', '');
  try {
    await engine.load((p) => {
      if (p.phase === 'preparing') {
        $('modelFill').style.width = '100%';
        $('engineStatus').textContent = 'Preparing the model (this can take a moment)...';
      } else if (p.phase === 'warming') {
        $('engineStatus').textContent = 'Warming up...';
      } else {
        $('modelFill').style.width = p.pct + '%';
        $('engineStatus').textContent = 'Downloading... ' + p.pct + '%';
      }
    });
    $('modelFill').style.width = '100%';
    toast('Transcription is ready - works offline from now on.');
  } catch (e) {
    notice('engineNotice',
      'Download did not finish: ' + (e && e.message ? e.message : e) +
      '. Check your internet and try again - it resumes cleanly.', 'err');
  } finally {
    $('btnGetModel').disabled = false;
    $('modelBar').hidden = true;
    refreshSetup();
  }
}

async function dropModel() {
  $('modelOverlayText').textContent =
    'Remove the transcription download from your browser? Captions and note ' +
    'transcription will need a re-download to work again.';
  $('modelCellWarn').hidden = true;
  const go = $('btnModelGo');
  const old = go.textContent;
  go.textContent = 'Remove';
  const yes = await askOverlay('modelOverlay', 'btnModelGo', 'btnModelNo');
  go.textContent = old;
  if (!yes) return;
  await engine.dropDownload();
  toast('Removed.');
  refreshSetup();
}

function saveProvider() {
  const prov = $('provPick').value;
  providers.setProvider(prov);
  const key = $('provKey').value.trim();
  if (key) providers.setKey(prov, key);
  providers.setModel(prov, $('provModel').value);
  $('provKey').value = '';
  notice('provNotice',
    providers.getKey(prov)
      ? 'Saved. ' + providers.PROVIDERS[prov].label + ' is ready for daily notes.'
      : 'Provider saved, but there is no key yet - daily notes will ask for one.',
    providers.getKey(prov) ? 'ok' : '');
  refreshProviderFields();
}

// ── Boot ──

function boot() {
  live.wire();
  notes.wire();
  daily.wire();
  $('provPick').addEventListener('change', () => { refreshProviderFields(); });
  $('btnSaveProvider').onclick = saveProvider;
  $('btnGetModel').onclick = downloadModel;
  $('btnDropModel').onclick = dropModel;
  $('modelPick').addEventListener('change', () => engine.pickModel($('modelPick').value).then(refreshSetup));

  refreshDeviceChip();
  refreshNetChip();
  device.setClock(localNaiveEpochMs());

  // Offline shell: only exists in secure contexts. On the plain-http device
  // address (window.isSecureContext === false) we must NOT even request
  // sw.js — the device serves the shell as a single inlined file and a stray
  // fetch only adds load to the single-SD-stream server. The device already
  // serves everything offline over its own WiFi, so nothing is lost.
  if (window.isSecureContext && 'serviceWorker' in navigator) {
    navigator.serviceWorker.register('sw.js').catch(() => {});
  }
}

boot();
