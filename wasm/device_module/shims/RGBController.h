// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC
//
// Guest-side RGBController shim (free-function API like lib/RGBController).
// Pixel index map: 0=Back 1=FrontTop 2=FrontMiddle 3=FrontBottom.

#ifndef RGB_CONTROLLER_H  // same guard as the real header — must shadow it
#define RGB_CONTROLLER_H

#include <stdint.h>

void setRandomColors();
void setDeterminedColorsFront(uint8_t r, uint8_t g, uint8_t b, uint8_t w);
void setDeterminedColorsAll(uint8_t r, uint8_t g, uint8_t b, uint8_t w);
void setColorsOff();

// No-op in guests: the host flushes the NeoPixel strip every loop iteration
// (HAL::loopHardware -> updateStrip with dirty-flag throttling), and every
// cf.led_set marks the strip dirty host-side. Kept so app code that pushes
// the strip explicitly (e.g. Starburst) compiles unmodified.
void updateStrip();

// Pure math (hue 0..4095 -> RGB), implemented guest-side in cf_app_glue.cpp
// as an exact copy of lib/RGBController/RGBController.cpp — reference
// out-params can't cross the wasm ABI, and a host round-trip per LED per
// frame would be wasteful. Keep in sync with the firmware implementation.
void mapToRainbow(int input, uint8_t dim, uint8_t &red, uint8_t &green, uint8_t &blue);

#endif  // RGB_CONTROLLER_H
