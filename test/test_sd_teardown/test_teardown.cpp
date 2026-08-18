// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#include <unity.h>

#include "Arduino.h"
#include "HALMock.h"
#include "SDManager.h"

namespace {

void assertSdPinsAreInputs() {
    TEST_ASSERT_EQUAL_INT(INPUT, HALMock::pinModeState(SDManager::kPinClock));
    TEST_ASSERT_EQUAL_INT(INPUT, HALMock::pinModeState(SDManager::kPinCs));
    TEST_ASSERT_EQUAL_INT(INPUT, HALMock::pinModeState(SDManager::kPinMosi));
    TEST_ASSERT_EQUAL_INT(INPUT, HALMock::pinModeState(SDManager::kPinMiso));
}

void assertNoCsHighAfterLastSpiEnd() {
    size_t teardownIndex = HALMock::callCount();
    for (size_t i = 0; i < HALMock::callCount(); ++i) {
        if (HALMock::callAt(i).type == HALMock::CallType::SpiEnd) {
            teardownIndex = i;
        }
    }
    TEST_ASSERT_LESS_THAN(HALMock::callCount(), teardownIndex);
    for (size_t i = teardownIndex + 1; i < HALMock::callCount(); ++i) {
        const HALMock::Call& call = HALMock::callAt(i);
        TEST_ASSERT_FALSE(call.type == HALMock::CallType::DigitalWrite &&
                          call.pin == SDManager::kPinCs && call.value == HIGH);
    }
}

} // namespace

void setUp(void) {
    SDManager::release();
    HALMock::resetHardware();
}

void tearDown(void) {
    SDManager::release();
}

void test_failed_mount_parks_all_sd_pins(void) {
    HALMock::setCardPresent(false);
    TEST_ASSERT_FALSE(SDManager::mount());
    assertSdPinsAreInputs();
    assertNoCsHighAfterLastSpiEnd();
}

void test_app_exit_release_parks_all_sd_pins(void) {
    HALMock::setCardPresent(true);
    TEST_ASSERT_TRUE(SDManager::mount());
    SDManager::release();
    assertSdPinsAreInputs();
    assertNoCsHighAfterLastSpiEnd();
}

void test_release_is_safe_without_a_mount(void) {
    SDManager::release();
    assertSdPinsAreInputs();
    assertNoCsHighAfterLastSpiEnd();
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_failed_mount_parks_all_sd_pins);
    RUN_TEST(test_app_exit_release_parks_all_sd_pins);
    RUN_TEST(test_release_is_safe_without_a_mount);
    return UNITY_END();
}
