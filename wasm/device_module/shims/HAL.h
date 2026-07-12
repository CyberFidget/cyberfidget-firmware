// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC
//
// Guest-side HAL shim: the subset of lib/HAL/HAL.h that apps touch.
// Sensor globals are refreshed from "cf" imports at the top of every
// exported entry point (see cf_app_glue.cpp).

#ifndef HAL_H  // same guard as the real header — must shadow it
#define HAL_H

#include "Arduino.h"
#include "AudioManager.h"
#include "ButtonManager.h"
#include "DisplayProxy.h"

// Button indices — values must match lib/HAL/HAL.cpp.
static const int button_TopLeftIndex     = 0;
static const int button_TopRightIndex    = 1;
static const int button_MiddleLeftIndex  = 2;
static const int button_MiddleRightIndex = 3;
static const int button_BottomLeftIndex  = 4;
static const int button_BottomRightIndex = 5;

static const int button_LeftIndex   = button_MiddleLeftIndex;
static const int button_RightIndex  = button_MiddleRightIndex;
static const int button_UpIndex     = button_TopLeftIndex;
static const int button_DownIndex   = button_TopRightIndex;
static const int button_SelectIndex = button_BottomLeftIndex;
static const int button_EnterIndex  = button_BottomRightIndex;

// RGBW LED pixel map (0=Back 1=FrontTop 2=FrontMiddle 3=FrontBottom).
static const uint16_t pixel_Back         = 0;
static const uint16_t pixel_Front_Top    = 1;
static const uint16_t pixel_Front_Middle = 2;
static const uint16_t pixel_Front_Bottom = 3;

// Sensor globals (refreshed each guest entry from cf imports).
extern float accelX;
extern float accelY;
extern float accelZ;
extern float tempC;

extern float sliderPosition_Percentage_Filtered;
extern float sliderPosition_Percentage_Inverted_Filtered;
extern int   sliderPosition_8Bits_Filtered;
extern int   sliderPosition_8Bits_Inverted_Filtered;
extern float sliderPosition_12Bits_Filtered;
extern float sliderPosition_12Bits_Inverted_Filtered;

namespace HAL {
    DisplayProxy& displayProxy();
    ButtonManager& buttonManager();
    AudioManager& audioManager();

    void clearDisplay();
    void updateDisplay();
    void drawString(int16_t x, int16_t y, const String& text);
    void setRgbLed(int index, uint8_t r, uint8_t g, uint8_t b, uint8_t w);
    void setRgbLedsOff();
}

#endif  // HAL_H
