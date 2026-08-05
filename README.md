# Cyber Fidget Firmware

Open-source firmware for the [Cyber Fidget](https://cyberfidget.com) — a handheld ESP32-based gadget with a 128x64 OLED display, clicky  buttons, slider, addressable LEDs, accelerometer, microphone, speaker, uSD card reader, and WiFi/Bluetooth.

## Features

- **20+ built-in apps** — games (Dino, Breakout, Simon Says, Stratagem Hero, Spaceship), screensavers (Matrix, Graveyard, Eye, Ghosts), tools (Clock, Flashlight, Spectrum Analyzer), and more
- **Music player** — Bluetooth A2DP streaming with AVRCP controls and MP3 playback
- **Web portal** — WiFi-based configuration and control interface
- **App SDK** — Build your own apps using the HAL API (see below)
- **WASM emulator** — Run firmware apps in the browser for development and testing

## Building

### Prerequisites

- [PlatformIO](https://platformio.org/) (CLI or VS Code extension)
- Cyber Fidget Mainboard

### Flash

```bash
# Build and flash over USB
pio run -e local -t upload

# Serial monitor
pio run -e local -t monitor
```

### WASM Emulator

Build the browser-based emulator using [Emscripten](https://emscripten.org/):

```bash
cd wasm
./build_wasm.sh                          # Default demo
./build_wasm.sh MyApp.h MyApp.cpp        # Custom app
```

Output: `wasm/build/cyberfidget.js` + `cyberfidget.wasm`

### Phone companion (SD pack)

The device serves a phone web app from the memory card at `/web/`. It is **not**
part of the firmware image and is **not** built by `pio run` — it is built
separately and copied to the card. If the portal shows "not on the memory card
yet", this is what's missing:

**You may not need to build it at all** — every tagged release attaches
`companion-pack.zip` (the full pack) and `companion-index.html` (the shell
alone) as downloads. Build from source only when you're changing the companion.

Three equivalent ways to build it:

```bash
# 1. Directly
cd portal-companion
npm install     # once — fetches the vendored speech libraries
npm run build   # -> dist/web/

# 2. Through PlatformIO (installs deps on first run)
pio run -t sdpack

# 3. VS Code: Terminal -> Run Task -> "Companion: Build SD pack"
#    .vscode/ is gitignored here, so add this task yourself if you want it:
#    { "label": "Companion: Build SD pack", "type": "shell",
#      "command": "npm run build",
#      "options": { "cwd": "${workspaceFolder}/portal-companion" } }
```

`pio run -t sdpack` is a custom target (`scripts/build_companion.py`); it is
registered on every build but only *runs* when you ask for it by name, so
ordinary `pio run` / `-t upload` cycles are unaffected and still work on
machines without Node.

Copy `dist/web/` to the card as `/web/` (so the card has `/web/index.html`).
**Live listening needs only the ~50 KB self-contained `index.html`**; the ~32 MB
`vendor/` tree is required only for captions and note transcription. `dist/` is
gitignored, so the pack is rebuilt rather than committed.

See [portal-companion/README.md](portal-companion/README.md) for the design
constraints (single-request page load, custody rules, the live-link protocol
contract).

## Writing Apps

Apps interact with the hardware through the **HAL API** — a set of abstraction headers that decouple app logic from the underlying ESP32 drivers:

| Header | Purpose |
|---|---|
| `HAL.h` | Hardware initialization, accelerometer globals |
| `DisplayProxy.h` | 128x64 OLED drawing (lines, rects, text, bitmaps) |
| `ButtonManager.h` | Button event callbacks |
| `AudioManager.h` | Audio playback control |
| `RGBController.h` | NeoPixel LED control |
| `globals.h` | Shared state (slider position, battery, etc.) |

Apps follow the `begin()` / `update()` / `end()` lifecycle and register via the `APP_ENTRY` macro in `AppManifest.h`. See any app in `lib/` for examples.

**Apps you create through the HAL API are yours** — the linking exception in the license means they are not considered derivative works of the firmware, regardless of how they are compiled or linked.

## Manual test checklist

Automated tests can't drive real hardware. Whenever a change touches LEDs, the OLED display, or audio, verify on-device before considering it done:

1. **Build/flash version match.** After flashing, compare the boot banner (and the `version` CLI command at 921600 baud) against the build summary printed to console — `fw=X.Y.Z+hash type=... built=...` must match character-for-character. If it doesn't, something's stale (wrong binary, wrong port, partial flash) — don't proceed until it does.
2. **LEDs.** Exercise every LED-driving path the change touches (idle, active states, transitions):
   - Correct physical LED per the pixel index map (`0` Back, `1` Front Top, `2` Front Middle, `3` Front Bottom) — no cross-wiring.
   - LEDs go fully dark on `begin()` (via `setColorsOff()`) and again on `end()` — no bleed from the previous app, none carried into the next.
   - Nothing lit during states that shouldn't show activity (idle/menu).
3. **Display.** No leftover pixels/tearing from the previous app on entry; screen clears appropriately on exit.
4. **Audio.** `stopTone()` (or equivalent) fires on `end()` — no audio bleeding into the menu or the next app.
5. **Sleep/power-cycle.** If the app's idle state drives LEDs, confirm they also go dark on deep-sleep entry, not just on app exit.

## Project Structure

```
src/              Main entry point
lib/              App and library modules
  AppDefs/        App manifest and registration
  HAL/            Hardware abstraction layer
  DisplayProxy/   OLED display interface
  ButtonManager/  Button input handling
  DinoGame/       Example app (and 20+ others)
include/          Board configuration, credentials
wasm/             WASM emulator build infrastructure
  hal/            WASM HAL implementation
  shims/          ESP32/Arduino API shims for browser
portal-companion/ Phone companion web app (built separately -> memory card /web/)
scripts/          Build utilities
```

## License

GPL-3.0-or-later with a **HAL Linking Exception** — apps built through the published HAL API may be licensed under terms of your choice.

See [LICENSE](LICENSE) for the full text, [PERMISSIONS.md](PERMISSIONS.md) for a plain-language summary, and [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md) for dependency attribution.

## Links

- [cyberfidget.com](https://cyberfidget.com) — product site and online emulator
- [Documentation](https://docs.cyberfidget.com) — hardware specs, guides, API reference
- [Cyber Fidget Docs repo](https://github.com/CyberFidget/cyberfidget-docs) — documentation source

---

Copyright (c) 2023-2026 Dismo Industries LLC
