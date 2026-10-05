// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2026 Dismo Industries LLC
//
// The device-module ButtonManager shim reconstructs press duration and
// pressed state from event delivery times (the host forwards only index and
// event type), records a frame's events before running any callback, and
// runs callbacks from flushCallbacks(). These cases pin the native
// ButtonManager's semantics.
#include <unity.h>
#include "../../wasm/device_module/shims/ButtonManager.h"

static uint32_t g_now = 0;
extern "C" uint32_t cf_millis(void) { return g_now; }

static ButtonManager* g_bm = nullptr;
static ButtonEvent g_last;
static int g_calls = 0;
static bool g_otherDownInCallback = false;
static void record(const ButtonEvent& e) { g_last = e; ++g_calls; }
static void recordChord(const ButtonEvent& e) {
    g_last = e; ++g_calls;
    g_otherDownInCallback = g_bm && g_bm->isPressed(1);
}
static void slowCallback(const ButtonEvent& e) { g_last = e; ++g_calls; g_now += 1000; }

static void send(ButtonManager& bm, int idx, int type) { bm.dispatch(idx, type); bm.flushCallbacks(); }

void setUp() { g_now = 1000; g_calls = 0; g_bm = nullptr; g_otherDownInCallback = false; g_last = ButtonEvent{-1, ButtonEvent_None, 12345}; }
void tearDown() {}

static void test_pressed_has_zero_duration_and_latches_state() {
    ButtonManager bm; bm.registerCallback(5, record);
    send(bm, 5, ButtonEvent_Pressed);
    TEST_ASSERT_EQUAL_INT(1, g_calls);
    TEST_ASSERT_EQUAL_INT(ButtonEvent_Pressed, g_last.eventType);
    TEST_ASSERT_EQUAL_UINT32(0, g_last.duration);
    TEST_ASSERT_TRUE(bm.isPressed(5));
    TEST_ASSERT_FALSE(bm.isPressed(4));
}

static void test_released_carries_press_length() {
    ButtonManager bm; bm.registerCallback(5, record);
    send(bm, 5, ButtonEvent_Pressed);
    g_now += 750;
    send(bm, 5, ButtonEvent_Released);
    TEST_ASSERT_EQUAL_INT(ButtonEvent_Released, g_last.eventType);
    TEST_ASSERT_EQUAL_UINT32(750, g_last.duration);
    TEST_ASSERT_FALSE(bm.isPressed(5));
}

static void test_tap_versus_long_press() {
    ButtonManager bm; bm.registerCallback(5, record);
    send(bm, 5, ButtonEvent_Pressed); g_now += 120; send(bm, 5, ButtonEvent_Released);
    TEST_ASSERT_TRUE(g_last.duration < 600);
    send(bm, 5, ButtonEvent_Pressed); g_now += 1700; send(bm, 5, ButtonEvent_Released);
    TEST_ASSERT_TRUE(g_last.duration > 600);
}

static void test_held_reports_hold_time_never_below_threshold() {
    ButtonManager bm; bm.registerCallback(2, record); bm.registerCallback(3, record);
    send(bm, 2, ButtonEvent_Pressed);
    g_now += 1700;
    send(bm, 2, ButtonEvent_Held);
    TEST_ASSERT_EQUAL_INT(ButtonEvent_Held, g_last.eventType);
    TEST_ASSERT_EQUAL_UINT32(1700, g_last.duration);
    TEST_ASSERT_TRUE(bm.isPressed(2));
    // Delivery jitter can make the measured time a little short of the host's
    // threshold; Held must still report at least the threshold.
    send(bm, 3, ButtonEvent_Pressed);
    g_now += 1490;
    send(bm, 3, ButtonEvent_Held);
    TEST_ASSERT_EQUAL_UINT32(ButtonManager::kHoldThresholdMs, g_last.duration);
}

