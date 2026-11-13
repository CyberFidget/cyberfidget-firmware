// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

// lib/LoadoutManifest/LoadoutStore.cpp — see LoadoutStore.h.

#ifndef HOST_TEST

#include "LoadoutStore.h"

#include <cstring>

#include <FS.h>
#include <LittleFS.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <esp_ota_ops.h>
#include "esp_log.h"

static const char* TAG_LOADOUT = "LoadoutStore";

namespace LoadoutStore {

static bool mounted = false;

// Created during static initialization, before any task can race for it.
static SemaphoreHandle_t manifestLock = xSemaphoreCreateRecursiveMutex();

void lock() {
    if (manifestLock) xSemaphoreTakeRecursive(manifestLock, portMAX_DELAY);
}

void unlock() {
    if (manifestLock) xSemaphoreGiveRecursive(manifestLock);
}

// Boot-time sweep of orphaned "<file>.part" temp files left behind when an
// fwrite transfer was interrupted (power cut / cable pull between fwdata and
// fwcommit). Confined to the two write roots so it can never touch anything
// outside the app/asset area. Cheap: one shallow directory listing per root,
// once per boot on the first successful mount.
static void sweepPartFiles() {
    static const char* kRoots[] = {"/apps", "/assets", "/assets/ss"};
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
    // holds no LittleFS image, so let it format itself once. Never while a
    // just-installed update is unconfirmed: its failure must hand the
    // previous image the filesystem exactly as it was.
    bool formatOnFail = true;
    const esp_partition_t* running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (running && esp_ota_get_state_partition(running, &state) == ESP_OK &&
        state == ESP_OTA_IMG_PENDING_VERIFY) {
        formatOnFail = false;
    }
    mounted = LittleFS.begin(formatOnFail);
    if (!mounted) {
        ESP_LOGE(TAG_LOADOUT, "LittleFS mount failed; loadout manifest unavailable");
        return mounted;
    }
    sweepPartFiles();  // one-shot orphan cleanup on first successful mount
    return mounted;
}

bool formatForFactoryReset() {
    Guard guard;
    if (mounted) {
        LittleFS.end();
        mounted = false;
    } else {
        // format() needs the partition label that only begin() records; a
        // start-up that finishes an interrupted reset has not mounted yet.
        // The mount result does not matter (the storage may be half erased).
        LittleFS.begin(false);
        LittleFS.end();
    }
    return LittleFS.format();
}

bool load(std::string& jsonOut) {
    return read(jsonOut) == LoadoutStoreIO::ReadResult::Ok && !jsonOut.empty();
}

LoadoutStoreIO::ReadResult read(std::string& jsonOut) {
    Guard guard;
    jsonOut.clear();
    if (!begin()) return LoadoutStoreIO::ReadResult::Unreadable;
    return LoadoutStoreIO::read(LittleFS, jsonOut);
}

bool save(const std::string& json) {
    Guard guard;  // one writer of the shared temp path at a time
    if (!begin()) return false;

    if (LoadoutStoreIO::save(LittleFS, json)) return true;
    ESP_LOGE(TAG_LOADOUT, "Failed to save loadout manifest; previous file retained");
    return false;
}

} // namespace LoadoutStore

#endif // HOST_TEST
