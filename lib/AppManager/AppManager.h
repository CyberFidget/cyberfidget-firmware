// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#ifndef APP_MANAGER_H
#define APP_MANAGER_H

#include <vector>

#include "AppDefs.h"
#include "LoadoutManifest.h"

class AppManager {
public:
    // The typical "singleton accessor"
    static AppManager& instance();

    // Non-static methods
    void setup();
    void loop();

    // Switch apps
    void switchToApp(AppIndex newApp);

    // Currently running app (read-only; used by the test-mode Serial CLI)
    AppIndex activeApp() const { return appActive; }

    /**
     * Persist a menu arrangement to the loadout manifest (/loadout.json).
     * Called by MenuManager when the user commits a long-press reorder.
     * `order` is the full display order (id-anchored, REQ-053 arrange op);
     * on a device without a manifest yet, the compiled-in registry is
     * snapshotted first and the arrange is applied on top.
     */
    void persistMenuArrangement(const std::vector<LoadoutManifest::ArrangeItem>& order);

    /**
     * Apply a staged-ops document (serial sync write direction) to the
     * loadout manifest and persist it atomically. `opsJson` is the ops
     * vocabulary consumed by LoadoutManifest::applyOps (adds/removes/hides +
     * one arrange). Starts from the stored manifest, or a compiled-in
     * registry snapshot on a device that has none yet. Returns true and
     * (when non-null) reports the resulting entry count and applied-op count
     * only when the document parses, every op succeeds, AND the save lands;
     * a malformed or rejected document leaves the stored manifest untouched.
     * The new order takes effect at the next boot menu build, mirroring the
     * on-device reorder path.
     */
    bool applyLoadoutOps(const char* opsJson, int* entriesOut = nullptr,
                         int* appliedOut = nullptr);

private:
    // Private constructor
    AppManager();

    void processButtonEvents();
    void runActiveApp();

    // Non-static fields
    AppIndex appActive     = APP_MENU;
    AppIndex appPreviously = APP_MENU;

    unsigned long lastUpdateMs = 0;
};

#endif
