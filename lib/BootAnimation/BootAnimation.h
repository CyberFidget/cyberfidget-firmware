// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#ifndef BOOTANIMATION_H
#define BOOTANIMATION_H

#include "ButtonManager.h"
#include "MenuManager.h"

void drawBootAnimation();

class BootAnimationApp {
    public:
        BootAnimationApp(ButtonManager& btnMgr);
        // Draws the opening frame before the rest of start-up (screen on).
        static void showFirstFrame();
        void begin();
        void end();
        void update();
        void timeout();
    
        static void onButtonBackPressed(const ButtonEvent& event);
        static BootAnimationApp* instance;
    
    private:
        ButtonManager& buttonManager;
    };

extern BootAnimationApp bootAnimationApp;

#endif // BOOTANIMATION_H