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

/// Close our mount and format only the LittleFS partition. No other flash partition.
bool formatForFactoryReset();

/// Read /loadout.json into jsonOut. @return false if absent/unreadable.
bool load(std::string& jsonOut);

/// Write /loadout.json via temp-file + rename. @return true on success.
bool save(const std::string& json);

/// Serializes manifest read-modify-write across tasks (the loop's menu
/// reorder and the network worker's apply share /loadout.json.tmp).
/// Recursive: a holder may call helpers that take it again.
void lock();
void unlock();

/// Holds lock() for a scope.
class Guard {
public:
    Guard() { lock(); }
    ~Guard() { unlock(); }
    Guard(const Guard&) = delete;
    Guard& operator=(const Guard&) = delete;
};

} // namespace LoadoutStore

#endif // HOST_TEST

#endif // LOADOUT_STORE_H
