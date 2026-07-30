// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#include <unity.h>

#include "cf_gfx_rig.h"
#include "cf_gfx_rig_golden.h"

using namespace cf::gfx;
using namespace cf::gfx::riggolden;

namespace {

RigBone fixtureBones[boneCount] = {};
RigPose fixturePoses[poseCount] = {};
int16_t fixtureOffsets[poseCount][boneCount] = {};
Rig fixtureRig = {};

void prepareFixture() {
    for (uint8_t i = 0; i < boneCount; ++i) {
        fixtureBones[i] = RigBone{
            bones[i].name, bones[i].parent, bones[i].length,
            bones[i].atX, bones[i].atY, bones[i].restDegrees};
    }
    for (uint8_t pose = 0; pose < poseCount; ++pose) {
        for (uint8_t bone = 0; bone < boneCount; ++bone) {
            fixtureOffsets[pose][bone] = poses[pose].offsets[bone];
        }
        fixturePoses[pose] = RigPose{
            poses[pose].name, fixtureOffsets[pose], poses[pose].rootDy};
    }
    fixtureRig = Rig{"contract-figure", fixtureBones, boneCount,
                     nullptr, 0, fixturePoses, poseCount, nullptr, 0};
}

void assertCase(uint8_t caseIndex) {
    const GoldenCase& golden = cases[caseIndex];
    RigBoneTransform actual[boneCount] = {};
    const RigPose* poseB =
        golden.poseB < 0 ? nullptr : &fixturePoses[golden.poseB];
    TEST_ASSERT_TRUE(solveRigPose(
        fixtureRig, &fixturePoses[golden.poseA], poseB,
        golden.blend, actual, boneCount));
    for (uint8_t bone = 0; bone < boneCount; ++bone) {
        const GoldenTransform& expected = golden.expected[bone];
        TEST_ASSERT_EQUAL_INT32(expected.startX, actual[bone].startX);
        TEST_ASSERT_EQUAL_INT32(expected.startY, actual[bone].startY);
        TEST_ASSERT_EQUAL_INT32(expected.endX, actual[bone].endX);
        TEST_ASSERT_EQUAL_INT32(expected.endY, actual[bone].endY);
        TEST_ASSERT_EQUAL_UINT8(expected.angle, actual[bone].angle);
    }
}

}  // namespace

void test_all_five_cross_language_golden_cases(void) {
    prepareFixture();
    TEST_ASSERT_EQUAL_UINT8(5, caseCount);
    for (uint8_t i = 0; i < caseCount; ++i) assertCase(i);
}

void setUp(void) {}
void tearDown(void) {}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_all_five_cross_language_golden_cases);
    return UNITY_END();
}
