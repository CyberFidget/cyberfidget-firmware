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

// A rig wider than the particle budget cannot ragdoll. That refusal used
// to be silent, so an over-budget rig simply did nothing with no way for
// the caller to know.
void test_over_budget_rig_reports_refusal(void) {
    static constexpr RigBone kWideBones[12] = {
        {"root", -1, 0, 0, 0, 0},   {"b1", 0, 0, -6, 0, -6},
        {"b2", 1, 0, -6, 0, -6},    {"b3", 2, 0, -5, 0, -5},
        {"b4", 3, -5, 0, -5, 0},    {"b5", 4, -6, 0, -6, 0},
        {"b6", 3, 5, 0, 5, 0},      {"b7", 6, 6, 0, 6, 0},
        {"b8", 0, -4, 6, -4, 6},    {"b9", 8, 0, 6, 0, 6},
        {"b10", 0, 4, 6, 4, 6},     {"b11", 10, 0, 6, 0, 6},
    };
    static constexpr int16_t kWideRestOffsets[12] = {};
    static constexpr RigPose kWideRest = {"rest", kWideRestOffsets, 0};
    static constexpr Rig kWideRig = {
        "over-budget", kWideBones, 12, nullptr, 0, &kWideRest, 1, nullptr, 0};

    RigActor actor;
    actor.setRig(&kWideRig);
    actor.setPos(64, 30);
    TEST_ASSERT_FALSE(actor.ragdoll());

    RigActor inBudget;
    inBudget.setRig(&kRig);
    inBudget.setPos(64, 30);
    TEST_ASSERT_TRUE(inBudget.ragdoll());
}

// The settle counter used to be a wrapping uint8, so a pin held past 256
// ticks re-armed the settle window and the stand-up stalled for ~2 s on
// roughly a quarter of release times. Hold well past the wrap point and
// require a prompt stand-up.
void test_long_pin_hold_still_stands_up_promptly(void) {
    RigActor actor;
    actor.setRig(&kRig);
    actor.setPos(64, 30);
    actor.ragdoll();
    TEST_ASSERT_TRUE(actor.pin(static_cast<uint8_t>(0), 64.0f, 30.0f));

    uint32_t tick = 0;
    for (; tick < 400; ++tick) actor.update(tick * 20);
    actor.unpin();

    const uint32_t unpinnedAt = tick;
    while (actor.state() != RigActor::STATE_STANDING &&
           tick < unpinnedAt + 200) {
        actor.update(tick * 20);
        ++tick;
    }
    TEST_ASSERT_EQUAL(static_cast<int>(RigActor::STATE_STANDING),
                      static_cast<int>(actor.state()));
    TEST_ASSERT_LESS_THAN_UINT32(unpinnedAt + 100, tick);
}

void setUp(void) {}
void tearDown(void) {}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_worst_case_ragdoll_tick_budget);
    RUN_TEST(test_over_budget_rig_reports_refusal);
    RUN_TEST(test_long_pin_hold_still_stands_up_promptly);
    return UNITY_END();
}
