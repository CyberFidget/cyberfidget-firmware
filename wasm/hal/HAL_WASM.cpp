// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2023-2026 Dismo Industries LLC

#include "HAL.h"
#include "AudioManager.h"
#include "BatteryManager.h"
#include "RGBController.h"
#include "SliderPosition.h"
#include "ButtonManager.h"
#include "SparkFun_LIS2DH12.h"
// WiFiManagerCF intentionally NOT included — not available in WASM emulator
// (also commented out in the real HAL.cpp)

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

// ---- Pin Assignments (matching real hardware for compatibility) ----
constexpr int POWER_PIN_OLED = 12;
constexpr int POWER_PIN_AUX  = 2;
constexpr int CHRG_ENA       = 13;
constexpr int PIN            = 0;
constexpr int OLED_RESET     = 7;
constexpr int VOLT_READ_PIN  = 35;

// ---- RGBW LEDs ----
constexpr int RGB_COUNT      = 4;

// Button pins/indices and pixel indices are constexpr in HAL.h now (one
// home for hardware-fact values); this file no longer redefines them.

// ---- Accelerometer (set from JS via wasm_set_accel for DeviceMotion / mouse tilt) ----
float accelX = 0;
float accelY = 0;
float accelZ = 0;
float tempC  = 0;

#ifdef __EMSCRIPTEN__
extern "C" void wasm_set_accel(float x, float y, float z) {
    accelX = x;
    accelY = y;
    accelZ = z;
}
#else
extern "C" void wasm_set_accel(float, float, float) {}
#endif

// ---- Slider ----
int sliderPosition_Millivolts     = 0;
int sliderPosition_12Bits         = 0;
uint16_t sliderPosition_12Bits_Inverted = 0;
uint8_t  sliderPosition_8Bits          = 0;
uint8_t  sliderPosition_8Bits_Inverted = 0;
float sliderPosition_12Bits_Filtered = 0.0f;
float sliderPosition_12Bits_Inverted_Filtered = 0.0f;
int sliderPosition_8Bits_Filtered = 0;
int sliderPosition_8Bits_Inverted_Filtered = 0;
float sliderPosition_Percentage_Filtered = 0.0f;
float sliderPosition_Percentage_Filtered_Old = 0.0f;
float sliderPosition_Percentage_Inverted_Filtered = 0.0f;

// ---- Battery ----
float    batteryVoltage             = 4.2f;
float    batteryVoltagePercentage   = 100.0f;
uint16_t batteryVoltageLowCutoff    = 2750;
uint16_t batteryVoltageHighCutoff   = 4200;
bool     preventSleepWhileCharging  = true;
bool     enableBatterySOCCutoff     = false;
float    batterySOCCutoff           = 80.0f;
float    sleepChargingChangeThreshold = -10.0f;
float    batteryChangeRate          = 0.0f;

// ---- Internal singletons ----
namespace {
    static SSD1306Wire s_realDisplay(0x3C, SDA, SCL);
    static DisplayProxy s_displayProxy(s_realDisplay);

    constexpr int numButtonsLocal = 6;
    static const int s_buttonPins[numButtonsLocal] = {
        button_TopLeft, button_TopRight,
        button_MiddleLeft, button_MiddleRight,
        button_BottomLeft, button_BottomRight
    };
    static const bool s_usePullups[numButtonsLocal] = {false, false, false, false, false, true};
    static ButtonManager s_buttonManager(
        s_buttonPins, s_usePullups, numButtonsLocal, 20UL, 1500UL
    );

    static AudioManager s_audioManager;
    static SPARKFUN_LIS2DH12 s_accel;
    static Adafruit_NeoPixel s_rgbStrip(RGB_COUNT, PIN, NEO_GRBW + NEO_KHZ800);
}

// ---- JS bridge for LED updates ----
#ifdef __EMSCRIPTEN__
EM_JS(void, js_set_led, (int index, int r, int g, int b, int w), {
    if (Module.onLedUpdate) Module.onLedUpdate(index, r, g, b, w);
});


#else
inline void js_set_led(int, int, int, int, int) {}
#endif

extern unsigned long millis_NOW;
extern unsigned long millis_HAL_TASK_20MS;
extern int TASK_20MS;
extern int wasm_analog_values[];

namespace HAL {

    void initHardware() {
        s_realDisplay.init();
        s_buttonManager.init();
        s_rgbStrip.begin();
    }

    void initEasyEverything() {
        initHardware();
    }

    void loopHardware() {
        ::millis_NOW = millis();
        s_buttonManager.update();

        if ((::millis_NOW - ::millis_HAL_TASK_20MS) >= (unsigned long)::TASK_20MS) {
            ::millis_HAL_TASK_20MS = ::millis_NOW;
            int sliderRaw = ::wasm_analog_values[0];
            sliderPosition_12Bits = sliderRaw;
            sliderPosition_12Bits_Filtered = (float)sliderRaw;
            sliderPosition_8Bits_Filtered = sliderRaw * 255 / 4095;
            sliderPosition_8Bits_Inverted_Filtered = 255 - sliderPosition_8Bits_Filtered;
            sliderPosition_Percentage_Filtered = sliderRaw * 100.0f / 4095.0f;
            sliderPosition_Percentage_Inverted_Filtered = 100.0f - sliderPosition_Percentage_Filtered;
        }

        // Push LED state to JS if changed
        if (s_rgbStrip.needsShow()) {
            s_rgbStrip.clearNeedsShow();
            for (uint16_t i = 0; i < s_rgbStrip.numPixels(); i++) {
                uint8_t r, g, b, w;
                s_rgbStrip.getLedRGBW(i, r, g, b, w);
                js_set_led(i, r, g, b, w);
            }
        }
    }

    AudioManager& audioManager()        { return s_audioManager; }
    ButtonManager& buttonManager()      { return s_buttonManager; }
    SSD1306Wire& realDisplay()          { return s_realDisplay; }
    SPARKFUN_LIS2DH12& accelerometer()  { return s_accel; }
    Adafruit_NeoPixel& strip()          { return s_rgbStrip; }

    // wifiManagerCF() intentionally omitted — not available in WASM

    void configureWakeupPins() {}
    void enterDeepSleep() {}
    void setAuxPower(bool) {}

    float getBatteryVoltage()    { return batteryVoltage; }
    float getAccelerometerX()    { return accelX; }
    float getAccelerometerY()    { return accelY; }
    float getAccelerometerZ()    { return accelZ; }

    void setOledPower(bool) {}

    DisplayProxy& displayProxy() { return s_displayProxy; }

    void clearDisplay()  { s_realDisplay.clear(); }
    void updateDisplay() { s_realDisplay.display(); }
    void drawString(int16_t x, int16_t y, const String &text) {
        s_realDisplay.drawString(x, y, text);
    }

    void setRgbLed(int index, uint8_t r, uint8_t g, uint8_t b, uint8_t w) {
        s_rgbStrip.setPixelColor(index, Adafruit_NeoPixel::Color(r, g, b, w));
        invalidateColorCache();
        s_rgbStrip.show();
    }

    void setRgbLedsOff() {
        for (uint16_t i = 0; i < s_rgbStrip.numPixels(); i++) {
            s_rgbStrip.setPixelColor(i, 0);
        }
        s_rgbStrip.show();
    }

    void chargingEnable()  {}
    void chargingDisable() {}

    void updateAccelerometer() {}

    void printWakeupReason() {}
}
