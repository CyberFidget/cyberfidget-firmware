// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

// lib/WebPortalApp/companion_fallback_page.h
//
// Served for any /web/* request when the companion pack is not on the
// memory card (no card, or /web/index.html missing). The real companion
// lives on the card (portal-companion/ -> SD pack) so its megabytes stay
// out of the firmware image; this page only explains how to get it.
//
// Visually this is the phase-B rail panel (see the "NO DEVICE ·
// VISITOR (TEASER)" state on the Device rail artboard): chamfered card,
// bordered cyan mono eyebrow, display-face heading, mono body. Tokens
// mirror the website's cf-kit with system font tails - the device serves
// this over one SPI stream, so no webfont or stylesheet fetch.

#pragma once

const char COMPANION_FALLBACK_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1.0">
<title>Cyber Fidget - Companion</title>
<style>
:root{--deep:#08050f;--card:#15102a;--sunk:rgba(0,0,0,0.35);--ink:#f5ecff;--dim:#a89cc8;--mute:#6b5e88;--cyan:#68e1fd;--magenta:#ff2bb8;--border:rgba(245,236,255,0.08);--border-cy:rgba(104,225,253,0.35);--f-d:'Chakra Petch',system-ui,sans-serif;--f-m:ui-monospace,Consolas,monospace}
body{margin:0;min-height:100vh;display:flex;align-items:center;justify-content:center;background:var(--deep);color:var(--ink);font-family:var(--f-d);padding:20px;box-sizing:border-box}
.card{max-width:440px;background:var(--card);border:1px solid var(--border);clip-path:polygon(0 0,calc(100% - 14px) 0,100% 14px,100% 100%,0 100%);padding:24px}
.eyebrow{display:inline-block;font-family:var(--f-m);font-size:0.66em;font-weight:700;letter-spacing:0.2em;text-transform:uppercase;color:var(--cyan);border:1px solid var(--border-cy);padding:3px 8px}
h1{font-size:1.3em;font-weight:700;margin:14px 0 12px;line-height:1.25}
p,ol{font-family:var(--f-m);line-height:1.65;color:var(--dim);font-size:0.8em}
ol{padding-left:22px;margin:12px 0 0}
li{margin-bottom:7px}
b{color:var(--ink)}
code{background:rgba(104,225,253,0.1);color:var(--cyan);padding:1px 5px}
.note{font-family:var(--f-m);line-height:1.6;border:1px solid rgba(255,216,74,0.4);background:rgba(255,216,74,0.06);color:#ffd84a;padding:8px 10px;margin:14px 0 0;font-size:0.74em}
.back{display:inline-block;margin-top:18px;font-family:var(--f-d);font-size:0.8em;font-weight:700;letter-spacing:0.06em;text-transform:uppercase;color:var(--cyan);text-decoration:none;border:1px solid var(--border-cy);padding:8px 14px}
.back:hover{background:var(--cyan);color:var(--deep)}
</style>
</head>
<body>
<div class="card">
  <div class="eyebrow">Phone companion</div>
  <h1>Not on the memory card yet</h1>
  <p>The companion is a small app for your phone that your Cyber Fidget
  serves itself - it adds live listening, captions, and note transcription.
  It lives on the memory card so this page stays tiny.</p>
  <ol>
    <li>Get the free companion pack - see the guide at
        <b>docs.cyberfidget.com</b> (look for <b>Phone companion</b>).</li>
    <li>Copy the pack's <code>index.html</code> onto the memory card, so the
        card has <code>/web/index.html</code>.</li>
    <li>Put the card back in and reopen this page.</li>
  </ol>
  <div class="note">One file, about 50 KB, is all live listening needs. The
  rest of the pack only adds captions and note transcription - copy it later
  if you want those.</div>
  <a class="back" href="/">&lt; Back to the portal</a>
</div>
</body>
</html>
)rawliteral";
