# Cyber Fidget Phone Companion (SD pack source)

The companion is a static web app the device serves itself from the memory
card at `/web/`. It adds live listening, on-phone speech-to-text captions
(shown on the device's OLED), voice-note transcription with transcript
sidecars written back to the card, and bring-your-own-key daily-note
summaries.

## Why it lives on the SD card

- The web server already runs on the device, so the phone needs nothing
  installed - open the portal, tap "Live listening".
- A browser page on `https://cyberfidget.com` cannot open a plain `ws://`
  connection to a LAN device (mixed content), so the companion must be
  served same-origin by the device itself.
- The app plus its vendored speech library is multiple megabytes - far too
  big for the firmware image, trivial for the card.

## Build the pack

```
cd portal-companion
npm install          # once - pulls the vendored libraries
npm run build        # -> dist/web/
```

The build also writes the shell into the firmware source tree as
`lib/WebPortalApp/companion_shell_gz.h` (gzipped, tracked in git). That is why
**live listening needs nothing on the memory card** - the device serves the shell
out of its own flash. Copy `dist/web/` onto the card as `/web/` only when you
want captions and transcription, or upload the files through the portal's Files
tab.

### You usually don't need the card at all

The build is deliberately split so the common case needs nothing copied:

| File | Size on card | Needed for |
|---|---|---|
| *(none)* | 0 | **Live listening** - the shell is in firmware |
| `engine.worker.js.gz` | ~1 KB | captions / transcription (with `vendor/`) |
| `vendor/` | ~7.8 MB gzipped | the on-phone speech runtime (transformers.js + onnxruntime-web) |
| `index.html` | ~53 KB | optional - overrides the firmware's copy, so a newer pack works without reflashing |
| `pack-info.json` | ~120 B | build provenance (versions + timestamp) |

The on-demand files are stored **pre-compressed**. The device serves a
`<name>.gz` sibling with `Content-Encoding: gzip` when the plain name is absent,
and browsers decompress transparently - so the card holds 7.8 MB instead of 32 MB
and WiFi transfers shrink proportionally. A card written by an older, uncompressed
pack still works: the plain file is matched first.

### Keeping the embedded shell in sync

`companion_shell_gz.h` is generated but tracked, so that the firmware builds
without a JS toolchain. It can therefore go stale:

```bash
npm run build     # regenerates dist/web/ AND the header
npm run verify    # decompresses the header, compares to dist/web/index.html
```

In CI, `npm run build && git diff --exit-code ../lib/WebPortalApp/companion_shell_gz.h`
additionally catches "edited src, forgot to regenerate".

### The shared kit - one navigation across two documents

The device serves **two** documents: the portal at `/` (a PROGMEM literal in
`lib/WebPortalApp/portal_page.h`, hand-authored so the firmware builds with no
JS toolchain) and this companion at `/web/`. The user is meant to experience one
app spanning both - five destinations, same order, same treatment - so the
navigation, the audio player, the gate panel and the bulk-download flow have a
single source in `src/shared/` and are **generated into both**:

```bash
npm run chrome:sync    # src/shared/ -> kit.gen.{css,js} + portal_page.h markers
npm run verify:chrome  # re-derives and fails if any copy has drifted
```

`npm run build` runs the sync first, and `npm run verify` runs all three checks.
Inside `portal_page.h` the generated blocks sit between `/* CF-KIT-CSS:BEGIN */`
and `/* CF-KIT-JS:BEGIN */` marker pairs; everything outside them is still the
editable source, so `scripts/gzip_portal_page.py` is unaffected.

**Edit `src/shared/`, never the generated copies.** An edit to the portal's copy
of the tab bar looks completely fine in the portal - the seam only reopens for a
user crossing between the two documents, which is the one thing nobody does
while working on a single surface. That is what `verify:chrome` is for.

The kit declares its own `--k-*` colour literals rather than either document's
token names (it has to be byte-identical in both). `verify:tokens` checks all
three spellings agree, so the anti-drift mechanism cannot itself drift.

### Firmware requirement

The device must be running firmware that serves `.mjs` as `text/javascript`
and `.wasm` as `application/wasm` (`webContentType()` in
`lib/WebPortalApp/WebPortalApp.cpp`). Older firmware served both as
`text/plain`, which browsers refuse to execute as modules - transcription
fails with a MIME-type error even though the pack is correctly copied. That
is the device's serving, not the pack, so recopying won't fix it; reflash.

## Layout

```
src/                  app source (plain ES modules - no bundler)
  index.html          shell + views (Listen / Notes / Daily / Settings)
  shared/             THE SHARED KIT - generated into this document AND the portal
    chrome.css        shell, tab bar, sidebar, seg control, gate, player, overlay
    chrome.js         the five destinations, nav render, player, bulk download
    icons.mjs         the five 9x9 nav bitmaps -> precomputed box-shadow rules
  css/tokens.css      Cyber Fidget design tokens (single visual source of truth)
  css/app.css         components built on the tokens
  js/live.js          live session: socket client, waveform, captions
  js/engine.js        on-phone speech-to-text (vendored transformers.js)
  js/notes.js         Notes: recordings + transcripts merged, one row each
  js/daily.js         daily-note summaries (BYOK, direct-to-provider)
  js/providers.js     Anthropic / OpenAI / Google direct calls + key custody
  js/device.js        device endpoint client
  js/db.js            IndexedDB (transcripts, model cache, settings)
  js/ui.js            toast/overlay/notice/ask helpers
  js/keepawake.js     screen keep-awake (degrades honestly on http)
  sw.js               offline-shell worker (secure contexts only)
build_sdpack.mjs      assembles dist/web/ and vendors the libraries
sync_chrome.mjs       writes the shared kit into both device documents
verify_chrome.mjs     fails if either copy of the kit has drifted
verify_tokens.mjs     fails if portal / companion / kit colours disagree
verify_shell_header.mjs  fails if the embedded shell is stale
```

## Design constraints worth knowing

- **Plain-http reality.** The device serves over `http://`, so secure-context
  APIs (service worker, Cache API, storage persistence, screen wake lock,
  install-to-home-screen) are unavailable on phones. Offline still works -
  the device itself serves every file - and the speech model is cached in
  IndexedDB (available on http). Code paths feature-detect and upgrade
  automatically in a secure context.
- **Custody invariant.** The page makes zero off-device requests except two
  explicit, user-consented flows: the one-time model download and the
  daily-note provider call (user's own key, direct to the provider; never
  through cyberfidget.com). Don't add CDN assets, web fonts, or analytics.
- **Link contract.** The live socket framing is pinned in the firmware's
  `lib/WebPortalApp/LiveLinkProtocol.h` - change both ends together.
- **Tokens-first styling.** Components consume only `css/tokens.css`
  custom properties so the look can be swapped/rolled back in one file.

## Licensing

App source: GPL-3.0-or-later WITH Cyberfidget-HAL-exception (repo license).
Vendored at build time (not in git): transformers.js (Apache-2.0),
onnxruntime-web (MIT) - their license files are copied into `dist/web/vendor/`.
Speech models are downloaded by the user at run time from Hugging Face under
their own licenses.
