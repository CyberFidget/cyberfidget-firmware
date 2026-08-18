// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#include "SDManager.h"

#include <Arduino.h>
#include <SD.h>
#include <SPI.h>

namespace SDManager {
namespace {

bool s_mounted = false;

void parkPins() {
    pinMode(kPinClock, INPUT);
    pinMode(kPinCs, INPUT);
    pinMode(kPinMosi, INPUT);
    pinMode(kPinMiso, INPUT);
}

} // namespace

bool mount() {
    if (s_mounted) return true;

    SPI.begin(kPinClock, kPinMiso, kPinMosi, kPinCs);
    s_mounted = SD.begin(kPinCs);
    if (!s_mounted) {
        release();
    }
    return s_mounted;
}

void release() {
    SD.end();
    SPI.end();
    parkPins();
    s_mounted = false;
}

void releaseForSleep() {
    release();
}

bool isMounted() {
    return s_mounted;
}

} // namespace SDManager
