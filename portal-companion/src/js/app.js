// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC
//
// Companion bootstrap: navigation, connection facts, the Settings segments, and
// the (secure-context-only) offline shell registration. The feature views live
// in live.js / notes.js / daily.js.
//
// This document renders two of the product's five destinations (Notes and
// Listen) plus two of Settings' three segments. The other three destinations
// render in the portal document at /. The navigation itself is the shared kit's
// (see src/shared/), identical on both sides, so the crossing is invisible.

import { $, toast, notice, askOverlay, fmtBytes, localNaiveEpochMs } from './ui.js';
import * as live from './live.js';
import * as notes from './notes.js';
import * as daily from './daily.js';
import * as engine from './engine.js';
import * as providers from './providers.js';
import * as device from './device.js';

// ── Routing ──
//
// Routes are `#<destination>` or `#<destination>/<sub>`. Two of them nest:
// Daily note under Notes (it is made from those transcripts), and the
// Transcription / Your data segments under Settings. The tab bar highlights the
// destination, not the sub-route.
const ROUTES = {
  listen:   { view: 'Listen',   dest: 'listen',   title: 'Live listening' },
  notes:    { view: 'Notes',    dest: 'notes',    title: 'Notes' },
  settings: { view: 'Settings', dest: 'settings', title: 'Settings' },
};
const SUB = {
  'notes/daily':            { view: 'Daily', title: 'Daily note' },
  'settings/transcription': { seg: 'Transcription' },
  'settings/data':          { seg: 'Your data' },
};
const VIEWS = ['Listen', 'Notes', 'Daily', 'Settings'];

// An earlier release used capitalised single-word hashes and the portal linked
// to them. Those links are in the wild - and in people's history - so they
// still resolve, onto whichever destination now owns what they used to open.
const LEGACY = {
  listen: 'listen', notes: 'notes', daily: 'notes/daily',
  setup: 'settings/transcription', transcripts: 'notes',
};

let route = 'listen';

function parseHash() {
  const raw = decodeURIComponent((location.hash || '').replace('#', '')).toLowerCase();
  if (!raw) return 'listen';
  if (LEGACY[raw]) return LEGACY[raw];
  if (SUB[raw]) return raw;
  return ROUTES[raw.split('/')[0]] ? raw.split('/')[0] : 'listen';
}

function go(target, push) {
  route = target;
  const base = target.split('/')[0];
  const r = ROUTES[base];
  const sub = SUB[target];
  const view = (sub && sub.view) || r.view;

  for (const v of VIEWS) $('view' + v).hidden = (v !== view);
  $('pageTitle').textContent = (sub && sub.title) || r.title;
  $('pageStatus').textContent = '';
  CFK.setActive(r.dest);
  if (push !== false && location.hash.replace('#', '') !== target) {
    // replaceState, not a hash assignment, so tapping tabs does not pile up
    // history entries the back button then has to walk through.
    history.replaceState(null, '', '#' + target);
  }

  if (view === 'Notes') notes.loadNotes();
  if (view === 'Daily') daily.refresh();
  if (view === 'Settings') showSegment(sub && sub.seg ? sub.seg : 'Transcription');
}

// Called by the kit when the user picks a destination this document renders.
function onDestination(id) {
  go(id === 'settings' ? 'settings/transcription' : id);
}

export function navigate(target) { go(target); }

// ── Settings segments ──
// Network lives in the portal document and is a plain link in the markup; these
// two render here. Landing on Settings from the tab bar lands on Transcription,
// because Network is one document away and the deep links that matter (both
// no-pack gates) target Transcription directly.
function showSegment(name) {
  const isData = name === 'Your data';
  $('segPaneTranscription').hidden = isData;
  $('segPaneData').hidden = !isData;
  $('segTranscription').classList.toggle('on', !isData);
  $('segData').classList.toggle('on', isData);
  if (!isData) refreshSetup();
  else refreshAbout();
}

