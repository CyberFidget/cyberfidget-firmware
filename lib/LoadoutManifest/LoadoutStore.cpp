// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

// lib/LoadoutManifest/LoadoutStore.cpp — see LoadoutStore.h.

#ifndef HOST_TEST

#include "LoadoutStore.h"

#include <cstring>

#include <FS.h>
#include <LittleFS.h>
#include "esp_log.h"

static const char* TAG_LOADOUT = "LoadoutStore";

static const char* kManifestPath = "/loadout.json";
static const char* kTempPath     = "/loadout.json.tmp";

namespace LoadoutStore {

static bool mounted = false;

// Boot-time sweep of orphaned "<file>.part" temp files left behind when an
// fwrite transfer was interrupted (power cut / cable pull between fwdata and
// fwcommit). Confined to the two write roots so it can never touch anything
// outside the app/asset area. Cheap: one shallow directory listing per root,
// once per boot on the first successful mount.
static void sweepPartFiles() {
    static const char* kRoots[] = {"/apps", "/assets"};
    for (unsigned r = 0; r < sizeof(kRoots) / sizeof(kRoots[0]); ++r) {
        const char* root = kRoots[r];
        File dir = LittleFS.open(root);
        if (!dir) continue;
        if (!dir.isDirectory()) { dir.close(); continue; }
        // Collect matches first, then delete: removing a file while its parent
        // directory handle is mid-iteration is not reliable across LittleFS
        // versions. Bounded batch — a boot never has more than a couple of
        // orphans, and any left over get caught on a later boot.
        char victims[8][112];
        int n = 0;
        const int kCap = (int)(sizeof(victims) / sizeof(victims[0]));
        for (File e = dir.openNextFile(); e; e = dir.openNextFile()) {
            const char* nm = e.name();
            bool isDir = e.isDirectory();
            e.close();
            if (isDir || !nm) continue;
            size_t len = strlen(nm);
            if (len < 5 || strcmp(nm + len - 5, ".part") != 0) continue;
            if (n >= kCap) break;
            // File::name() may be a bare basename or a full path depending on
            // the core version; normalize to an absolute path under this root.
            if (nm[0] == '/') {
                strncpy(victims[n], nm, sizeof(victims[0]) - 1);
                victims[n][sizeof(victims[0]) - 1] = '\0';
            } else {
                snprintf(victims[n], sizeof(victims[0]), "%s/%s", root, nm);
            }
            n++;
        }
        dir.close();
        for (int i = 0; i < n; ++i) {
            LittleFS.remove(victims[i]);
            ESP_LOGI(TAG_LOADOUT, "swept orphaned temp file %s", victims[i]);
        }
    }
}

bool begin() {
    if (mounted) return true;
    // true = format on failed mount: first boot the `spiffs` partition
    // holds no LittleFS image, so let it format itself once.
    mounted = LittleFS.begin(true);
    if (!mounted) {
        ESP_LOGE(TAG_LOADOUT, "LittleFS mount failed; loadout manifest unavailable");
        return mounted;
    }
    sweepPartFiles();  // one-shot orphan cleanup on first successful mount
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
