// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2026 Dismo Industries LLC
//
// T-192: device-profile twin of the emulator's cf_custom_app.h. Force-
// included into a website-generated app's .cpp so the SAME source compiles
// for the device (wasm3 / cf.* imports) as it does for the browser
// emulator (EM_JS), even when the generated code forgot an #include. The
// ONE difference from the emulator header is the HAL backend behind these
// names - here the device shims that forward onto cf.* imports.
#ifndef CF_CUSTOM_APP_DEVICE_H
#define CF_CUSTOM_APP_DEVICE_H

// Standard C/C++ headers generated apps routinely use (snprintf, memset,
// strlen, sqrtf, fixed-width ints). The emulator gets these transitively
// through its fuller shim set; the device set is lean, so provide them here
// for force-include parity (T-192 trap 3).
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <cmath>

#include "App.h"           // optional base class some generated apps use
#include "cf_gfx.h"
#include <Arduino.h>
#include "HAL.h"
#include "globals.h"
#include "DisplayProxy.h"
#include "ButtonManager.h"
#include "AudioManager.h"
#include "RGBController.h"
#include "MenuManager.h"

#endif
