// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC
//
// Guest-side glue for device wasm app modules (spike/wasm3-apps).
// Defines the shimmed globals/singletons and exports the app lifecycle:
//   app_begin / app_update / app_end / app_handle_button
// Select the app at compile time with -DCF_WASM_APP_<APP>
// (see build_device_modules.bat for the list).

#include "Arduino.h"
#include "AudioManager.h"
#include "ButtonManager.h"
#include "DisplayProxy.h"
#include "HAL.h"
#include "MenuManager.h"
#include "RGBController.h"
#include "cf_hal_imports.h"
#include "globals.h"

#if defined(CF_WASM_APP_REACTION)
#include "ReactionTimeGame.h"
#define CF_APP_INSTANCE reactionTimeGame
#elif defined(CF_WASM_APP_BREAKOUT)
#include "BreakoutGame.h"
#define CF_APP_INSTANCE breakoutGame
#elif defined(CF_WASM_APP_STARBURST)
#include "Starburst.h"
#define CF_APP_INSTANCE starburstApp
#elif defined(CF_WASM_APP_SPACESHIP)
#include "Spaceship.h"
#define CF_APP_INSTANCE spaceshipApp
#elif defined(CF_WASM_APP_SPHFLUID)
#include "SPHFluidGame.h"
#define CF_APP_INSTANCE sphFluidGame
#elif defined(CF_WASM_APP_SPLOOTY)
#include "Splooty.h"
#define CF_APP_INSTANCE splootyApp
#elif defined(CF_WASM_APP_CUSTOM)
// T-192: the website's user-app path. Mirrors the emulator's WASM_APP_CUSTOM
// branch so the SAME generated source builds both ways. The build passes the
// header name and the instance identifier the same way the emulator CMake
// does (CUSTOM_APP_NAME -> a <name>.h header; CUSTOM_APP_INSTANCE -> the
// global). The force-includes come via -include cf_custom_app_device.h.
#include CF_CUSTOM_APP_HEADER
#define CF_APP_INSTANCE CUSTOM_APP_INSTANCE
#else
#error "Define one CF_WASM_APP_<APP> (see build_device_modules.bat) or CF_WASM_APP_CUSTOM"
#endif

// ---- Globals declared in the shim headers ----
unsigned long millis_NOW = 0;
unsigned long millis_APP_LASTINTERACTION = 0;
const char* TAG_MAIN = "wasmapp";

float accelX = 0.0f;
float accelY = 0.0f;
float accelZ = 0.0f;
float tempC  = 0.0f;

float sliderPosition_Percentage_Filtered          = 0.0f;
float sliderPosition_Percentage_Inverted_Filtered = 0.0f;
int   sliderPosition_8Bits_Filtered               = 0;
int   sliderPosition_8Bits_Inverted_Filtered      = 0;
float sliderPosition_12Bits_Filtered              = 0.0f;
float sliderPosition_12Bits_Inverted_Filtered     = 0.0f;

// ---- Font handles (ID byte read by DisplayProxy::setFont) ----
const uint8_t ArialMT_Plain_10[] = {10};
const uint8_t ArialMT_Plain_16[] = {16};
const uint8_t ArialMT_Plain_24[] = {24};

// ---- Singletons ----
static DisplayProxy  g_displayProxy;
static ButtonManager g_buttonManager;
static AudioManager  g_audioManager;
static MenuManager   g_menuManager;

namespace HAL {
DisplayProxy& displayProxy() { return g_displayProxy; }
ButtonManager& buttonManager() { return g_buttonManager; }
AudioManager& audioManager() { return g_audioManager; }

void clearDisplay() { g_displayProxy.clear(); }
void updateDisplay() { g_displayProxy.display(); }
void drawString(int16_t x, int16_t y, const String& text) {
    g_displayProxy.drawString(x, y, text);
}
void setRgbLed(int index, uint8_t r, uint8_t g, uint8_t b, uint8_t w) {
    cf_led_set(index, r, g, b, w);
}
void setRgbLedsOff() { cf_led_all_off(); }
}  // namespace HAL

MenuManager& MenuManager::instance() { return g_menuManager; }

