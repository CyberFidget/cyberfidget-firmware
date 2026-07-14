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

Copy `dist/web/` onto the memory card as `/web/` (so the card has
`/web/index.html`), or upload the folder's files through the portal's Files
tab. Without the pack, `/web/` serves a small built-in page explaining how
to get it.

## Layout

```
src/                  app source (plain ES modules - no bundler)
  index.html          shell + views (Listen / Notes / Daily / Setup)
  css/tokens.css      Cyber Fidget design tokens (single visual source of truth)
  css/app.css         components built on the tokens
  js/live.js          live session: socket client, waveform, captions
  js/engine.js        on-phone speech-to-text (vendored transformers.js)
  js/notes.js         stored-note transcription + sidecars
  js/daily.js         daily-note summaries (BYOK, direct-to-provider)
  js/providers.js     Anthropic / OpenAI / Google direct calls + key custody
  js/device.js        device endpoint client
  js/db.js            IndexedDB (transcripts, model cache, settings)
  js/ui.js            toast/overlay/notice helpers
  js/keepawake.js     screen keep-awake (degrades honestly on http)
  sw.js               offline-shell worker (secure contexts only)
build_sdpack.mjs      assembles dist/web/ and vendors the libraries
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
