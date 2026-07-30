// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#include <unity.h>

#include "cf_gfx_rig.h"

using namespace cf::gfx;

namespace {

constexpr RigBone kBones[] = {
    {"root", -1, 4, 0, 0, 0},
    {"child", 0, 3, 4, 0, 0},
};
constexpr int16_t kRestOffsets[] = {0, 0};
constexpr RigPose kRest = {"rest", kRestOffsets, 0};
constexpr Rig kRig = {
    "hand-proof", kBones, 2, nullptr, 0, &kRest, 1, nullptr, 0};

}  // namespace

void test_degree_conversion_rounds_half_away_and_wraps(void) {
    TEST_ASSERT_EQUAL_UINT8(64, degreesToBinaryAngle(90));
    TEST_ASSERT_EQUAL_UINT8(192, degreesToBinaryAngle(-90));
    TEST_ASSERT_EQUAL_UINT8(1, degreesToBinaryAngle(1));
    TEST_ASSERT_EQUAL_UINT8(255, degreesToBinaryAngle(-1));
    TEST_ASSERT_EQUAL_UINT8(128, degreesToBinaryAngle(180));
    TEST_ASSERT_EQUAL_UINT8(128, degreesToBinaryAngle(-180));
}

void test_parent_first_quarter_turn_has_negative_coordinates(void) {
    static constexpr int16_t offsets[] = {-90, 0};
    static constexpr RigPose pose = {"up", offsets, -2};
    RigBoneTransform out[2] = {};
    TEST_ASSERT_TRUE(solveRigPose(kRig, &pose, out, 2));
    TEST_ASSERT_EQUAL_INT32(0, out[0].startX);
    TEST_ASSERT_EQUAL_INT32(-2, out[0].startY);
    TEST_ASSERT_EQUAL_INT32(0, out[0].endX);
    TEST_ASSERT_EQUAL_INT32(-6, out[0].endY);
    TEST_ASSERT_EQUAL_UINT8(192, out[0].angle);
    TEST_ASSERT_EQUAL_INT32(0, out[1].startX);
    TEST_ASSERT_EQUAL_INT32(-6, out[1].startY);
    TEST_ASSERT_EQUAL_INT32(0, out[1].endX);
    TEST_ASSERT_EQUAL_INT32(-9, out[1].endY);
    TEST_ASSERT_EQUAL_UINT8(192, out[1].angle);
}

void test_arithmetic_shift_floors_negative_rotation(void) {
    int32_t x = 0;
    int32_t y = 0;
    rotateRigOffset(1, 0, 96, x, y);
    // cos(135deg) and sin(135deg) are +/-11585 Q14:
    // -11585 >> 14 floors to -1; +11585 >> 14 truncates via shift to 0.
    TEST_ASSERT_EQUAL_INT32(-1, x);
    TEST_ASSERT_EQUAL_INT32(0, y);
}

void test_opposite_angle_blend_tie_takes_negative_path(void) {
    TEST_ASSERT_EQUAL_UINT8(0, blendBinaryAngle(0, 128, 0));
    TEST_ASSERT_EQUAL_UINT8(192, blendBinaryAngle(0, 128, 128));
    TEST_ASSERT_EQUAL_UINT8(128, blendBinaryAngle(0, 128, 255));
}

void test_signed_ratio_half_rounds_away_from_zero(void) {
    TEST_ASSERT_EQUAL_INT32(1, rigRoundRatioHalfAwayFromZero(1, 2));
    TEST_ASSERT_EQUAL_INT32(-1, rigRoundRatioHalfAwayFromZero(-1, 2));
    TEST_ASSERT_EQUAL_INT32(0, rigRoundRatioHalfAwayFromZero(1, 3));
    TEST_ASSERT_EQUAL_INT32(0, rigRoundRatioHalfAwayFromZero(-1, 3));
}

void test_recording_display_captures_head_and_eye_circles(void) {
    static constexpr RigBone headBones[] = {
        {"head", -1, 4, 0, 0, 0},
    };
    static constexpr RigPart headParts[] = {
        {0, RIG_RENDER_LINE, nullptr, 0, 0, 0, 0},
    };
    static constexpr int16_t headOffsets[] = {0};
    static constexpr RigPose headPose = {"rest", headOffsets, 0};
    static constexpr Rig headRig = {
        "head-proof", headBones, 1, headParts, 1,
        &headPose, 1, nullptr, 0};
    RigActor actor;
    actor.setRig(&headRig);
    actor.setPos(10, 10);
    DisplayProxy display;
    actor.draw(display);
    TEST_ASSERT_EQUAL_INT(0, display.lineCount);
    TEST_ASSERT_EQUAL_INT(3, display.circleCount);
    TEST_ASSERT_EQUAL_INT16(12, display.circles[0].x);
    TEST_ASSERT_EQUAL_INT16(9, display.circles[0].y);
    TEST_ASSERT_EQUAL_INT16(4, display.circles[0].radius);
    TEST_ASSERT_EQUAL_INT16(0, display.circles[1].radius);
    TEST_ASSERT_EQUAL_INT16(0, display.circles[2].radius);
}

void setUp(void) {}
void tearDown(void) {}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_degree_conversion_rounds_half_away_and_wraps);
    RUN_TEST(test_parent_first_quarter_turn_has_negative_coordinates);
    RUN_TEST(test_arithmetic_shift_floors_negative_rotation);
    RUN_TEST(test_opposite_angle_blend_tie_takes_negative_path);
    RUN_TEST(test_signed_ratio_half_rounds_away_from_zero);
    RUN_TEST(test_recording_display_captures_head_and_eye_circles);
    return UNITY_END();
}