// ── The transcription pack ──
// Live listening works with nothing on the memory card; turning speech into text
// needs the pack. The device reports whether it is there - it cannot be probed
// over HTTP, because a missing path under /web/ is an ordinary 404. Views ask
// hasPack() to decide between showing an affordance and showing a gate.
let packPresent = true;

// What the kit shows in the sidebar footer and the masthead chips. Two endpoints
// feed it - wifi/status for the networks, status for the firmware version - so it
// accumulates here and is re-applied, rather than each caller clearing the
// other's half (which is how the address briefly became the browser's own host).
const connState = {};
function applyConn(patch) {
  Object.assign(connState, patch);
  CFK.conn(connState);
}

export function hasPack() { return packPresent; }

// One gate treatment, wherever something needs a part that is not installed.
// Yellow is notice, never error: the device is working exactly as shipped.
export function renderGate(containerId, lead, body, show = !packPresent,
                           action = 'Set it up >', href = '#settings/transcription') {
  const host = $(containerId);
  if (!host) return;
  host.innerHTML = '';
  if (!show) return;
  host.appendChild(CFK.gate({
    lead,
    body,
    action,
    href,
  }));
}

async function refreshStatus() {
  try {
    const st = await device.getStatus();
    packPresent = st.captions !== false;
    $('aboutFirmware').textContent = st.version || '-';
    const cardOlder = st.shell?.cardOlder === true;
    $('aboutCardShellRow').hidden = !cardOlder;
    $('aboutCardShell').textContent = st.shell?.cardStamp || '-';
    renderGate(
      'shellGate',
      'Your Fidget is showing its own built-in companion.',
      "The companion files on your memory card are older than the ones built into your Fidget, so it is showing the newer built-in copy. To use the card's copy instead, put a newer set of files on it.",
      cardOlder,
      'See versions >',
      '#settings/data',
    );
    applyConn({ version: st.version });
  } catch {
    // Unreachable device is the connection chips' story, not a gate's: keep the
    // affordances and let the actual call fail with something specific.
  }
}

async function refreshConnection() {
  try {
    const st = await device.getWifiStatus();
    applyConn({
      apIp: st.ap_ip || location.host,
      ssid: st.connected ? (st.ssid || 'home WiFi') : '',
      staIp: st.ip,
    });
    $('aboutDevice').textContent = location.host +
      (st.connected ? ' + ' + (st.ssid || 'WiFi') : ' (its own network)');
  } catch {
    applyConn({ apIp: location.host, ssid: '' });
    $('aboutDevice').textContent = 'not reachable';
  }
}

// ── Settings: Transcription segment ──

async function refreshSetup() {
  function populate(pick, slot) {
    if (pick.options.length !== 0) return;
    for (const m of engine.listModels().filter((model) => model[slot])) {
      const opt = document.createElement('option');
      opt.value = m.id;
      opt.textContent = m.label;
      pick.appendChild(opt);
    }
  }
  const livePick = $('modelPickLive');
  const notesPick = $('modelPickNotes');
  populate(livePick, 'live');
  populate(notesPick, 'notes');
  livePick.value = await engine.pickedModel('live');
  notesPick.value = await engine.pickedModel('notes');
  const liveReady = await engine.isDownloaded(livePick.value);
  const notesReady = await engine.isDownloaded(notesPick.value);
  const bytes = await engine.downloadedBytes();
  const storage = bytes ? ' (' + fmtBytes(bytes) + ' in your browser)' : '';
  $('engineStatus').textContent =
    'Live captions: ' + (liveReady ? 'ready' : 'not downloaded') + '. ' +
    'Saved notes: ' + (notesReady ? 'ready' : 'not downloaded') + '.' + storage;
  $('btnGetModel').textContent = liveReady && notesReady ? 'Re-download' : 'Download';
  $('btnDropModel').hidden = !liveReady && !notesReady;

  const prov = providers.getProvider();
  $('provPick').value = prov;
  refreshProviderFields();
}

