// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#include <unity.h>
#include <string.h>
#include <type_traits>
#include "ResumePolicy.h"
#include "PromptPolicy.h"
#include "BootGesture.h"
using namespace ResumePolicy;
void setUp() {}
void tearDown() {}

void test_arm_only_eligible_idle_foreground() {
    const SleepReason reasons[] = {SleepReason::Idle, SleepReason::UserOff, SleepReason::EmptyBattery};
    for (SleepReason reason : reasons)
        for (int app = -1; app < 8; ++app)
            for (int eligible = 0; eligible < 2; ++eligible)
                TEST_ASSERT_EQUAL(reason == SleepReason::Idle && app >= 0 && eligible,
                                  shouldArm(reason, app, eligible));
}

void test_record_validation_and_build_identity() {
    static_assert(std::is_trivial<Record>::value, "RTC record must be trivial");
    static_assert(std::is_standard_layout<Record>::value, "RTC record must have fixed layout");
    const auto build = buildId("1.0+abc", "2026-11-05T10:00:00Z");
    TEST_ASSERT_EQUAL_UINT32(build, buildId("1.0+abc", "2026-11-05T10:00:00Z"));
    TEST_ASSERT_NOT_EQUAL(build, buildId("1.0+def", "2026-11-05T10:00:00Z"));
    TEST_ASSERT_NOT_EQUAL(build, buildId("1.0+abc", "2026-11-05T11:00:00Z"));
    TEST_ASSERT_NOT_EQUAL(buildId("ab", "c"), buildId("a", "bc"));
    Record good;
    TEST_ASSERT_TRUE(makeRecord(good, 3, "Tools", build));
    TEST_ASSERT_TRUE(valid(good, 4, build));
    TEST_ASSERT_EQUAL_STRING("Tools", good.category);
    Record bad = good; bad.magic ^= 1; TEST_ASSERT_FALSE(valid(bad, 4, build));
    bad = good; bad.version++; TEST_ASSERT_FALSE(valid(bad, 4, build));
    bad = good; bad.appIndex = -1; TEST_ASSERT_FALSE(valid(bad, 4, build));
    bad = good; bad.appIndex = 4; TEST_ASSERT_FALSE(valid(bad, 4, build));
    bad = good; bad.buildId++; TEST_ASSERT_FALSE(valid(bad, 4, build));
    bad = good; memset(bad.category, 'a', sizeof(bad.category)); TEST_ASSERT_FALSE(valid(bad, 4, build));
    TEST_ASSERT_FALSE(valid(good, 0, build));
    TEST_ASSERT_FALSE(makeRecord(bad, -1, "Tools", build));
    TEST_ASSERT_EQUAL_UINT32(0, bad.magic);
    TEST_ASSERT_FALSE(makeRecord(bad, 32768, "Tools", build));
    TEST_ASSERT_FALSE(makeRecord(bad, 3, nullptr, build));
    char longCategory[kCategorySize + 1]; memset(longCategory, 'a', kCategorySize); longCategory[kCategorySize] = 0;
    TEST_ASSERT_FALSE(makeRecord(bad, 3, longCategory, build));
    longCategory[kCategorySize - 1] = 0;
    TEST_ASSERT_TRUE(makeRecord(bad, 3, longCategory, build));
    TEST_ASSERT_TRUE(valid(bad, 4, build));
    TEST_ASSERT_TRUE(makeRecord(bad, 0, "", build));
    TEST_ASSERT_TRUE(valid(bad, 4, build));
}

