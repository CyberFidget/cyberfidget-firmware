// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC
//
// Guest-side globals shim: the subset of lib/Globals/globals.h that apps use.

#ifndef GLOBALS_H  // same guard as the real header — must shadow it
#define GLOBALS_H

#include "Arduino.h"
#include "HAL.h"
#include "esp_log.h"

// Refreshed from cf_millis() at the top of every guest entry point.
extern unsigned long millis_NOW;

// Guest-local; the glue forwards bumps of this to the host via cf_keepalive()
// after each update so the device's deep-sleep timer really resets.
extern unsigned long millis_APP_LASTINTERACTION;

extern const char* TAG_MAIN;

#endif  // GLOBALS_H