function refreshAbout() {
  $('aboutVersion').textContent =
    document.querySelector('meta[name="cf-shell"]')?.content || '-';
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
  const selections = [
    { slot: 'live', name: 'live captions', id: $('modelPickLive').value },
    { slot: 'notes', name: 'saved notes', id: $('modelPickNotes').value },
  ];
  await Promise.all(selections.map((x) => engine.pickModel(x.slot, x.id)));
  const readiness = await Promise.all(selections.map((x) => engine.isDownloaded(x.id)));
  let pending = selections.filter((x, i) => !readiness[i]);
  if (pending.length === 0) pending = selections;
  // The same selected model only needs one pipeline build and one download.
  const downloads = pending.filter((x, i) => pending.findIndex((y) => y.id === x.id) === i);

  $('modelOverlayText').textContent =
    pending.map((x) => {
      const m = engine.listModels().find((model) => model.id === x.id);
      return 'The ' + x.name + ' pack is about ' + m.sizeMB + ' MB.';
    }).join(' ') + ' Downloads once and is best on WiFi.';
  const conn = navigator.connection;
  $('modelCellWarn').hidden = !(conn && (conn.type === 'cellular' ||
    /2g|3g/.test(conn.effectiveType || '')));
  const yes = await askOverlay('modelOverlay', 'btnModelGo', 'btnModelNo');
  if (!yes) return;

  $('btnGetModel').disabled = true;
  $('modelBar').hidden = false;
  notice('engineNotice', '');
  try {
    for (const item of downloads) {
      await engine.load(item.id, (p) => {
        if (p.phase === 'preparing') {
          $('modelFill').style.width = '100%';
          $('engineStatus').textContent = 'Preparing the ' + item.name + ' pack (this can take a moment)...';
        } else if (p.phase === 'warming') {
          $('engineStatus').textContent = 'Warming up the ' + item.name + ' pack...';
        } else {
          $('modelFill').style.width = p.pct + '%';
          $('engineStatus').textContent = 'Downloading the ' + item.name + ' pack... ' + p.pct + '%';
        }
      });
    }
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

async function boot() {
  // The kit builds the navigation first, so the tab bar and sidebar are up
  // whether or not the device answers anything below.
  CFK.nav('companion', ROUTES[parseHash().split('/')[0]].dest, onDestination);
  CFK.onToast(toast);

  await refreshConnection();
  // Whether the pack is present decides what several views render, so settle it
  // before anything wires up or paints - otherwise affordances appear and then
  // flash away, which reads as a fault rather than a state.
  await refreshStatus();

  live.wire();
  notes.wire();
  daily.wire();
  $('segTranscription').onclick = () => go('settings/transcription');
  $('segData').onclick = () => go('settings/data');
  $('dailyOpen').onclick = (ev) => { ev.preventDefault(); go('notes/daily'); };
  $('btnBackToNotes').onclick = () => go('notes');
  $('provPick').addEventListener('change', () => { refreshProviderFields(); });
  $('btnSaveProvider').onclick = saveProvider;
  $('btnGetModel').onclick = downloadModel;
  $('btnDropModel').onclick = dropModel;
  $('modelPickLive').addEventListener('change', () =>
    engine.pickModel('live', $('modelPickLive').value).then(refreshSetup));
  $('modelPickNotes').addEventListener('change', () =>
    engine.pickModel('notes', $('modelPickNotes').value).then(refreshSetup));

  window.addEventListener('hashchange', () => go(parseHash(), false));
  go(parseHash(), false);

  device.setClock(localNaiveEpochMs());

  // Offline shell: only exists in secure contexts. On the plain-http device
  // address (window.isSecureContext === false) we must NOT even request
  // sw.js - the device serves the shell as a single inlined file and a stray
  // fetch only adds load to the single-SD-stream server. The device already
  // serves everything offline over its own WiFi, so nothing is lost.
  if (window.isSecureContext && 'serviceWorker' in navigator) {
    navigator.serviceWorker.register('sw.js').catch(() => {});
  }
}

boot();