void test_exhaustive_boot_arbitration() {
    const Reset resets[] = {Reset::DeepSleep, Reset::PowerOn, Reset::Software, Reset::Panic,
                            Reset::Watchdog, Reset::Brownout, Reset::Other};
    const Wake wakes[] = {Wake::Button, Wake::Timer, Wake::Other};
    // All combinations of record, handoff, explicit one-shot and recovery gates.
    for (Reset reset : resets) for (Wake wake : wakes) for (unsigned bits = 0; bits < 128; ++bits) {
        BootInputs in;
        in.reset = reset; in.wake = wake;
        in.recordPresent = bits & 1; in.recordValid = bits & 2;
        in.resumeOneShot = bits & 4; in.explicitOneShot = bits & 8;
        in.imagePending = bits & 16; in.updateVerification = bits & 32;
        in.factoryReset = bits & 64; in.appIndex = 3;
        const auto out = decide(in);
        if (wake == Wake::Timer) {
            TEST_ASSERT_EQUAL(-1, out.appIndex);
            TEST_ASSERT_FALSE(out.discardRecord);
            TEST_ASSERT_EQUAL((int)Reason::Timer, (int)out.reason);
            continue;
        }
        const bool present = in.recordPresent || in.resumeOneShot;
        const bool blocked = in.explicitOneShot || in.imagePending || in.updateVerification || in.factoryReset;
        const bool allowed = in.resumeOneShot || (reset == Reset::DeepSleep && wake == Wake::Button);
        const bool resumes = present && in.recordValid && !blocked && allowed;
        TEST_ASSERT_EQUAL(resumes ? 3 : -1, out.appIndex);
        TEST_ASSERT_EQUAL(present, out.discardRecord);
        Reason reason = Reason::NoRecord;
        if (present) {
            if (in.explicitOneShot) reason = Reason::ExplicitBoot;
            else if (in.imagePending || in.updateVerification) reason = Reason::Update;
            else if (in.factoryReset) reason = Reason::FactoryReset;
            else if (!in.recordValid) reason = Reason::InvalidRecord;
            else if (in.resumeOneShot) reason = Reason::Handoff;
            else if (reset != Reset::DeepSleep) reason = Reason::Reset;
            else if (wake != Wake::Button) reason = Reason::Wake;
            else reason = Reason::Button;
        }
        TEST_ASSERT_EQUAL((int)reason, (int)out.reason);
        TEST_ASSERT_NOT_NULL(reasonName(out.reason));
    }
}

void test_timer_preserves_record_then_button_consumes_before_launch() {
    Record record;
    TEST_ASSERT_TRUE(makeRecord(record, 3, "Tools", 12));
    const Record original = record;
    BootInputs in;
    in.reset = Reset::DeepSleep; in.wake = Wake::Timer;
    in.recordPresent = true; in.recordValid = valid(record, 4, 12); in.appIndex = record.appIndex;
    for (int wake = 0; wake < 24; ++wake) {
        const auto out = decide(in);
        if (out.discardRecord) record.magic = 0;
        TEST_ASSERT_EQUAL_MEMORY(&original, &record, sizeof(record));
    }
    in.wake = Wake::Button;
    const auto out = decide(in);
    TEST_ASSERT_EQUAL(3, out.appIndex);
    TEST_ASSERT_TRUE(out.discardRecord);
    record.magic = 0; // boot consumes before invoking begin()
    in.recordPresent = false; in.recordValid = valid(record, 4, 12);
    TEST_ASSERT_EQUAL(-1, decide(in).appIndex);
    // A malformed handoff must also be consumed, never launched.
    in.resumeOneShot = true;
    TEST_ASSERT_EQUAL(-1, decide(in).appIndex);
    TEST_ASSERT_TRUE(decide(in).discardRecord);
    in.recordValid = true; in.appIndex = -1;
    TEST_ASSERT_EQUAL(-1, decide(in).appIndex);
}

void test_install_records_only_eligible_app_in_dev_mode() {
    for (int dev = 0; dev < 2; ++dev)
        for (int eligible = 0; eligible < 2; ++eligible)
            TEST_ASSERT_EQUAL(dev && eligible, shouldRecordForInstall(dev, eligible));
}

