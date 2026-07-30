// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#include <chrono>
#include <stdio.h>
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
    "budget-test", kBones, 11, nullptr, 0, &kRest, 1, nullptr, 0};

}  // namespace

void test_worst_case_ragdoll_tick_budget(void) {
    RigActor actor;
    actor.setRig(&kRig);
    actor.setPos(64, 30);
    actor.ragdoll();
    TEST_ASSERT_TRUE(actor.pin(static_cast<uint8_t>(0), 64.0f, 30.0f));

    constexpr uint32_t iterations = 50000;
    const auto begin = std::chrono::steady_clock::now();
    for (uint32_t i = 0; i < iterations; ++i) actor.update(i * 20);
    const auto end = std::chrono::steady_clock::now();
    const double microseconds =
        std::chrono::duration<double, std::micro>(end - begin).count() /
        iterations;
    printf("rig ragdoll budget: %.3f us/tick (%lu ticks)\n",
           microseconds, static_cast<unsigned long>(iterations));
    char timingMessage[96] = {};
    snprintf(timingMessage, sizeof(timingMessage),
             "rig ragdoll budget: %.3f us/tick (%lu ticks)",
             microseconds, static_cast<unsigned long>(iterations));
    TEST_MESSAGE(timingMessage);
    TEST_ASSERT_LESS_THAN_FLOAT(500.0f, static_cast<float>(microseconds));
}

void setUp(void) {}
void tearDown(void) {}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_worst_case_ragdoll_tick_budget);
    return UNITY_END();
}
