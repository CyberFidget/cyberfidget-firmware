// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#include <unity.h>

#include "UvloLogic.h"

void test_sleep_threshold_boundaries(void) {
    TEST_ASSERT_EQUAL_INT(
        (int)UvloLogic::SleepDecision::Shutdown,
        (int)UvloLogic::decideSleep(CF_UVLO_SLEEP_THRESHOLD_MV - 1, true));
    TEST_ASSERT_EQUAL_INT(
        (int)UvloLogic::SleepDecision::Resleep,
        (int)UvloLogic::decideSleep(CF_UVLO_SLEEP_THRESHOLD_MV, true));
    TEST_ASSERT_EQUAL_INT(
        (int)UvloLogic::SleepDecision::Resleep,
        (int)UvloLogic::decideSleep(CF_UVLO_SLEEP_THRESHOLD_MV + 1, true));
}

void test_sleep_rejects_implausible_samples(void) {
    TEST_ASSERT_EQUAL_INT(
        (int)UvloLogic::SleepDecision::Resleep,
        (int)UvloLogic::decideSleep(0, false));
    TEST_ASSERT_EQUAL_INT(
        (int)UvloLogic::SleepDecision::Resleep,
        (int)UvloLogic::decideSleep(-1, false));
    TEST_ASSERT_EQUAL_INT(
        (int)UvloLogic::SleepDecision::Resleep,
        (int)UvloLogic::decideSleep(5000, false));
}

void test_runtime_brief_sag_does_not_shutdown(void) {
    UvloLogic::RuntimeDebounce guard;
    TEST_ASSERT_FALSE(guard.feed(CF_UVLO_RUNTIME_THRESHOLD_MV - 1, 1000, true));
    TEST_ASSERT_FALSE(guard.feed(CF_UVLO_RUNTIME_THRESHOLD_MV - 1,
                                 1000 + CF_UVLO_RUNTIME_DEBOUNCE_MS - 1, true));
    TEST_ASSERT_FALSE(guard.feed(CF_UVLO_RUNTIME_THRESHOLD_MV, 4000, true));
}

void test_runtime_sustained_sag_shuts_down_at_window(void) {
    UvloLogic::RuntimeDebounce guard;
    TEST_ASSERT_FALSE(guard.feed(CF_UVLO_RUNTIME_THRESHOLD_MV - 1, 500, true));
    TEST_ASSERT_FALSE(guard.feed(CF_UVLO_RUNTIME_THRESHOLD_MV - 1,
                                 500 + CF_UVLO_RUNTIME_DEBOUNCE_MS - 1, true));
    TEST_ASSERT_TRUE(guard.feed(CF_UVLO_RUNTIME_THRESHOLD_MV - 1,
                                500 + CF_UVLO_RUNTIME_DEBOUNCE_MS, true));
}

void test_runtime_recovery_mid_window_restarts_debounce(void) {
    UvloLogic::RuntimeDebounce guard;
    TEST_ASSERT_FALSE(guard.feed(CF_UVLO_RUNTIME_THRESHOLD_MV - 1, 100, true));
    TEST_ASSERT_FALSE(guard.feed(CF_UVLO_RUNTIME_THRESHOLD_MV, 2100, true));
    TEST_ASSERT_FALSE(guard.feed(CF_UVLO_RUNTIME_THRESHOLD_MV - 1, 2200, true));
    TEST_ASSERT_FALSE(guard.feed(CF_UVLO_RUNTIME_THRESHOLD_MV - 1,
                                 2200 + CF_UVLO_RUNTIME_DEBOUNCE_MS - 1, true));
    TEST_ASSERT_TRUE(guard.feed(CF_UVLO_RUNTIME_THRESHOLD_MV - 1,
                                2200 + CF_UVLO_RUNTIME_DEBOUNCE_MS, true));
}

void test_runtime_implausible_sample_resets_debounce(void) {
    UvloLogic::RuntimeDebounce guard;
    TEST_ASSERT_FALSE(guard.feed(CF_UVLO_RUNTIME_THRESHOLD_MV - 1, 100, true));
    TEST_ASSERT_FALSE(guard.feed(-1, 2100, false));
    TEST_ASSERT_FALSE(guard.feed(CF_UVLO_RUNTIME_THRESHOLD_MV - 1, 2200, true));
    TEST_ASSERT_FALSE(guard.feed(CF_UVLO_RUNTIME_THRESHOLD_MV - 1,
                                 2200 + CF_UVLO_RUNTIME_DEBOUNCE_MS - 1, true));
}

void test_check_interval_converts_to_64_bit_microseconds(void) {
    TEST_ASSERT_EQUAL_UINT64(3600000000ULL, UvloLogic::checkIntervalUs());
    // The default 3600s happens to fit in 32 bits (4295s would not); what
    // matters is that the conversion is performed and returned at 64 bits.
    TEST_ASSERT_EQUAL_UINT(8, (unsigned)sizeof(UvloLogic::checkIntervalUs()));
    static_assert(sizeof(UvloLogic::checkIntervalUs()) == sizeof(uint64_t),
                  "interval conversion must not truncate to 32 bits");
}

void setUp(void) {}
void tearDown(void) {}

int main(int /*argc*/, char** /*argv*/) {
    UNITY_BEGIN();
    RUN_TEST(test_sleep_threshold_boundaries);
    RUN_TEST(test_sleep_rejects_implausible_samples);
    RUN_TEST(test_runtime_brief_sag_does_not_shutdown);
    RUN_TEST(test_runtime_sustained_sag_shuts_down_at_window);
    RUN_TEST(test_runtime_recovery_mid_window_restarts_debounce);
    RUN_TEST(test_runtime_implausible_sample_resets_debounce);
    RUN_TEST(test_check_interval_converts_to_64_bit_microseconds);
    return UNITY_END();
}
