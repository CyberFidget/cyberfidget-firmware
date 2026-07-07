// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#ifndef APP_MANAGER_H
#define APP_MANAGER_H

#include "AppDefs.h"

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
