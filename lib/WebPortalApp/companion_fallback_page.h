// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

// lib/WebPortalApp/companion_fallback_page.h
//
// Served for any /web/* request when the companion pack is not on the
// memory card (no card, or /web/index.html missing). The real companion
// lives on the card (portal-companion/ -> SD pack) so its megabytes stay
// out of the firmware image; this page only explains how to get it.

#pragma once

const char COMPANION_FALLBACK_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1.0">
<title>Cyber Fidget - Companion</title>
<style>
:root{--bg:#0e0820;--card:#15102a;--ink:#f5ecff;--dim:#a89cc8;--cyan:#68e1fd;--magenta:#ff2bb8;--border:rgba(104,225,253,0.2)}
body{margin:0;min-height:100vh;display:flex;align-items:center;justify-content:center;background:var(--bg);color:var(--ink);font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;padding:20px;box-sizing:border-box}
.card{max-width:430px;background:var(--card);border:1px solid var(--border);border-radius:12px;padding:28px 24px;box-shadow:0 0 32px rgba(104,225,253,0.08)}
h1{font-size:1.2em;margin:0 0 4px;color:var(--cyan);letter-spacing:1px}
.sub{color:var(--magenta);font-size:0.8em;letter-spacing:2px;text-transform:uppercase;margin-bottom:16px}
p{line-height:1.55;color:var(--dim);font-size:0.95em}
ol{color:var(--dim);line-height:1.7;padding-left:20px;font-size:0.95em}
b{color:var(--ink)}
code{background:rgba(104,225,253,0.1);color:var(--cyan);padding:1px 6px;border-radius:4px;font-size:0.9em}
a{color:var(--cyan)}
.back{display:inline-block;margin-top:18px;color:var(--cyan);text-decoration:none;border:1px solid var(--border);padding:8px 16px;border-radius:8px}
</style>
</head>
<body>
<div class="card">
  <h1>PHONE COMPANION</h1>
  <div class="sub">not on the memory card yet</div>
  <p>The companion is a small app for your phone that your Cyber Fidget
  serves itself - it adds live listening, captions, and note transcription.
  It lives on the memory card so this page stays tiny.</p>
  <ol>
    <li>Get the free companion pack - see the guide at
        <b>docs.cyberfidget.com</b> (look for <b>Phone companion</b>).</li>
    <li>Copy the pack's <code>web</code> folder onto the memory card,
        so the card has <code>/web/index.html</code>.</li>
    <li>Put the card back in and reopen this page.</li>
  </ol>
  <a class="back" href="/">&#8592; Back to the portal</a>
</div>
</body>
</html>
)rawliteral";
