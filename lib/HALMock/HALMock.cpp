// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

// lib/HALMock/HALMock.cpp — see HALMock.h for scope + usage notes.

#include "HALMock.h"

#include <vector>

#include "Arduino.h"

namespace HALMock {

float accelX = 0.0f;
float accelY = 0.0f;
float accelZ = 1.0f; // gravity baseline

float sliderPosition_Percentage_Filtered          = 0.0f;
float sliderPosition_Percentage_Inverted_Filtered = 100.0f;
int   sliderPosition_12Bits        = 0;
int   sliderPosition_8Bits_Filtered = 0;

static unsigned long s_mockMillis = 0;
static std::vector<Call> s_calls;
static int s_pinModes[64];
static int s_pinValues[64];
static bool s_cardPresent = false;

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
    resetHardware();
}

unsigned long mockMillis()                   { return s_mockMillis; }
void          mockAdvanceMs(unsigned long m) { s_mockMillis += m; }
void          mockResetMillis()              { s_mockMillis = 0; }

void resetHardware() {
    s_calls.clear();
    s_cardPresent = false;
    for (int i = 0; i < 64; ++i) {
        s_pinModes[i] = INPUT;
        s_pinValues[i] = LOW;
    }
}

void recordCall(CallType type, int pin, int value, int arg2, int arg3) {
    s_calls.push_back({type, pin, value, arg2, arg3});
    if (pin >= 0 && pin < 64) {
        if (type == CallType::PinMode) s_pinModes[pin] = value;
        if (type == CallType::DigitalWrite) s_pinValues[pin] = value;
    }
}

size_t callCount() { return s_calls.size(); }

const Call& callAt(size_t index) { return s_calls.at(index); }

int pinModeState(int pin) { return s_pinModes[pin]; }

int pinValueState(int pin) { return s_pinValues[pin]; }

void setCardPresent(bool present) { s_cardPresent = present; }

bool cardPresent() { return s_cardPresent; }

} // namespace HALMock

void pinMode(int pin, int mode) {
    HALMock::recordCall(HALMock::CallType::PinMode, pin, mode);
}

void digitalWrite(int pin, int value) {
    HALMock::recordCall(HALMock::CallType::DigitalWrite, pin, value);
}

void gpio_deep_sleep_hold_en() {
    HALMock::recordCall(HALMock::CallType::DeepSleepHoldEnable);
}