static void test_orphan_held_latches_pressed_and_keeps_the_hold() {
    // The press happened before the module started; only Held then Released arrive.
    ButtonManager bm; bm.registerCallback(5, record);
    g_now = 20000;
    send(bm, 5, ButtonEvent_Held);
    TEST_ASSERT_TRUE(bm.isPressed(5));
    TEST_ASSERT_EQUAL_UINT32(ButtonManager::kHoldThresholdMs, g_last.duration);
    g_now += 400;
    send(bm, 5, ButtonEvent_Released);
    TEST_ASSERT_FALSE(bm.isPressed(5));
    TEST_ASSERT_EQUAL_UINT32(ButtonManager::kHoldThresholdMs + 400, g_last.duration);
}

static void test_state_tracked_without_a_callback() {
    ButtonManager bm;  // polling app: no callbacks registered
    send(bm, 0, ButtonEvent_Pressed);
    TEST_ASSERT_TRUE(bm.isPressed(0));
    send(bm, 0, ButtonEvent_Released);
    TEST_ASSERT_FALSE(bm.isPressed(0));
    TEST_ASSERT_EQUAL_INT(0, g_calls);
}

static void test_release_without_press_reports_zero() {
    ButtonManager bm; bm.registerCallback(4, record);
    g_now = 50000;
    send(bm, 4, ButtonEvent_Released);
    TEST_ASSERT_EQUAL_UINT32(0, g_last.duration);
    TEST_ASSERT_FALSE(bm.isPressed(4));
}

static void test_callbacks_run_on_flush_not_on_dispatch() {
    ButtonManager bm; bm.registerCallback(5, record);
    bm.dispatch(5, ButtonEvent_Pressed);
    TEST_ASSERT_EQUAL_INT(0, g_calls);
    TEST_ASSERT_TRUE(bm.isPressed(5));  // state is already current
    bm.flushCallbacks();
    TEST_ASSERT_EQUAL_INT(1, g_calls);
    bm.flushCallbacks();                // nothing left
    TEST_ASSERT_EQUAL_INT(1, g_calls);
}

static void test_chord_visible_inside_callback() {
    // Native scans every button before dispatching, so button 0's callback
    // sees button 1 already down when both go down in the same frame.
    ButtonManager bm; g_bm = &bm; bm.registerCallback(0, recordChord);
    bm.dispatch(0, ButtonEvent_Pressed);
    bm.dispatch(1, ButtonEvent_Pressed);
    bm.flushCallbacks();
    TEST_ASSERT_TRUE(g_otherDownInCallback);
}

static void test_slow_callback_does_not_skew_later_durations() {
    // Two presses arrive in one frame; the first callback takes a second.
    ButtonManager bm; bm.registerCallback(0, slowCallback); bm.registerCallback(1, record);
    bm.dispatch(0, ButtonEvent_Pressed);
    bm.dispatch(1, ButtonEvent_Pressed);  // recorded at 1000, before any callback
    bm.flushCallbacks();                  // slow callback advances the clock to 2000
    g_now += 1;
    send(bm, 1, ButtonEvent_Released);
    TEST_ASSERT_EQUAL_UINT32(1001, g_last.duration);
}

static void test_callbacks_keep_arrival_order() {
    ButtonManager bm; bm.registerCallback(3, record);
    bm.dispatch(3, ButtonEvent_Pressed);
    g_now += 100;
    bm.dispatch(3, ButtonEvent_Released);
    bm.flushCallbacks();
    TEST_ASSERT_EQUAL_INT(2, g_calls);
    TEST_ASSERT_EQUAL_INT(ButtonEvent_Released, g_last.eventType);
    TEST_ASSERT_EQUAL_UINT32(100, g_last.duration);
}

static void test_out_of_range_index_is_ignored() {
    ButtonManager bm; bm.registerCallback(0, record);
    send(bm, -1, ButtonEvent_Pressed);
    send(bm, 6, ButtonEvent_Pressed);
    TEST_ASSERT_EQUAL_INT(0, g_calls);
    TEST_ASSERT_FALSE(bm.isPressed(-1));
    TEST_ASSERT_FALSE(bm.isPressed(6));
}

static void test_millis_wraparound_exact() {
    ButtonManager bm; bm.registerCallback(1, record);
    g_now = 0xFFFFFF00u;
    send(bm, 1, ButtonEvent_Pressed);
    g_now = 0x00000100u;  // wrapped
    send(bm, 1, ButtonEvent_Released);
    TEST_ASSERT_EQUAL_UINT64(0x200u, (unsigned long long)g_last.duration);  // full value, no truncation
}

