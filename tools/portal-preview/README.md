# portal-preview

Renders the device's web-portal pages in a desktop browser with no device
attached, so portal UI changes can be reviewed and screenshotted.

The portal ships as a PROGMEM raw-string literal inside
`lib/WebPortalApp/portal_page.h` (and `companion_fallback_page.h`). This tool
extracts that literal, serves it, and answers the portal's `/api/*` calls with
fixtures so the page renders populated instead of sitting on "Loading...".

## What it does not do

Fixtures only. It is a **rendering harness, not a device simulator**: writes
(upload, delete, move, rename, WiFi connect) return `{"ok":true}` without doing
anything, and media streams are not served, so audio playback and the seek bar
stay inert. Anything behavioural still needs the real device or the HIL bench.

## Run

```bash
node tools/portal-preview/serve.mjs
#   portal:   http://localhost:8099/
#   fallback: http://localhost:8099/web/
```

`PORT=9000 node tools/portal-preview/serve.mjs` to move it off 8099.

No dependencies - plain `node:http`, Node 18+.

## Fixtures

Editing the `FIXTURES` object in `serve.mjs` changes what the page shows. Field
names mirror what the portal's own JS reads, so if a fixture stops rendering,
check it against the matching `fetch('/api/...')` handler in `portal_page.h`.