void test_install_reload_only_for_target_image_and_when_consumed() {
    TEST_ASSERT_TRUE(shouldReload("1.6.2+abc", "1.6.2+abc", true));
    TEST_ASSERT_FALSE(shouldReload("1.6.2+abc", "1.6.1+def", true));  // rollback or failed download
    TEST_ASSERT_FALSE(shouldReload("1.6.2+abc", "1.6.3+abc", true));  // a later image
    TEST_ASSERT_FALSE(shouldReload("1.6.2+abc", "1.6.2+abc", false)); // record not consumed
    TEST_ASSERT_FALSE(shouldReload("", "", true));                    // record without a target
    TEST_ASSERT_FALSE(shouldReload(nullptr, "1.6.2+abc", true));
    TEST_ASSERT_FALSE(shouldReload("1.6.2+abc", nullptr, true));
}

void test_install_reload_finds_app_by_id() {
    const char* const ids[] = {"APP_MENU", "APP_COUNTER", "APP_HOME_REMOTE"};
    TEST_ASSERT_EQUAL(1, indexOfId("APP_COUNTER", ids, 3));
    TEST_ASSERT_EQUAL(2, indexOfId("APP_HOME_REMOTE", ids, 3));
    TEST_ASSERT_EQUAL(-1, indexOfId("APP_GONE", ids, 3));
    TEST_ASSERT_EQUAL(-1, indexOfId("", ids, 3));
    TEST_ASSERT_EQUAL(-1, indexOfId(nullptr, ids, 3));
    TEST_ASSERT_EQUAL(-1, indexOfId("APP_COUNTER", ids, 1));
}

void test_resume_has_no_early_animation_frame() {
    PromptPolicy::StartShots shots;
    TEST_ASSERT_TRUE(PromptPolicy::earlyAnimationFrame(shots));
    shots.resumeApp = true;
    TEST_ASSERT_FALSE(PromptPolicy::earlyAnimationFrame(shots));
}

void test_wake_press_release_and_bounce_are_swallowed() {
    BootGesture gate;
    gate.init(true);
    TEST_ASSERT_TRUE(gate.swallow(true, 0, 20));
    TEST_ASSERT_TRUE(gate.swallow(true, 10000, 20)); // longer than animation and Held
    TEST_ASSERT_TRUE(gate.swallow(false, 10001, 20));
    TEST_ASSERT_TRUE(gate.swallow(true, 10010, 20)); // release bounce
    TEST_ASSERT_TRUE(gate.swallow(false, 10011, 20));
    TEST_ASSERT_TRUE(gate.swallow(false, 10031, 20));
    TEST_ASSERT_TRUE(gate.swallow(false, 10032, 20)); // swallow the stable release itself
    TEST_ASSERT_FALSE(gate.swallow(false, 10033, 20));
    TEST_ASSERT_FALSE(gate.swallow(true, 10040, 20)); // next Pressed
    TEST_ASSERT_FALSE(gate.swallow(true, 13040, 20)); // next Held
    TEST_ASSERT_FALSE(gate.swallow(false, 13050, 20)); // next Released
}

void test_released_boot_buttons_are_normal_and_gates_are_independent() {
    BootGesture held, released;
    held.init(true); released.init(false);
    TEST_ASSERT_TRUE(held.swallow(true, 100, 20));
    TEST_ASSERT_FALSE(released.swallow(true, 100, 20));
    TEST_ASSERT_FALSE(released.swallow(false, 300, 20));
    held.init(false);
    TEST_ASSERT_FALSE(held.swallow(true, 500, 20));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_arm_only_eligible_idle_foreground);
    RUN_TEST(test_record_validation_and_build_identity);
    RUN_TEST(test_exhaustive_boot_arbitration);
    RUN_TEST(test_timer_preserves_record_then_button_consumes_before_launch);
    RUN_TEST(test_install_records_only_eligible_app_in_dev_mode);
    RUN_TEST(test_install_reload_only_for_target_image_and_when_consumed);
    RUN_TEST(test_install_reload_finds_app_by_id);
    RUN_TEST(test_resume_has_no_early_animation_frame);
    RUN_TEST(test_wake_press_release_and_bounce_are_swallowed);
    RUN_TEST(test_released_boot_buttons_are_normal_and_gates_are_independent);
    return UNITY_END();
}
