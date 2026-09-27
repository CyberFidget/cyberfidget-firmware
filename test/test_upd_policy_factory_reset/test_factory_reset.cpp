// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#include <unity.h>
#include "FactoryResetPolicy.h"

using FactoryResetPolicy::Hold;
using FactoryResetPolicy::Refusal;
using FactoryResetPolicy::refusal;
using FactoryResetPolicy::BootStep;
using FactoryResetPolicy::bootStep;
using FactoryResetPolicy::MarkRead;
using FactoryResetPolicy::markConfirmed;
using FactoryResetPolicy::finishFormats;
using FactoryResetPolicy::FinishResult;
using FactoryResetPolicy::finishResult;

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

void test_unreadable_mark_is_its_own_step_not_clear_or_finish() {
    TEST_ASSERT_EQUAL(static_cast<int>(BootStep::Unreadable), static_cast<int>(bootStep(MarkRead::Error, false)));
    TEST_ASSERT_EQUAL(static_cast<int>(BootStep::Unreadable), static_cast<int>(bootStep(MarkRead::Error, true)));
    TEST_ASSERT_EQUAL(static_cast<int>(BootStep::Normal), static_cast<int>(bootStep(MarkRead::Clear, false)));
    TEST_ASSERT_EQUAL(static_cast<int>(BootStep::Finish), static_cast<int>(bootStep(MarkRead::Set, false)));
    TEST_ASSERT_EQUAL(static_cast<int>(BootStep::Wait), static_cast<int>(bootStep(MarkRead::Set, true)));
}

void test_reset_starts_only_when_mark_written_and_read_back() {
    TEST_ASSERT_TRUE(markConfirmed(true, MarkRead::Set));
    TEST_ASSERT_FALSE(markConfirmed(false, MarkRead::Set));    // write reported failure
    TEST_ASSERT_FALSE(markConfirmed(true, MarkRead::Clear));   // write "ok" but not there
    TEST_ASSERT_FALSE(markConfirmed(true, MarkRead::Error));   // cannot read it back
    TEST_ASSERT_FALSE(markConfirmed(false, MarkRead::Error));
}

void test_startup_finish_formats_twice_then_skips() {
    TEST_ASSERT_TRUE(finishFormats(1));
    TEST_ASSERT_TRUE(finishFormats(2));
    TEST_ASSERT_FALSE(finishFormats(3));
    TEST_ASSERT_FALSE(finishFormats(100));
}

void test_finish_without_apps_erase_is_partial_never_done() {
    TEST_ASSERT_EQUAL(static_cast<int>(FinishResult::Done), static_cast<int>(finishResult(true)));
    // Covers a failed format and the skipped (anti-loop) try alike.
    TEST_ASSERT_EQUAL(static_cast<int>(FinishResult::Partial), static_cast<int>(finishResult(false)));
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
    RUN_TEST(test_unreadable_mark_is_its_own_step_not_clear_or_finish);
    RUN_TEST(test_reset_starts_only_when_mark_written_and_read_back);
    RUN_TEST(test_startup_finish_formats_twice_then_skips);
    RUN_TEST(test_finish_without_apps_erase_is_partial_never_done);
    return UNITY_END();
}
