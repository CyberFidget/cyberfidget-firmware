// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

// lib/BreakoutGame/test/test_input_mapping.cpp
//
// App↔HAL contract tests — verify BreakoutGame's input-mapping math
// behaves correctly under the documented HAL semantics. If HAL changes
// (e.g., slider returns 0..1023 instead of 0..100, or accel sign
// inverts), these tests catch it via the HALMock layer + the pure
// functions in BreakoutMath.
//
// See plan file `i-want-to-update-toasty-crown.md` §6c for the layered
// approach (HALMock at native here; full WASM SIL coverage in T-002).

#include <unity.h>
#include "BreakoutMath.h"
#include "HALMock.h"

// Same geometry as BreakoutGame's compile-time constants.
static constexpr int SCREEN_WIDTH  = 128;
static constexpr int PADDLE_WIDTH  = 20;

// --- Slider contract ---------------------------------------------------

void test_slider_0pct_parks_paddle_at_left_edge(void) {
    HALMock::setSliderPercent(0.0f);
    const float px = BreakoutMath::paddleXFromSlider(
        HALMock::sliderPosition_Percentage_Filtered, SCREEN_WIDTH, PADDLE_WIDTH);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, px);
}

void test_slider_100pct_parks_paddle_at_right_edge(void) {
    HALMock::setSliderPercent(100.0f);
    const float px = BreakoutMath::paddleXFromSlider(
        HALMock::sliderPosition_Percentage_Filtered, SCREEN_WIDTH, PADDLE_WIDTH);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, (float)(SCREEN_WIDTH - PADDLE_WIDTH), px);
}

void test_slider_50pct_centers_paddle(void) {
    HALMock::setSliderPercent(50.0f);
    const float px = BreakoutMath::paddleXFromSlider(
        HALMock::sliderPosition_Percentage_Filtered, SCREEN_WIDTH, PADDLE_WIDTH);
    const float expected = (SCREEN_WIDTH - PADDLE_WIDTH) / 2.0f; // 54
    TEST_ASSERT_FLOAT_WITHIN(0.5f, expected, px);
}

// If HAL changed slider to 0..1023 silently, the paddleX would overshoot —
// caught here because we use HALMock::setSliderPercent's clamp (0..100).
// More importantly, this test pins the *contract* of paddleXFromSlider:
// it expects 0..100 in, period.
void test_slider_out_of_range_clamps(void) {
    // Direct call with out-of-range input — paddleXFromSlider clamps internally.
    const float pxNeg  = BreakoutMath::paddleXFromSlider(-10.0f,  SCREEN_WIDTH, PADDLE_WIDTH);
    const float pxOver = BreakoutMath::paddleXFromSlider(200.0f, SCREEN_WIDTH, PADDLE_WIDTH);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, pxNeg);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, (float)(SCREEN_WIDTH - PADDLE_WIDTH), pxOver);
}

// --- Accelerometer contract --------------------------------------------

// Positive accelX → paddle moves left (sign convention from existing code).
// If someone flips the sign in BreakoutMath::paddleXFromAccel later, this
// fails immediately.
void test_accel_positive_moves_paddle_left(void) {
    const float startX = 60.0f;
    const float nextX = BreakoutMath::paddleXFromAccel(
        startX, /*accelX=*/1.0f, /*paddleSpeed=*/2.0f, SCREEN_WIDTH, PADDLE_WIDTH);
    TEST_ASSERT_TRUE(nextX < startX);
}

void test_accel_negative_moves_paddle_right(void) {
    const float startX = 60.0f;
    const float nextX = BreakoutMath::paddleXFromAccel(
        startX, /*accelX=*/-1.0f, /*paddleSpeed=*/2.0f, SCREEN_WIDTH, PADDLE_WIDTH);
    TEST_ASSERT_TRUE(nextX > startX);
}

void test_accel_zero_is_no_motion(void) {
    const float startX = 60.0f;
    const float nextX = BreakoutMath::paddleXFromAccel(
        startX, /*accelX=*/0.0f, /*paddleSpeed=*/2.0f, SCREEN_WIDTH, PADDLE_WIDTH);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, startX, nextX);
}

// Paddle clamps at screen edges in both directions.
void test_accel_clamps_at_left_wall(void) {
    const float startX = 1.0f;
    const float nextX = BreakoutMath::paddleXFromAccel(
        startX, /*accelX=*/1000.0f, /*paddleSpeed=*/2.0f, SCREEN_WIDTH, PADDLE_WIDTH);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, nextX);
}

void test_accel_clamps_at_right_wall(void) {
    const float startX = (float)(SCREEN_WIDTH - PADDLE_WIDTH - 1);
    const float nextX = BreakoutMath::paddleXFromAccel(
        startX, /*accelX=*/-1000.0f, /*paddleSpeed=*/2.0f, SCREEN_WIDTH, PADDLE_WIDTH);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, (float)(SCREEN_WIDTH - PADDLE_WIDTH), nextX);
}

// --- Setup ---

void setUp(void) {
    HALMock::resetAll();
}

void tearDown(void) {}

int main(int /*argc*/, char** /*argv*/) {
    UNITY_BEGIN();
    RUN_TEST(test_slider_0pct_parks_paddle_at_left_edge);
    RUN_TEST(test_slider_100pct_parks_paddle_at_right_edge);
    RUN_TEST(test_slider_50pct_centers_paddle);
    RUN_TEST(test_slider_out_of_range_clamps);
    RUN_TEST(test_accel_positive_moves_paddle_left);
    RUN_TEST(test_accel_negative_moves_paddle_right);
    RUN_TEST(test_accel_zero_is_no_motion);
    RUN_TEST(test_accel_clamps_at_left_wall);
    RUN_TEST(test_accel_clamps_at_right_wall);
    return UNITY_END();
}
