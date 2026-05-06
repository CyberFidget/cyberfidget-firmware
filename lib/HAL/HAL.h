// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#ifndef HAL_H
#define HAL_H

#include <Arduino.h>


#include <SSD1306Wire.h>
#include "DisplayProxy.h"

// Forward-declare any hardware-related classes you want to expose from the HAL:
class AudioManager;
class ButtonManager;
class SPARKFUN_LIS2DH12;
class Adafruit_NeoPixel;

// Buttons
extern const int button_TopLeft;
extern const int button_TopRight;
extern const int button_MiddleLeft;
extern const int button_MiddleRight;
extern const int button_BottomLeft;
extern const int button_BottomRight;

extern const int button_TopLeftIndex;
extern const int button_TopRightIndex;
extern const int button_MiddleLeftIndex;
extern const int button_MiddleRightIndex;
extern const int button_BottomLeftIndex;
extern const int button_BottomRightIndex;

extern const int button_LeftIndex;
extern const int button_RightIndex;
extern const int button_UpIndex;
extern const int button_DownIndex;
extern const int button_SelectIndex;
extern const int button_EnterIndex;

// RGBW LEDS
extern const uint16_t pixel_Front_Top;
extern const uint16_t pixel_Front_Middle;
extern const uint16_t pixel_Front_Bottom;
extern const uint16_t pixel_Back;

// Accelerometer readings
extern float accelX;
extern float accelY;
extern float accelZ;
extern float tempC;

// Slider readings
extern int sliderPosition_Millivolts;
extern int sliderPosition_12Bits;
// Filtered slider positions
extern float sliderPosition_12Bits_Filtered;
extern float sliderPosition_12Bits_Inverted_Filtered;
extern int sliderPosition_8Bits_Filtered;
extern int sliderPosition_8Bits_Inverted_Filtered;
extern float sliderPosition_Percentage_Filtered;
extern float sliderPosition_Percentage_Filtered_Old;
extern float sliderPosition_Percentage_Inverted_Filtered;

// Battery
extern float batteryVoltage;
extern float batteryVoltagePercentage;
extern uint16_t batteryVoltageLowCutoff;
extern uint16_t batteryVoltageHighCutoff;
extern bool preventSleepWhileCharging;
extern bool enableBatterySOCCutoff;
extern float batterySOCCutoff;
extern float sleepChargingChangeThreshold;
extern float batteryChangeRate;

// The “HAL” namespace wraps all hardware initialization + references
namespace HAL
{
    // Initializes all hardware (pins, I2C, display, accelerometer, etc.)
    void initHardware();

    // Initializes all the things for standalone apps
    void initEasyEverything();

    // A “loop” method, if you want to update any hardware each pass
    void loopHardware();

    // Provide references to the hardware objects so higher-level code
    // (like AppManager) can access them without knowing the low-level details.
    AudioManager& audioManager();
    ButtonManager& buttonManager();

    SPARKFUN_LIS2DH12& accelerometer();

    // If you want to set wake pins, deep sleep, etc. directly from AppManager
    void configureWakeupPins();
    void enterDeepSleep();

    // Additional helper calls for controlling certain pins, turning hardware on/off, etc.
    void setOledPower(bool on);
    void setAuxPower(bool on);
    void chargingEnable();
    void chargingDisable();
    void updateAccelerometer();
    void printWakeupReason();
    void setRgbLed(int index, uint8_t r, uint8_t g, uint8_t b, uint8_t w);
    void setRgbLedsOff();
    Adafruit_NeoPixel& strip();

    // Display-related methods
    DisplayProxy& displayProxy();
    SSD1306Wire& realDisplay();
    void clearDisplay();
    void updateDisplay();
    void drawString(int16_t x, int16_t y, const String &text);

    // Boot banner — emits the firmware identification line to Serial. Lives in
    // HAL because it touches the UART; the underlying values are exposed
    // directly via getFirmwareVersionString() etc. in lib/Globals/.
    void        printFirmwareBanner();        // [boot] fw=... line, called from initHardware

    // ... add more hardware-related getters/setters as you see fit
};

#endif // HAL_H
