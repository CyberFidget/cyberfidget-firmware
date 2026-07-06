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

    /**
     * Persist a menu arrangement to the loadout manifest (/loadout.json).
     * Called by MenuManager when the user commits a long-press reorder.
     * `order` is the full display order (id-anchored, REQ-053 arrange op);
     * on a device without a manifest yet, the compiled-in registry is
     * snapshotted first and the arrange is applied on top.
     */
    void persistMenuArrangement(const std::vector<LoadoutManifest::ArrangeItem>& order);

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
