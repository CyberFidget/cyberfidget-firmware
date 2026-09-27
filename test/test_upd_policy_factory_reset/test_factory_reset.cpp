// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#include <unity.h>
#include "FactoryResetPolicy.h"

using FactoryResetPolicy::Hold;
using FactoryResetPolicy::Refusal;
using FactoryResetPolicy::refusal;
using FactoryResetPolicy::BootStep;
using FactoryResetPolicy::bootStep;

void setUp(void) {}
void tearDown(void) {}

void test_refusal_priority_and_clear_path() {
    TEST_ASSERT_EQUAL(static_cast<int>(Refusal::None), static_cast<int>(refusal(false, false)));
    TEST_ASSERT_EQUAL(static_cast<int>(Refusal::PendingImage), static_cast<int>(refusal(true, false)));
    TEST_ASSERT_EQUAL(static_cast<int>(Refusal::ArmedSession), static_cast<int>(refusal(false, true)));
    TEST_ASSERT_EQUAL(static_cast<int>(Refusal::PendingImage), static_cast<int>(refusal(true, true)));
}

void test_hold_progress_and_completion_at_three_seconds() {
    Hold h;
    TEST_ASSERT_FALSE(h.tick(100));
    h.press(100);
    TEST_ASSERT_TRUE(h.pressed());
    TEST_ASSERT_EQUAL_UINT32(0, h.progress(100));
    TEST_ASSERT_FALSE(h.tick(3099));
    TEST_ASSERT_EQUAL_UINT32(2999, h.progress(3099));
    TEST_ASSERT_TRUE(h.tick(3100));
    TEST_ASSERT_EQUAL_UINT32(3000, h.progress(3500));
}

void test_early_release_cancels_and_next_press_starts_fresh() {
    Hold h;
    h.press(10);
    TEST_ASSERT_FALSE(h.tick(2000));
    h.release();
    TEST_ASSERT_EQUAL_UINT32(0, h.progress(4000));
    TEST_ASSERT_FALSE(h.tick(4000));
    h.press(4000);
    TEST_ASSERT_FALSE(h.tick(6999));
    TEST_ASSERT_TRUE(h.tick(7000));
}

void test_back_cancels_even_during_hold() {
    Hold h;
    h.press(0);
    h.back();
    TEST_ASSERT_FALSE(h.pressed());
    TEST_ASSERT_FALSE(h.tick(5000));
    TEST_ASSERT_EQUAL_UINT32(0, h.progress(5000));
}

void test_hold_timer_wraps_with_millis() {
    Hold h;
    h.press(0xFFFFFF00u);
    TEST_ASSERT_FALSE(h.tick(0x00000AB7u));
    TEST_ASSERT_TRUE(h.tick(0x00000AB8u));
}

void test_boot_without_mark_starts_normally() {
    TEST_ASSERT_EQUAL(static_cast<int>(BootStep::Normal), static_cast<int>(bootStep(false, false)));
    TEST_ASSERT_EQUAL(static_cast<int>(BootStep::Normal), static_cast<int>(bootStep(false, true)));
}

void test_boot_with_mark_finishes_the_cut_off_reset() {
    TEST_ASSERT_EQUAL(static_cast<int>(BootStep::Finish), static_cast<int>(bootStep(true, false)));
}

void test_boot_with_mark_waits_while_image_is_on_probation() {
    // Consistent with the reset's own refusal: never erase under a pending image.
    TEST_ASSERT_EQUAL(static_cast<int>(Refusal::PendingImage), static_cast<int>(refusal(true, false)));
    TEST_ASSERT_EQUAL(static_cast<int>(BootStep::Wait), static_cast<int>(bootStep(true, true)));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_refusal_priority_and_clear_path);
    RUN_TEST(test_hold_progress_and_completion_at_three_seconds);
    RUN_TEST(test_early_release_cancels_and_next_press_starts_fresh);
    RUN_TEST(test_back_cancels_even_during_hold);
    RUN_TEST(test_hold_timer_wraps_with_millis);
    RUN_TEST(test_boot_without_mark_starts_normally);
    RUN_TEST(test_boot_with_mark_finishes_the_cut_off_reset);
    RUN_TEST(test_boot_with_mark_waits_while_image_is_on_probation);
    return UNITY_END();
}
