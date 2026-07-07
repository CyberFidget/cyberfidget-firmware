// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC
//
// Tiny shared UI helpers: toast, overlay open/close, notice rendering.

export const $ = (id) => document.getElementById(id);

let toastTimer = null;
export function toast(msg, ms = 2400) {
  const t = $('toast');
  t.textContent = msg;
  t.classList.add('show');
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => t.classList.remove('show'), ms);
}

// Show an overlay and resolve true/false from its confirm/cancel buttons.
// Buttons are re-bound each call so stale handlers never stack.
export function askOverlay(overlayId, okId, cancelId) {
  return new Promise((resolve) => {
    const overlay = $(overlayId);
    const ok = $(okId);
    const cancel = $(cancelId);
    const done = (answer) => {
      overlay.hidden = true;
      ok.onclick = null;
      cancel.onclick = null;
      resolve(answer);
    };
    ok.onclick = () => done(true);
    cancel.onclick = () => done(false);
    overlay.hidden = false;
  });
}

// Notices: small inline status panels. kind: '' | 'err' | 'ok'
export function notice(id, text, kind = '') {
  const el = $(id);
  if (!text) { el.hidden = true; return; }
  el.textContent = '';
  if (typeof text === 'string') el.textContent = text;
  else el.appendChild(text);   // allow a prebuilt fragment (e.g. with a link)
  el.className = 'notice' + (kind ? ' ' + kind : '');
  el.hidden = false;
}

// Build "message + provider console link" content for key problems, so the
// fix is always one tap away.
export function noticeWithLink(msg, linkText, href) {
  const frag = document.createDocumentFragment();
  frag.append(msg + ' ');
  const a = document.createElement('a');
  a.href = href;
  a.target = '_blank';
  a.rel = 'noopener';
  a.textContent = linkText;
  frag.appendChild(a);
  return frag;
}

export function fmtBytes(b) {
  if (b < 1024) return b + ' B';
  if (b < 1048576) return (b / 1024).toFixed(1) + ' KB';
  return (b / 1048576).toFixed(1) + ' MB';
}

// The device stores local wall-clock time as a timezone-naive epoch; this
// mirrors the portal page's adjustment so both set the clock identically.
export function localNaiveEpochMs() {
  return Date.now() - new Date().getTimezoneOffset() * 60000;
}

export function todayISO() {
  const d = new Date();
  return `${d.getFullYear()}-${String(d.getMonth() + 1).padStart(2, '0')}-${String(d.getDate()).padStart(2, '0')}`;
}
