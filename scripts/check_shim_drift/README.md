<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 Dismo Industries LLC -->

# Device shim drift checker

This standard-library-only Python tool checks the hand-authored device-WASM adapters against their in-repo firmware declarations. It verifies the field name, type, and order of the ABI-copied `ButtonEvent` and `AudioManager::ToneStep` structures; the button-count, button-index, and pixel-index constants; and matching declarations on the `ButtonManager`, `AudioManager`, and `DisplayProxy` adapter surfaces.

Deliberate adapter behavior is documented in `adapter_allowlist.json`. A mirrored signature that differs without a file-and-symbol allowlist entry fails with an actionable diagnostic. Broad entries document adapters that have no direct firmware declaration; they do not suppress mismatches on the checked mirrored class methods.

The ThingPulse dependency is declared in `platformio.ini` as `thingpulse/ESP8266 and ESP32 OLED driver for SSD1306 displays@^4.6.1`, but its headers are downloaded by PlatformIO and are not vendored in this repository. Consequently, this phase checks that dependency mapping and compares the guest's mirrored methods with the in-repo `lib/DisplayProxy/DisplayProxy.h` wrapper. It does not invent or inspect a `.pio` dependency path.

Run the checker from the repository root:

```sh
python scripts/check_shim_drift/check_shim_drift.py
```

Run its tests:

```sh
python scripts/check_shim_drift/test_check_shim_drift.py
```
