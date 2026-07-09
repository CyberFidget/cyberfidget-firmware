// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

// lib/LoadoutManifest/LoadoutStore.cpp — see LoadoutStore.h.

#ifndef HOST_TEST

#include "LoadoutStore.h"

#include <FS.h>
#include <LittleFS.h>
#include "esp_log.h"

static const char* TAG_LOADOUT = "LoadoutStore";

static const char* kManifestPath = "/loadout.json";
static const char* kTempPath     = "/loadout.json.tmp";

namespace LoadoutStore {

static bool mounted = false;

bool begin() {
    if (mounted) return true;
    // true = format on failed mount: first boot the `spiffs` partition
    // holds no LittleFS image, so let it format itself once.
    mounted = LittleFS.begin(true);
    if (!mounted) {
        ESP_LOGE(TAG_LOADOUT, "LittleFS mount failed; loadout manifest unavailable");
    }
    return mounted;
}

bool load(std::string& jsonOut) {
    if (!begin()) return false;
    File f = LittleFS.open(kManifestPath, FILE_READ);
    if (!f || f.isDirectory()) return false;
    jsonOut.clear();
    jsonOut.reserve(f.size());
    uint8_t buf[256];
    while (true) {
        size_t n = f.read(buf, sizeof(buf));
        if (n == 0) break;
        jsonOut.append((const char*)buf, n);
    }
    f.close();
    return !jsonOut.empty();
}

bool save(const std::string& json) {
    if (!begin()) return false;

    // 1) Write the full document to a temp file.
    File f = LittleFS.open(kTempPath, FILE_WRITE);
    if (!f) {
        ESP_LOGE(TAG_LOADOUT, "Failed to open %s for write", kTempPath);
        return false;
    }
    size_t written = f.write((const uint8_t*)json.data(), json.size());
    f.close();
    if (written != json.size()) {
        ESP_LOGE(TAG_LOADOUT, "Short write to %s (%u/%u)", kTempPath,
                 (unsigned)written, (unsigned)json.size());
        LittleFS.remove(kTempPath);
        return false;
    }

    // 2) Rename over the real manifest. littlefs renames atomically and
    //    can replace an existing file; keep a remove+retry fallback in
    //    case the VFS layer refuses the overwrite. Worst case after a
    //    power cut here: no manifest -> compile-order menu, never a
    //    corrupt one.
    if (LittleFS.rename(kTempPath, kManifestPath)) return true;
    LittleFS.remove(kManifestPath);
    if (LittleFS.rename(kTempPath, kManifestPath)) return true;

    ESP_LOGE(TAG_LOADOUT, "Failed to rename %s -> %s", kTempPath, kManifestPath);
    return false;
}

} // namespace LoadoutStore

#endif // HOST_TEST
