// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#include <unity.h>

#include "cf_gfx_rig.h"

using namespace cf::gfx;

namespace {

constexpr RigBone kBones[] = {
    {"pelvis", -1, 2, 0, 0, -90},
    {"spine", 0, 7, 2, 0, 0},
    {"head", 1, 5, 7, 0, 0},
    {"l_arm", 1, 5, 6, -3, 192},
    {"l_fore", 3, 5, 5, 0, -6},
    {"r_arm", 1, 5, 6, 3, 168},
    {"r_fore", 5, 5, 5, 0, 6},
    {"l_thigh", 0, 6, 0, -2, 185},
    {"l_shin", 7, 6, 6, 0, 4},
    {"r_thigh", 0, 6, 0, 2, 175},
    {"r_shin", 9, 6, 6, 0, -4},
};
constexpr int16_t kRestOffsets[11] = {};
constexpr RigPose kRest = {"rest", kRestOffsets, 0};
constexpr Rig kRig = {
    "verlet-test", kBones, 11, nullptr, 0, &kRest, 1, nullptr, 0};

}  // namespace

void test_scripted_drop_stays_inside_floor_and_settles_hard_sticks(void) {
    RigActor actor;
    actor.setRig(&kRig);
    actor.setPos(64, 18);
    actor.ragdoll(1.25f, -2.0f);
    for (uint32_t tick = 1; tick <= 1200 &&
                            actor.state() != RigActor::STATE_STANDING; ++tick) {
        actor.update(tick * 20);
    }
    TEST_ASSERT_EQUAL_UINT8(RigActor::STATE_STANDING, actor.state());
    TEST_ASSERT_EQUAL_UINT8(11, actor.particleCount());
    for (uint8_t i = 0; i < actor.particleCount(); ++i) {
        TEST_ASSERT_LESS_OR_EQUAL_FLOAT(58.0f, actor.particle(i).y);
    }
    TEST_ASSERT_LESS_THAN_FLOAT(0.1f, actor.maxHardConstraintError());
}

void setUp(void) {}
void tearDown(void) {}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_scripted_drop_stays_inside_floor_and_settles_hard_sticks);
    return UNITY_END();
}
