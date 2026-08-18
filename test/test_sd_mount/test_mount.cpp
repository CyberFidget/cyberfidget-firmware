// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#include <unity.h>

#include "HALMock.h"
#include "SDManager.h"

void setUp(void) {
    SDManager::release();
    HALMock::resetHardware();
}

void tearDown(void) {
    SDManager::release();
}

void test_present_card_mounts_after_expected_init_sequence(void) {
    HALMock::setCardPresent(true);

    TEST_ASSERT_TRUE(SDManager::mount());
    TEST_ASSERT_TRUE(SDManager::isMounted());
    TEST_ASSERT_GREATER_OR_EQUAL(2, HALMock::callCount());

    const HALMock::Call& spi = HALMock::callAt(0);
    TEST_ASSERT_EQUAL_INT((int)HALMock::CallType::SpiBegin, (int)spi.type);
    TEST_ASSERT_EQUAL_INT(SDManager::kPinClock, spi.pin);
    TEST_ASSERT_EQUAL_INT(SDManager::kPinMiso, spi.value);
    TEST_ASSERT_EQUAL_INT(SDManager::kPinMosi, spi.arg2);
    TEST_ASSERT_EQUAL_INT(SDManager::kPinCs, spi.arg3);

    const HALMock::Call& sd = HALMock::callAt(1);
    TEST_ASSERT_EQUAL_INT((int)HALMock::CallType::SdBegin, (int)sd.type);
    TEST_ASSERT_EQUAL_INT(SDManager::kPinCs, sd.pin);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_present_card_mounts_after_expected_init_sequence);
    return UNITY_END();
}
