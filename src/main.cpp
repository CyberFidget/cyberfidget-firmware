// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

/**
 * Cyber Fidget Firmware — Main Entry Point
 *
 * This file demonstrates how to create two distinct code paths for your
 * Cyber Fidget project:
 *
 * 1) An AppManager-driven (multi-app) approach.
 * 2) A straightforward single-application approach using HAL calls.
 *
 * Choose which approach you want to use by defining USE_APPMANAGER in your
 * platformio.ini or by adding "#define USE_APPMANAGER" at the top of this file.
 *
 * Example (in platformio.ini):
 *   [env:your_environment]
 *   build_flags = -DUSE_APPMANAGER
 *
 * If you do NOT define USE_APPMANAGER, the single-app approach will be used by default.
 */

#define USE_APPMANAGER  // Uncomment this line to use the AppManager approach

// A freshly installed update must prove itself before it is kept. Without
// this the Arduino core marks it good before setup() even runs; with it,
// the update's own self-test (lib/CloudSync/UpdateSession) decides, and a
// failure or a crash before that returns the Fidget to the previous image.
extern "C" bool verifyRollbackLater() { return true; }

/********************************************************************
*   OPTION 1: APPMANAGER APPROACH
*   ---------------------------------
*   If USE_APPMANAGER is defined, we rely on AppManager to handle
*   setup, updates, and switching between potential multiple apps.
********************************************************************/
#ifdef USE_APPMANAGER

#include "AppManager.h"

void setup() {
  // Let AppManager handle setup.
  AppManager::instance().setup();
}

void loop() {
  // Let AppManager handle the main loop.
  AppManager::instance().loop();
}

#else  //*************************************************************

/********************************************************************
*   OPTION 2: SINGLE APPLICATION APPROACH
*   --------------------------------------
*   If USE_APPMANAGER is NOT defined, we use a simpler, all-in-one
*   application flow using HAL calls directly.
********************************************************************/

#include "globals.h"

void setup() {
  HAL::initEasyEverything();
}

void loop() {
  // Regularly poll hardware
  HAL::loopHardware();

  // Display a message & slider reading
  HAL::clearDisplay();
  HAL::drawString(0, 0, "Hello, World!");
  HAL::drawString(
    0, 10,
    "Slider: " + String(sliderPosition_Percentage_Inverted_Filtered) + "%"
  );
  HAL::updateDisplay();

  // Adjust the front-top RGB LED based on the slider
  int brightness = map(
    sliderPosition_Percentage_Inverted_Filtered, 0, 100, 0, 255
  );
  HAL::setRgbLed(pixel_Front_Top, brightness, 0, 0, 0);
}

#endif // USE_APPMANAGER