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

The device serves a phone web app at `/web/`. The **shell** ships inside the
firmware image, gzipped (`lib/WebPortalApp/companion_shell_gz.h`, ~18 KB), so
live listening works with an empty or absent memory card and needs nothing
copied anywhere. Only **captions and transcription** need the memory card: the
on-phone speech runtime is ~7.8 MB gzipped and is built separately.

`companion_shell_gz.h` is a **generated file that is tracked in git**, so the
firmware still builds with no JS toolchain installed. Regenerate it with
`npm run build` in `portal-companion/` after changing anything under
`portal-companion/src/`, and check it with `npm run verify` (or in CI:
`npm run build && git diff --exit-code ../lib/WebPortalApp/companion_shell_gz.h`).

A copy of `index.html` on the card still wins over the embedded one, so a newer
pack can be dropped in without reflashing.

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

**Two C++ features are off in device builds** to keep the firmware image small: exceptions (`try`/`catch`/`throw` do not compile) and the `<iostream>` streams (`std::cout`, `std::cin`). Log with `ESP_LOGx(...)` or `Serial.printf(...)` instead. Avoid `<sstream>` too: it compiles, but pulls roughly 200 KB of stream and locale code back into the image. The emulator does not enforce these limits, so an app that runs there can still fail the device build.

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

## Developer gotchas

Things that have cost real bench time. None of them is a bug to fix; they are how the hardware and tools behave.

- **Flash dev and bench units with `pio run -e <env> -t upload`.** Every build also writes `merged_firmware.bin` (bootloader, partition table and app in one file, starting at `0x1000`). The gaps in that file are padding, so writing it covers the settings area (NVS at `0x9000`) and erases saved WiFi, the account link and every other stored setting. Use the merged image only for a deliberate factory-fresh flash.
- **A Fidget that seems frozen may be asleep.** After 60 s without a button press (`TASK_LASTINTERACT`) it goes into deep sleep: the screen is off and the USB serial port stops answering. Press a button to wake it. Settings > Awake & dev mode > Stay awake keeps it up while you work.
- **Opening the USB serial port resets the board.** Wait for it to start, then send `version` until it answers before sending anything else. A command sent into the boot is lost.
- **Any WiFi use splits internal memory for the rest of that power cycle.** After a check-in, the portal or dev mode listening, internal RAM no longer has large free blocks, even with WiFi off. Delivered (WASM) apps cope because the interpreter's stack lives in PSRAM (only a ~4 KB internal task is needed); if even that does not fit, the app restarts straight into itself ("Opening <app>..."). Bench any change that touches memory or app launch by opening a delivered app after a check-in, not only from a cold start.
- **Nothing on the delivered-app task may touch the flash driver.** Its stack is in PSRAM, and the SDK aborts any SPI flash call (reads included) made from a PSRAM stack. New host functions for apps that need files or settings must hand that work to the loop task.
- **Native `pio test` envs on Windows need the MSYS2 ucrt64 compiler first on `PATH`.** From Git Bash, the `/mingw64` DLLs on its default `PATH` collide with the ucrt64 toolchain and the compiler dies with no useful message. Run `PATH="/c/msys64/ucrt64/bin:$PATH" pio test -e <env>`.
- **`pio run -v` fails at the image step on Windows.** Use plain `pio run`.
- **Rebuild the emulator after changing a header it includes.** The WASM build compiles against this repo's headers in `lib/` (the HAL, `lib/Globals` and the built-in apps, see `wasm/CMakeLists.txt`); a changed struct, constant or signature there leaves an old emulator build out of step with the firmware until you run `wasm/build_wasm.sh` (or `build_wasm.bat`) again.

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
tools/            Standalone developer tools (see below)
```

## Developer tools

Each tool lives in its own folder under `tools/` with a README covering what it
does and how to invoke it.

| Tool | What it does |
|---|---|
| [`tools/portal-preview/`](tools/portal-preview/README.md) | Serves the device's web-portal pages in a desktop browser with fixture API data, so portal UI changes can be reviewed and screenshotted without a device. Rendering harness only - not a device simulator. |
| [`tools/trusted-roots/`](tools/trusted-roots/README.md) | Generates the device's short trusted root certificate table from `lib/TrustedRoots/roots/`, and checks that each protected host's live chain still ends in that list (also run, non-blocking, by the release workflow). |

## License

GPL-3.0-or-later with a **HAL Linking Exception** — apps built through the published HAL API may be licensed under terms of your choice.

See [LICENSE](LICENSE) for the full text, [PERMISSIONS.md](PERMISSIONS.md) for a plain-language summary, and [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md) for dependency attribution.

## Links

- [cyberfidget.com](https://cyberfidget.com) — product site and online emulator
- [Documentation](https://docs.cyberfidget.com) — hardware specs, guides, API reference
- [Cyber Fidget Docs repo](https://github.com/CyberFidget/cyberfidget-docs) — documentation source

---

Copyright (c) 2023-2026 Dismo Industries LLC
