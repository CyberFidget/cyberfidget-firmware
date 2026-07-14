// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC
//
// Best-effort screen keep-awake. The proper Screen Wake Lock API only
// exists on secure pages, and the companion is served over plain http from
// the device - so on most phones this degrades to honest UI copy ("keep
// the screen on"). The lock is still requested where available so the
// behavior upgrades for free if the page ever runs in a secure context.

let lock = null;
let wantHeld = false;

async function request() {
  if (!('wakeLock' in navigator)) return false;
  try {
    lock = await navigator.wakeLock.request('screen');
    lock.addEventListener('release', () => { lock = null; });
    return true;
  } catch {
    lock = null;
    return false;
  }
}

// Re-grab on tab return: the platform silently releases on visibility loss.
document.addEventListener('visibilitychange', () => {
  if (wantHeld && document.visibilityState === 'visible' && !lock) request();
});

// Returns true if a real lock is held; false means "copy-only" mode.
export async function hold() {
  wantHeld = true;
  return request();
}

export function release() {
  wantHeld = false;
  if (lock) {
    try { lock.release(); } catch { /* already gone */ }
    lock = null;
  }
}

export function supported() {
  return 'wakeLock' in navigator;
}