static void test_full_queue_drops_oldest_and_state_stays_current() {
    ButtonManager bm; bm.registerCallback(2, record);
    // 40 events: the first 8 callbacks are dropped (oldest first); the last
    // event is a Pressed, so a policy that skipped state on drop would fail.
    for (int i = 0; i < 40; ++i) { g_now += 10; bm.dispatch(2, (i % 2) ? ButtonEvent_Pressed : ButtonEvent_Released); }
    TEST_ASSERT_TRUE(bm.isPressed(2));
    bm.flushCallbacks();
    TEST_ASSERT_EQUAL_INT(32, g_calls);
    TEST_ASSERT_EQUAL_INT(ButtonEvent_Pressed, g_last.eventType);  // newest kept
}

static bool g_stop = false;
static int g_sideEffects = 0;
static void exitOnBack(const ButtonEvent& e) { ++g_calls; if (e.eventType == ButtonEvent_Released) g_stop = true; }
static void sideEffect(const ButtonEvent&) { ++g_sideEffects; }

static void test_exit_request_discards_later_callbacks() {
    ButtonManager bm; g_stop = false; g_sideEffects = 0;
    bm.registerCallback(4, exitOnBack); bm.registerCallback(5, sideEffect);
    bm.dispatch(4, ButtonEvent_Released);   // Back: the app asks to leave
    bm.dispatch(5, ButtonEvent_Pressed);    // a later event in the same frame
    bm.flushCallbacks(&g_stop);
    TEST_ASSERT_EQUAL_INT(1, g_calls);
    TEST_ASSERT_EQUAL_INT(0, g_sideEffects);
    bm.flushCallbacks(&g_stop);             // nothing resurfaces
    TEST_ASSERT_EQUAL_INT(0, g_sideEffects);
}

static void test_batched_press_held_release_keeps_the_hold() {
    // A delayed frame delivers Pressed and Held together; Released follows.
    ButtonManager bm; bm.registerCallback(5, record);
    bm.dispatch(5, ButtonEvent_Pressed);
    bm.dispatch(5, ButtonEvent_Held);
    bm.flushCallbacks();
    TEST_ASSERT_EQUAL_UINT32(ButtonManager::kHoldThresholdMs, g_last.duration);
    g_now += 100;
    send(bm, 5, ButtonEvent_Released);
    TEST_ASSERT_EQUAL_UINT32(ButtonManager::kHoldThresholdMs + 100, g_last.duration);
}

static void test_reset_clears_pending_and_pressed() {
    ButtonManager bm; bm.registerCallback(1, record);
    bm.dispatch(1, ButtonEvent_Pressed);
    bm.reset();
    TEST_ASSERT_FALSE(bm.isPressed(1));
    bm.flushCallbacks();
    TEST_ASSERT_EQUAL_INT(0, g_calls);
    TEST_ASSERT_TRUE(bm.hasCallback(1));  // registrations belong to the app, not reset
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_pressed_has_zero_duration_and_latches_state);
    RUN_TEST(test_released_carries_press_length);
    RUN_TEST(test_tap_versus_long_press);
    RUN_TEST(test_held_reports_hold_time_never_below_threshold);
    RUN_TEST(test_orphan_held_latches_pressed_and_keeps_the_hold);
    RUN_TEST(test_state_tracked_without_a_callback);
    RUN_TEST(test_release_without_press_reports_zero);
    RUN_TEST(test_callbacks_run_on_flush_not_on_dispatch);
    RUN_TEST(test_chord_visible_inside_callback);
    RUN_TEST(test_slow_callback_does_not_skew_later_durations);
    RUN_TEST(test_callbacks_keep_arrival_order);
    RUN_TEST(test_out_of_range_index_is_ignored);
    RUN_TEST(test_millis_wraparound_exact);
    RUN_TEST(test_full_queue_drops_oldest_and_state_stays_current);
    RUN_TEST(test_exit_request_discards_later_callbacks);
    RUN_TEST(test_batched_press_held_release_keeps_the_hold);
    RUN_TEST(test_reset_clears_pending_and_pressed);
    return UNITY_END();
}
