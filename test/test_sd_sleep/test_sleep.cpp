// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#include <unity.h>

#include "Arduino.h"
#include "HALMock.h"
#include "SDManager.h"

void setUp(void) {
    SDManager::release();
    HALMock::resetHardware();
}

void tearDown(void) {
    SDManager::release();
}

void test_sleep_release_parks_bus_before_hold_enable(void) {
    pinMode(SDManager::kPinClock, OUTPUT);
    pinMode(SDManager::kPinCs, OUTPUT);
    pinMode(SDManager::kPinMosi, OUTPUT);
    pinMode(SDManager::kPinMiso, OUTPUT);

    SDManager::releaseForSleep();
    gpio_deep_sleep_hold_en();

    size_t holdIndex = HALMock::callCount();
    for (size_t i = 0; i < HALMock::callCount(); ++i) {
        if (HALMock::callAt(i).type == HALMock::CallType::DeepSleepHoldEnable) {
            holdIndex = i;
            break;
        }
    }
    TEST_ASSERT_LESS_THAN(HALMock::callCount(), holdIndex);

    const int pins[] = {
        SDManager::kPinClock, SDManager::kPinCs,
        SDManager::kPinMosi, SDManager::kPinMiso
    };
    for (int pin : pins) {
        size_t inputIndex = HALMock::callCount();
        for (size_t i = 0; i < holdIndex; ++i) {
            const HALMock::Call& call = HALMock::callAt(i);
            if (call.type == HALMock::CallType::PinMode &&
                call.pin == pin && call.value == INPUT) {
                inputIndex = i;
            }
        }
        TEST_ASSERT_LESS_THAN(holdIndex, inputIndex);
    }
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_sleep_release_parks_bus_before_hold_enable);
    return UNITY_END();
}
