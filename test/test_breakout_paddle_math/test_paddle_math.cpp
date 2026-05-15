// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

// lib/BreakoutGame/test/test_paddle_math.cpp
//
// Pure-logic tests for BreakoutMath's paddle physics functions.
// No HAL, no display, no audio — just math.

#include <unity.h>
#include <math.h>

#include "BreakoutMath.h"

// Forward declaration — definition below.
static bool approxEqual(float a, float b);

// Center hit → ball goes near straight up (small VX, negative VY).
void test_reflect_center_hit_goes_straight_up(void) {
    float outVX = 0.0f, outVY = 0.0f;
    BreakoutMath::reflectFromPaddle(/*hitPos=*/0.0f, /*vxIn=*/1.0f, /*vyIn=*/1.0f,
                                    /*currentLevel=*/0, outVX, outVY);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, outVX);
    TEST_ASSERT_TRUE_MESSAGE(outVY < 0.0f, "ball must travel upward after paddle hit");
}

// Left-edge hit → ball goes upper-left.
void test_reflect_left_edge_goes_upper_left(void) {
    float outVX = 0.0f, outVY = 0.0f;
    BreakoutMath::reflectFromPaddle(/*hitPos=*/-1.0f, /*vxIn=*/0.5f, /*vyIn=*/0.5f,
                                    /*currentLevel=*/0, outVX, outVY);
    TEST_ASSERT_TRUE(outVX < 0.0f);
    TEST_ASSERT_TRUE(outVY < 0.0f);
}

// Right-edge hit → ball goes upper-right.
void test_reflect_right_edge_goes_upper_right(void) {
    float outVX = 0.0f, outVY = 0.0f;
    BreakoutMath::reflectFromPaddle(/*hitPos=*/+1.0f, /*vxIn=*/0.5f, /*vyIn=*/0.5f,
                                    /*currentLevel=*/0, outVX, outVY);
    TEST_ASSERT_TRUE(outVX > 0.0f);
    TEST_ASSERT_TRUE(outVY < 0.0f);
}

// Speed magnitude preserved across the reflection.
void test_reflect_preserves_speed(void) {
    const float vxIn = 0.7f, vyIn = 0.5f;
    const float speedIn = sqrtf(vxIn * vxIn + vyIn * vyIn);
    // Sample at 11 different hit positions.
    for (int i = -5; i <= 5; i++) {
        const float hitPos = i / 5.0f;
        float outVX = 0.0f, outVY = 0.0f;
        BreakoutMath::reflectFromPaddle(hitPos, vxIn, vyIn, 0, outVX, outVY);
        const float speedOut = sqrtf(outVX * outVX + outVY * outVY);
        TEST_ASSERT_FLOAT_WITHIN(0.001f, speedIn, speedOut);
    }
}

// Textured deflection is deterministic: same input → same output.
void test_reflect_texture_deterministic(void) {
    const int texturedLevel = BreakoutMath::TEXTURE_FROM_LEVEL;
    float ax = 0, ay = 0, bx = 0, by = 0;
    BreakoutMath::reflectFromPaddle(0.3f, 1.0f, 1.0f, texturedLevel, ax, ay);
    BreakoutMath::reflectFromPaddle(0.3f, 1.0f, 1.0f, texturedLevel, bx, by);
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, ax, bx);
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, ay, by);
}

// Textured deflection differs from untextured at the same hitPos.
// (Texture only activates at currentLevel >= TEXTURE_FROM_LEVEL.)
void test_reflect_texture_differs_from_smooth(void) {
    const int smoothLevel   = BreakoutMath::TEXTURE_FROM_LEVEL - 1;
    const int texturedLevel = BreakoutMath::TEXTURE_FROM_LEVEL;
    float sx = 0, sy = 0, tx = 0, ty = 0;
    // Pick a hitPos where sin(hitPos * 10) is nonzero (avoid coincidence at 0).
    BreakoutMath::reflectFromPaddle(0.3f, 1.0f, 1.0f, smoothLevel,   sx, sy);
    BreakoutMath::reflectFromPaddle(0.3f, 1.0f, 1.0f, texturedLevel, tx, ty);
    // At least one of the velocity components must differ.
    const bool differs = !approxEqual(sx, tx) || !approxEqual(sy, ty);
    TEST_ASSERT_TRUE_MESSAGE(differs, "textured deflection should differ from smooth at hitPos=0.3");
}

static bool approxEqual(float a, float b) {
    float d = a - b;
    if (d < 0) d = -d;
    return d < 1e-4f;
}

// computeHitPos sanity: center of ball over center of paddle → 0.
void test_computeHitPos_center(void) {
    // 20-px paddle from x=54 → center at 64. Ball at x=63 with size 2 → ball center at 64.
    const float hp = BreakoutMath::computeHitPos(63.0f, 2, 54.0f, 20);
    TEST_ASSERT_FLOAT_WITHIN(0.05f, 0.0f, hp);
}

// computeHitPos: ball at left edge → ~-1
void test_computeHitPos_left_edge(void) {
    // Paddle from x=54, width 20 → left edge at x=54. Ball ball center at 54 → hitPos = -1
    const float hp = BreakoutMath::computeHitPos(53.0f, 2, 54.0f, 20);
    TEST_ASSERT_FLOAT_WITHIN(0.05f, -1.0f, hp);
}

// computeHitPos: ball at right edge → ~+1
void test_computeHitPos_right_edge(void) {
    // Paddle right edge at x=74. Ball center near 74 → hitPos ≈ +1
    const float hp = BreakoutMath::computeHitPos(73.0f, 2, 54.0f, 20);
    TEST_ASSERT_FLOAT_WITHIN(0.05f, +1.0f, hp);
}

void setUp(void)    {}
void tearDown(void) {}

int main(int /*argc*/, char** /*argv*/) {
    UNITY_BEGIN();
    RUN_TEST(test_reflect_center_hit_goes_straight_up);
    RUN_TEST(test_reflect_left_edge_goes_upper_left);
    RUN_TEST(test_reflect_right_edge_goes_upper_right);
    RUN_TEST(test_reflect_preserves_speed);
    RUN_TEST(test_reflect_texture_deterministic);
    RUN_TEST(test_reflect_texture_differs_from_smooth);
    RUN_TEST(test_computeHitPos_center);
    RUN_TEST(test_computeHitPos_left_edge);
    RUN_TEST(test_computeHitPos_right_edge);
    return UNITY_END();
}
