// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

// PowerManager.h

#ifndef POWERMANAGER_H
#define POWERMANAGER_H

#include <Arduino.h>
#include "ButtonManager.h"
#include "DisplayProxy.h"

class PowerManager {
public:
    // Constructor
    PowerManager(ButtonManager& buttonManager);

    // Initialize the power manager
    void begin();

    // Function to handle shutdown screen updates
    void update();

    void end();

    // Register button callback for shutdown
    void registerShutdownCallback();

    // Unregister button callback
    void unregisterShutdownCallback();

    // Call for Deep Sleep
    void deepSleep(bool force = false);

    // Show the empty-battery message and enter no-wake deep sleep.
    void shutdownForEmptyBattery();

private:
    // The function to display the shutdown screen
    void drawShutdownScreen();

    // The callback function for button press event
    static void onButtonPressCallback(const ButtonEvent &event);
    static void onButtonBackPressed(const ButtonEvent& event);

    // Reference to display and buttonManager
    DisplayProxy& display;
    ButtonManager& buttonManager;

    // Add a static instance reference
    static PowerManager* instance;

    // Double tap handling
    int bottomLeftButtonIndex;
    unsigned long lastTapTime;
    static const unsigned long DOUBLE_TAP_THRESHOLD_MS = 500; // Time window in milliseconds for double-tap
};

// Declare that there's a global object called below somewhere for AppDefs to use
// must be after the reference class definition exists
extern PowerManager powermanager;

// Declare global functions
void powerManagerBegin();
void powerManagerEnd();
void powerManagerUpdate();

#endif  // POWERMANAGER_H
