// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC
//
// Offline-shell worker. NOTE: service workers require a secure context, so
// this never runs on the plain-http device address (http://192.168.4.1) -
// there, offline-after-first-visit holds anyway because the device itself
// serves every file over its own WiFi. This worker exists so the shell
// also self-caches if the companion is ever hosted in a secure context.

const CACHE = 'cf-companion-shell-v1';
const SHELL = [
  './',
  'index.html',
  'css/tokens.css',
  'css/app.css',
  'js/app.js',
  'js/ui.js',
  'js/live.js',
  'js/notes.js',
  'js/daily.js',
  'js/engine.js',
  'js/providers.js',
  'js/device.js',
  'js/db.js',
  'js/keepawake.js',
  'manifest.webmanifest',
  'icons/icon.svg',
];

self.addEventListener('install', (e) => {
  e.waitUntil(caches.open(CACHE).then((c) => c.addAll(SHELL)));
  self.skipWaiting();
});

self.addEventListener('activate', (e) => {
  e.waitUntil(
    caches.keys().then((keys) =>
      Promise.all(keys.filter((k) => k !== CACHE).map((k) => caches.delete(k))))
  );
  self.clients.claim();
});

self.addEventListener('fetch', (e) => {
  const url = new URL(e.request.url);
  // Only the shell: device data (/api/, /recordings/) and the live link
  // must always hit the device.
  if (url.origin !== self.location.origin) return;
  if (url.pathname.includes('/api/') || url.pathname.includes('/recordings/')) return;
  e.respondWith(
    caches.match(e.request).then((hit) => hit || fetch(e.request))
  );
});
