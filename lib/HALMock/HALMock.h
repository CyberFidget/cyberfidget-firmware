// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

// lib/HALMock/HALMock.h
//
// Host-side stubs of the HAL extern globals that apps read. Linked only by
// the native test envs (see platformio.ini [env:test_*]). Lets a test set a
// slider/accel value, then call a pure-logic function and assert on output —
// without dragging in Arduino, AudioTools, or the OLED driver.
//
// SCOPE: this is the minimum scaffold. It mirrors only the extern globals
// that BreakoutGame's input-mapping pure functions consume. Full
// BreakoutGame-instance tests (which would need stubs for DisplayProxy,
// AudioManager, ButtonManager, RGBController, MenuManager, etc.) are punted
// to T-002's WASM SIL harness — they're the right tool for that fidelity.
// See plan file `i-want-to-update-toasty-crown.md` §6c for the layering.
//
// To use from a test:
//
//   #include "HALMock.h"
//   void test_x() {
//       HALMock::setSliderPercent(50.0f);
//       float px = BreakoutMath::paddleXFromSlider(
//           HALMock::sliderPosition_Percentage_Filtered, 128, 20);
//       TEST_ASSERT_FLOAT_WITHIN(0.5f, 54.0f, px);
//   }

#ifndef HAL_MOCK_H
#define HAL_MOCK_H

#include <stdint.h>

namespace HALMock {

// Mirror of the extern globals BreakoutGame consumes from real HAL.
// Defined in HALMock.cpp; tests read/write directly.
extern float accelX;
extern float accelY;
extern float accelZ;

extern float sliderPosition_Percentage_Filtered;          // [0, 100]
extern float sliderPosition_Percentage_Inverted_Filtered; // [0, 100]
extern int   sliderPosition_12Bits;
extern int   sliderPosition_8Bits_Filtered;

// Convenience setters — explicit names so test code reads cleanly.
void setSliderPercent(float pct);
void setAccel(float x, float y = 0.0f, float z = 0.0f);
void resetAll();

// Host-side time. Tests advance manually.
unsigned long mockMillis();
void mockAdvanceMs(unsigned long ms);
void mockResetMillis();

// Helper for unit tests of float comparisons that should match exactly.
inline bool approxEqual(float a, float b, float eps) {
    float d = a - b;
    if (d < 0) d = -d;
    return d <= eps;
}

} // namespace HALMock

#endif // HAL_MOCK_H
