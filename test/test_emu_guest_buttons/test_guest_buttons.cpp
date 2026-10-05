// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2026 Dismo Industries LLC
//
// The device-module ButtonManager shim reconstructs press duration and
// pressed state from event delivery times (the host forwards only index and
// event type). These cases pin the native ButtonManager's semantics.
#include <unity.h>
#include "../../wasm/device_module/shims/ButtonManager.h"

static uint32_t g_now = 0;
extern "C" uint32_t cf_millis(void) { return g_now; }

static ButtonEvent g_last;
static int g_calls = 0;
static void record(const ButtonEvent& e) { g_last = e; ++g_calls; }

void setUp() { g_now = 1000; g_calls = 0; g_last = ButtonEvent{-1, ButtonEvent_None, 12345}; }
void tearDown() {}

static void test_pressed_has_zero_duration_and_latches_state() {
    ButtonManager bm; bm.registerCallback(5, record);
    bm.dispatch(5, ButtonEvent_Pressed);
    TEST_ASSERT_EQUAL_INT(1, g_calls);
    TEST_ASSERT_EQUAL_INT(ButtonEvent_Pressed, g_last.eventType);
    TEST_ASSERT_EQUAL_UINT32(0, g_last.duration);
    TEST_ASSERT_TRUE(bm.isPressed(5));
    TEST_ASSERT_FALSE(bm.isPressed(4));
}

static void test_released_carries_press_length() {
    ButtonManager bm; bm.registerCallback(5, record);
    bm.dispatch(5, ButtonEvent_Pressed);
    g_now += 750;
    bm.dispatch(5, ButtonEvent_Released);
    TEST_ASSERT_EQUAL_INT(ButtonEvent_Released, g_last.eventType);
    TEST_ASSERT_EQUAL_UINT32(750, g_last.duration);
    TEST_ASSERT_FALSE(bm.isPressed(5));
}

static void test_tap_versus_long_press() {
    ButtonManager bm; bm.registerCallback(5, record);
    bm.dispatch(5, ButtonEvent_Pressed); g_now += 120; bm.dispatch(5, ButtonEvent_Released);
    TEST_ASSERT_TRUE(g_last.duration < 600);
    bm.dispatch(5, ButtonEvent_Pressed); g_now += 1500; bm.dispatch(5, ButtonEvent_Released);
    TEST_ASSERT_TRUE(g_last.duration > 600);
}

static void test_held_reports_hold_time_never_below_threshold() {
    ButtonManager bm; bm.registerCallback(2, record);
    bm.dispatch(2, ButtonEvent_Pressed);
    g_now += 1500;
    bm.dispatch(2, ButtonEvent_Held);
    TEST_ASSERT_EQUAL_INT(ButtonEvent_Held, g_last.eventType);
    TEST_ASSERT_EQUAL_UINT32(1500, g_last.duration);
    TEST_ASSERT_TRUE(bm.isPressed(2));  // still down while held
    // Delivery jitter can make the measured time a little short of the
    // host's threshold; Held must still report at least the threshold.
    bm.dispatch(3, ButtonEvent_Pressed);
    g_now += 1490;
    bm.registerCallback(3, record);
    bm.dispatch(3, ButtonEvent_Held);
    TEST_ASSERT_EQUAL_UINT32(ButtonManager::kHoldThresholdMs, g_last.duration);
}

static void test_state_tracked_without_a_callback() {
    ButtonManager bm;  // polling app: no callbacks registered
    bm.dispatch(0, ButtonEvent_Pressed);
    TEST_ASSERT_TRUE(bm.isPressed(0));
    bm.dispatch(0, ButtonEvent_Released);
    TEST_ASSERT_FALSE(bm.isPressed(0));
    TEST_ASSERT_EQUAL_INT(0, g_calls);
}

static void test_release_without_press_reports_zero() {
    // App launched while the button was already down: only Released arrives.
    ButtonManager bm; bm.registerCallback(4, record);
    g_now = 50000;
    bm.dispatch(4, ButtonEvent_Released);
    TEST_ASSERT_EQUAL_UINT32(0, g_last.duration);
    TEST_ASSERT_FALSE(bm.isPressed(4));
}

static void test_out_of_range_index_is_ignored() {
    ButtonManager bm; bm.registerCallback(0, record);
    bm.dispatch(-1, ButtonEvent_Pressed);
    bm.dispatch(6, ButtonEvent_Pressed);
    TEST_ASSERT_EQUAL_INT(0, g_calls);
    TEST_ASSERT_FALSE(bm.isPressed(-1));
    TEST_ASSERT_FALSE(bm.isPressed(6));
}

static void test_millis_wraparound() {
    ButtonManager bm; bm.registerCallback(1, record);
    g_now = 0xFFFFFF00u;
    bm.dispatch(1, ButtonEvent_Pressed);
    g_now = 0x00000100u;  // wrapped
    bm.dispatch(1, ButtonEvent_Released);
    TEST_ASSERT_EQUAL_UINT32(0x200u, (uint32_t)g_last.duration);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_pressed_has_zero_duration_and_latches_state);
    RUN_TEST(test_released_carries_press_length);
    RUN_TEST(test_tap_versus_long_press);
    RUN_TEST(test_held_reports_hold_time_never_below_threshold);
    RUN_TEST(test_state_tracked_without_a_callback);
    RUN_TEST(test_release_without_press_reports_zero);
    RUN_TEST(test_out_of_range_index_is_ignored);
    RUN_TEST(test_millis_wraparound);
    return UNITY_END();
}
