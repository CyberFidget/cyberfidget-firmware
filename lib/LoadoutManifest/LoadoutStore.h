// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

/**
 * LoadoutStore — LittleFS persistence for the loadout manifest.
 *
 * Device-only companion to the pure LoadoutManifest core: mounts
 * LittleFS (on the `spiffs` data partition from default_8MB.csv) and
 * reads/writes /loadout.json. Saves are atomic-ish: the JSON is written
 * to a temp file first and renamed over the real one, so a torn write
 * leaves either the old manifest or none at all — never a half-written
 * file. A missing/unreadable manifest just means the menu falls back to
 * compiled-in order.
 *
 * Not compiled for native tests (HOST_TEST) or the WASM emulator (which
 * builds the pure core only; see wasm/CMakeLists.txt).
 */

#ifndef LOADOUT_STORE_H
#define LOADOUT_STORE_H

#ifndef HOST_TEST

#include <string>

namespace LoadoutStore {

/// Mount LittleFS (formats the partition on first use). Idempotent.
/// @return true if the filesystem is available.
bool begin();

/// Read /loadout.json into jsonOut. @return false if absent/unreadable.
bool load(std::string& jsonOut);

/// Write /loadout.json via temp-file + rename. @return true on success.
bool save(const std::string& json);

} // namespace LoadoutStore

#endif // HOST_TEST

#endif // LOADOUT_STORE_H