// ---- ButtonManager (guest-local callback table) ----
void ButtonManager::registerCallback(int buttonIndex, ButtonCallback callback) {
    if (buttonIndex < 0 || buttonIndex >= kMaxButtons) return;
    callbacks[buttonIndex] = callback;
}
void ButtonManager::unregisterCallback(int buttonIndex) {
    if (buttonIndex < 0 || buttonIndex >= kMaxButtons) return;
    callbacks[buttonIndex] = nullptr;
}
bool ButtonManager::hasCallback(int buttonIndex) const {
    return buttonIndex >= 0 && buttonIndex < kMaxButtons && callbacks[buttonIndex];
}
ButtonCallback ButtonManager::getCallback(int buttonIndex) const {
    if (buttonIndex < 0 || buttonIndex >= kMaxButtons) return nullptr;
    return callbacks[buttonIndex];
}
void ButtonManager::dispatch(int buttonIndex, int eventType) {
    if (buttonIndex < 0 || buttonIndex >= kMaxButtons) return;
    ButtonCallback cb = callbacks[buttonIndex];
    if (!cb) return;
    ButtonEvent ev;
    ev.buttonIndex = buttonIndex;
    ev.eventType   = (ButtonEventType)eventType;
    cb(ev);
}

// ---- RGBController free functions ----
void setRandomColors() {
    for (int i = 0; i < 4; i++) {
        cf_led_set(i, (int32_t)random(0, 256), (int32_t)random(0, 256),
                   (int32_t)random(0, 256), 0);
    }
}
void setDeterminedColorsFront(uint8_t r, uint8_t g, uint8_t b, uint8_t w) {
    cf_led_set(pixel_Front_Top, r, g, b, w);
    cf_led_set(pixel_Front_Middle, r, g, b, w);
    cf_led_set(pixel_Front_Bottom, r, g, b, w);
}
void setDeterminedColorsAll(uint8_t r, uint8_t g, uint8_t b, uint8_t w) {
    for (int i = 0; i < 4; i++) cf_led_set(i, r, g, b, w);
}
void setColorsOff() { cf_led_all_off(); }

// No-op: the host flushes the strip every loop and cf.led_set marks it dirty
// host-side (see shims/RGBController.h).
void updateStrip() {}

// Exact copy of lib/RGBController/RGBController.cpp mapToRainbow — pure math,
// kept guest-side because reference out-params can't cross the wasm ABI.
void mapToRainbow(int input, uint8_t dim, uint8_t &red, uint8_t &green, uint8_t &blue) {
    input = input < 0 ? 0 : (input > 4095 ? 4095 : input);
    dim = dim > 255 ? 255 : dim;

    float normalized = (float)input / 4095.0;

    float x, y, z;
    if (normalized < 0.2) {
        x = 1.0; y = normalized / 0.2; z = 0.0;
    } else if (normalized < 0.4) {
        x = 1.0; y = 1.0; z = 0.0;
    } else if (normalized < 0.6) {
        float t = (normalized - 0.4) / 0.2;
        x = 1.0 - t; y = 1.0; z = 0.0;
    } else if (normalized < 0.8) {
        float t = (normalized - 0.6) / 0.2;
        x = 0.0; y = 1.0 - t; z = t;
    } else {
        float t = (normalized - 0.8) / 0.2;
        x = t; y = 0.0; z = 1.0;
    }

    red   = (uint8_t)(y * dim);
    green = (uint8_t)(x * dim);
    blue  = (uint8_t)(z * dim);
}

// ---- State refresh + exported lifecycle ----
static void refreshHostState() {
    millis_NOW = (unsigned long)cf_millis();
    accelX = cf_accel_x();
    accelY = cf_accel_y();
    accelZ = cf_accel_z();
    float pct = cf_slider_pct();
    sliderPosition_Percentage_Filtered          = pct;
    sliderPosition_Percentage_Inverted_Filtered = 100.0f - pct;
    sliderPosition_8Bits_Filtered               = (int)(pct * 255.0f / 100.0f);
    sliderPosition_8Bits_Inverted_Filtered      = 255 - sliderPosition_8Bits_Filtered;
    sliderPosition_12Bits_Filtered              = pct * 4095.0f / 100.0f;
    sliderPosition_12Bits_Inverted_Filtered     = 4095.0f - sliderPosition_12Bits_Filtered;
}

extern "C" {

__attribute__((export_name("app_begin"))) void app_begin() {
    refreshHostState();
    CF_APP_INSTANCE.begin();
}

__attribute__((export_name("app_update"))) void app_update() {
    refreshHostState();
    unsigned long lastInteractionBefore = millis_APP_LASTINTERACTION;
    CF_APP_INSTANCE.update();
    if (millis_APP_LASTINTERACTION != lastInteractionBefore) {
        cf_keepalive();  // forward the deep-sleep inhibit to the host
    }
}

__attribute__((export_name("app_end"))) void app_end() {
    CF_APP_INSTANCE.end();
}

__attribute__((export_name("app_handle_button"))) void app_handle_button(
    int32_t buttonIndex, int32_t eventType) {
    refreshHostState();
    g_buttonManager.dispatch(buttonIndex, eventType);
}

}  // extern "C"
