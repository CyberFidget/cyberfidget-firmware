// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2023-2026 Dismo Industries LLC

// Auto-included for custom app WASM builds.
// Provides all common headers so generated code compiles even if
// the app builder forgets to #include something.

#ifndef CF_CUSTOM_APP_H
#define CF_CUSTOM_APP_H

// Same standard headers as the device twin (device_module/shims/
// cf_custom_app_device.h), so these facilities are available in both builds.
// Trade-off: an app that does `using namespace std;` and defines its own
// `vector` or `function` now sees an ambiguous name.
#include <functional>
#include <vector>
#include <algorithm>

#include "App.h"
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
