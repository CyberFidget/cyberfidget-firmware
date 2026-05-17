// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

// lib/HALMock/HALMock.cpp — see HALMock.h for scope + usage notes.

#include "HALMock.h"

namespace HALMock {

float accelX = 0.0f;
float accelY = 0.0f;
float accelZ = 1.0f; // gravity baseline

float sliderPosition_Percentage_Filtered          = 0.0f;
float sliderPosition_Percentage_Inverted_Filtered = 100.0f;
int   sliderPosition_12Bits        = 0;
int   sliderPosition_8Bits_Filtered = 0;

static unsigned long s_mockMillis = 0;

void setSliderPercent(float pct) {
    if (pct < 0.0f) pct = 0.0f;
    if (pct > 100.0f) pct = 100.0f;
    sliderPosition_Percentage_Filtered          = pct;
    sliderPosition_Percentage_Inverted_Filtered = 100.0f - pct;
    sliderPosition_12Bits        = static_cast<int>((pct / 100.0f) * 4095.0f);
    sliderPosition_8Bits_Filtered = static_cast<int>((pct / 100.0f) * 255.0f);
}

void setAccel(float x, float y, float z) {
    accelX = x;
    accelY = y;
    accelZ = z;
}

void resetAll() {
    accelX = 0.0f;
    accelY = 0.0f;
    accelZ = 1.0f;
    sliderPosition_Percentage_Filtered          = 0.0f;
    sliderPosition_Percentage_Inverted_Filtered = 100.0f;
    sliderPosition_12Bits        = 0;
    sliderPosition_8Bits_Filtered = 0;
    s_mockMillis = 0;
}

unsigned long mockMillis()                   { return s_mockMillis; }
void          mockAdvanceMs(unsigned long m) { s_mockMillis += m; }
void          mockResetMillis()              { s_mockMillis = 0; }

} // namespace HALMock
