// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2023-2026 Dismo Industries LLC

#include "HAL.h"
#include "globals.h"
#include "DisplayProxy.h"
#include "ButtonManager.h"
#include "RGBController.h"
#include "version.h"  // FW_* macros, exposed to JS via ccall (see exports below)

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#define KEEPALIVE EMSCRIPTEN_KEEPALIVE
#else
#define KEEPALIVE
#endif

// ---- App selection (set via CMake -DWASM_APP=xxx) ----
#if defined(WASM_APP_DINOGAME)
    #include "DinoGame.h"
    #define APP_INSTANCE  dinoGame
#elif defined(WASM_APP_FLASHLIGHT)
    #include "Flashlight.h"
    #define APP_INSTANCE  flashlight
#elif defined(WASM_APP_BREAKOUT)
    #include "BreakoutGame.h"
    #define APP_INSTANCE  breakoutGame
#elif defined(WASM_APP_MATRIXSCREENSAVER)
    #include "MatrixScreensaver.h"
    #define APP_INSTANCE  matrixScreensaver
#elif defined(WASM_APP_SPACESHIP)
    #include "Spaceship.h"
    #define APP_INSTANCE  spaceshipApp
#elif defined(WASM_APP_BEWILDERYOURBRAIN)
    #include "BewilderYourBrain.h"
    #define APP_INSTANCE  bewilderYourBrainApp
#elif defined(WASM_APP_REACTIONTIMEGAME)
    #include "ReactionTimeGame.h"
    #define APP_INSTANCE  reactionTimeGame
#elif defined(WASM_APP_SPHFLUIDGAME)
    #include "SPHFluidGame.h"
    #define APP_INSTANCE  sphFluidGame
#elif defined(WASM_APP_SIMONSAYSGAME)
    #include "SimonSaysGame.h"
    #define APP_INSTANCE  simonSaysGame
#elif defined(WASM_APP_STRATAGEMGAME)
    #include "StratagemGame.h"
    #define APP_INSTANCE  stratagemGame
#elif defined(WASM_APP_BOOPER)
    #include "Booper.h"
    #define APP_INSTANCE  booper
#elif defined(WASM_APP_EYE)
    #include "Eye.h"
    #define APP_INSTANCE  eyeApp
#elif defined(WASM_APP_GHOSTS)
    #include "Ghosts.h"
    #define APP_INSTANCE  ghostsApp
#elif defined(WASM_APP_SPLOOTY)
    #include "Splooty.h"
    #define APP_INSTANCE  splootyApp
#elif defined(WASM_APP_GRAVEYARDSCREENSAVER)
    #include "GraveyardScreensaver.h"
    #define APP_INSTANCE  graveyardScreensaverApp
#elif defined(WASM_APP_CYBERFIDGIE)
    #include "MyScreensaver.h"
    #define APP_INSTANCE  myScreensaverApp
#elif defined(WASM_APP_BOOTANIMATION)
    #include "BootAnimation.h"
    #define APP_INSTANCE  bootAnimationApp
#elif defined(WASM_APP_CUSTOM)
    #include "app_include.h"
    #define APP_INSTANCE  WASM_CUSTOM_INSTANCE
#endif

extern uint8_t wasm_button_states[];
extern int wasm_analog_values[];

// ---- Exported C functions for JS bridge ----
extern "C" {

void wasm_button_press(int buttonIndex) {
    if (buttonIndex >= 0 && buttonIndex < 6) {
        wasm_button_states[buttonIndex] = 1;
    }
}

void wasm_button_release(int buttonIndex) {
    if (buttonIndex >= 0 && buttonIndex < 6) {
        wasm_button_states[buttonIndex] = 0;
    }
}

void wasm_set_slider(int value) {
    wasm_analog_values[0] = 4095 - value;
}

const uint8_t* wasm_get_framebuffer() {
    return HAL::realDisplay().getBuffer();
}

int wasm_get_framebuffer_size() {
    return HAL::realDisplay().getBufferSize();
}

void wasm_stop() {
#ifdef __EMSCRIPTEN__
    emscripten_cancel_main_loop();
#endif
}

// Firmware version surface for JS callers (App Builder mismatch warning,
// The Archives compatibility checks). Same values as the firmware HAL exposes.
KEEPALIVE const char* cyberfidget_version_string()  { return FW_VERSION_FULL_STRING; }
KEEPALIVE uint32_t    cyberfidget_version_encoded() { return FW_VERSION; }
KEEPALIVE const char* cyberfidget_build_type()      { return FW_BUILD_TYPE; }

} // extern "C"

// ---- Default demo (used when no app is selected) ----
#ifndef APP_INSTANCE

static int demoFrame = 0;

static void demoBegin() { demoFrame = 0; }

static void demoUpdate() {
    auto& display = HAL::displayProxy();
    display.clear();

    display.setFont(ArialMT_Plain_10);
    display.setTextAlignment(TEXT_ALIGN_CENTER);
    display.drawString(64, 5, "CYBER FIDGET");
    display.drawString(64, 20, "WASM Emulator");

    display.drawRect(10, 38, 108, 12);
    int fill = (demoFrame % 100);
    display.fillRect(12, 40, fill, 8);

    display.setTextAlignment(TEXT_ALIGN_LEFT);
    display.drawString(10, 52, "Slider: " + String(sliderPosition_8Bits_Filtered));

    display.display();
    demoFrame++;
}

#endif // !APP_INSTANCE

// ---- Main loop called by Emscripten ----
static void mainLoop() {
    HAL::loopHardware();
    updateStrip();

    ButtonEvent ev;
    while (HAL::buttonManager().getNextEvent(ev)) {
        if (HAL::buttonManager().hasCallback(ev.buttonIndex)) {
            auto cb = HAL::buttonManager().getCallback(ev.buttonIndex);
            if (cb) cb(ev);
        }
    }

#ifdef APP_INSTANCE
    APP_INSTANCE.update();
#else
    demoUpdate();
#endif
}

int main() {
    HAL::initEasyEverything();

#ifdef APP_INSTANCE
    APP_INSTANCE.begin();
#else
    demoBegin();
#endif

#ifdef __EMSCRIPTEN__
    emscripten_set_main_loop(mainLoop, 50, 1);
#else
    for (int i = 0; i < 300; i++) mainLoop();
#endif

    return 0;
}
