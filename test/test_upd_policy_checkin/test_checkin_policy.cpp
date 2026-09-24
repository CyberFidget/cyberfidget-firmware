// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#include <unity.h>

#include "CheckinPolicy.h"

using namespace CheckinPolicy;

namespace {
constexpr uint32_t kNow = 1790000000u;   // a set clock (2026)
constexpr uint32_t kHour = 3600u;
constexpr uint32_t kDay = 24u * kHour;

// Everything an automatic session needs, all passing.
Inputs eligible() {
    Inputs in;
    in.policy = Policy::Auto;
    in.wifiSaved = true;
    in.linked = true;
    in.vbatMv = 3900;
    in.socPct = 70;
    in.due = true;
    in.atIdleMenu = true;
    in.btIdle = true;
    return in;
}

void expect(Verdict want, Session s, const Inputs& in) {
    TEST_ASSERT_EQUAL_STRING(verdictName(want), verdictName(decide(s, in)));
}
} // namespace

void setUp(void) {}
void tearDown(void) {}

// ---- policy text -----------------------------------------------------------

void test_policy_text_defaults_to_auto(void) {
    TEST_ASSERT_EQUAL_INT((int)Policy::Auto, (int)parsePolicy(nullptr));
    TEST_ASSERT_EQUAL_INT((int)Policy::Auto, (int)parsePolicy(""));
    TEST_ASSERT_EQUAL_INT((int)Policy::Auto, (int)parsePolicy("auto"));
    TEST_ASSERT_EQUAL_INT((int)Policy::Auto, (int)parsePolicy("garbage"));
    TEST_ASSERT_EQUAL_INT((int)Policy::Never, (int)parsePolicy("never"));
}

// ---- boot window -------------------------------------------------------------

void test_boot_starts_when_eligible_without_being_due(void) {
    Inputs in = eligible();
    in.due = false;   // the boot window is not interval-gated
    expect(Verdict::Start, Session::Boot, in);
}

void test_boot_blocked_by_each_condition(void) {
    Inputs in = eligible(); in.policy = Policy::Never;  expect(Verdict::PolicyOff, Session::Boot, in);
    in = eligible(); in.wifiSaved = false;              expect(Verdict::NoWifi, Session::Boot, in);
    in = eligible(); in.linked = false;                 expect(Verdict::NotLinked, Session::Boot, in);
    in = eligible(); in.oneShotBoot = true;             expect(Verdict::OneShotBoot, Session::Boot, in);
    in = eligible(); in.timerWake = true;               expect(Verdict::TimerWake, Session::Boot, in);
    in = eligible(); in.vbatMv = 3599;                  expect(Verdict::LowBattery, Session::Boot, in);
    in = eligible(); in.socPct = 19;                    expect(Verdict::LowBattery, Session::Boot, in);
    in = eligible(); in.sessionRunning = true;          expect(Verdict::Busy, Session::Boot, in);
}

void test_boot_skips_a_recent_check_in_and_a_running_backoff(void) {
    // Every button wake is a start: a check-in less than an hour old, or a
    // failure still backing off (e.g. away from the saved network), must
    // not bring the radio up again.
    Inputs in = eligible(); in.bootGapPassed = false;   expect(Verdict::NotDue, Session::Boot, in);
    in = eligible(); in.backedOff = true;               expect(Verdict::BackedOff, Session::Boot, in);
    TEST_ASSERT_TRUE(kBootMinGapSec <= 3600u);
    TEST_ASSERT_TRUE(elapsedSinceCheckin(kPlausibleEpoch + 3599, kPlausibleEpoch, 0) < kBootMinGapSec);
    TEST_ASSERT_TRUE(elapsedSinceCheckin(kPlausibleEpoch + 3600, kPlausibleEpoch, 0) >= kBootMinGapSec);
}

// ---- battery gate -----------------------------------------------------------

void test_battery_gate_needs_both_values(void) {
    TEST_ASSERT_TRUE(batteryEligible(3600, 20));
    TEST_ASSERT_FALSE(batteryEligible(3599, 100));
    TEST_ASSERT_FALSE(batteryEligible(4200, 19));
    TEST_ASSERT_FALSE(batteryEligible(-1, 80));   // unreadable voltage
    TEST_ASSERT_FALSE(batteryEligible(4000, -1)); // unreadable charge
}

void test_low_battery_blocks_daily_and_awake_radio(void) {
    Inputs in = eligible(); in.vbatMv = 3500;
    expect(Verdict::LowBattery, Session::Daily, in);
    expect(Verdict::LowBattery, Session::Awake, in);
    in = eligible(); in.socPct = 10;
    expect(Verdict::LowBattery, Session::Daily, in);
    expect(Verdict::LowBattery, Session::Awake, in);
}

// ---- daily wake ---------------------------------------------------------------

void test_daily_needs_due_and_no_backoff(void) {
    Inputs in = eligible(); in.due = false;     expect(Verdict::NotDue, Session::Daily, in);
    in = eligible(); in.backedOff = true;       expect(Verdict::BackedOff, Session::Daily, in);
    in = eligible(); in.policy = Policy::Never; expect(Verdict::PolicyOff, Session::Daily, in);
    in = eligible(); in.linked = false;         expect(Verdict::NotLinked, Session::Daily, in);
}

void test_backoff_grows_and_caps(void) {
    TEST_ASSERT_EQUAL_UINT32(0, backoffSec(0));
    TEST_ASSERT_EQUAL_UINT32(1 * kHour, backoffSec(1));
    TEST_ASSERT_EQUAL_UINT32(2 * kHour, backoffSec(2));
    TEST_ASSERT_EQUAL_UINT32(4 * kHour, backoffSec(3));
    TEST_ASSERT_EQUAL_UINT32(8 * kHour, backoffSec(4));
    TEST_ASSERT_EQUAL_UINT32(16 * kHour, backoffSec(5));
    TEST_ASSERT_EQUAL_UINT32(kDay, backoffSec(6));
    TEST_ASSERT_EQUAL_UINT32(kDay, backoffSec(200));
}

void test_failure_schedules_backoff_success_clears(void) {
    Backoff b;
    b.record(false, kNow);
    TEST_ASSERT_TRUE(b.running(kNow + kHour - 1));
    TEST_ASSERT_FALSE(b.running(kNow + kHour));
    b.record(false, kNow + kHour);
    TEST_ASSERT_EQUAL_UINT32(kNow + kHour + 2 * kHour, b.until);
    b.record(true, kNow + 4 * kHour);
    TEST_ASSERT_EQUAL_UINT8(0, b.failures);
    TEST_ASSERT_FALSE(b.running(kNow + 4 * kHour));
    TEST_ASSERT_EQUAL_UINT32(kNow + 4 * kHour, b.localLast);
}

// ---- interval -----------------------------------------------------------------

void test_interval_default_and_bounds(void) {
    TEST_ASSERT_EQUAL_UINT16(24, sanitizeInterval(0));
    TEST_ASSERT_EQUAL_UINT16(24, sanitizeInterval(24 * 365 + 1));
    TEST_ASSERT_EQUAL_UINT16(72, sanitizeInterval(72));
    TEST_ASSERT_FALSE(isDue(kDay - 1, 24));
    TEST_ASSERT_TRUE(isDue(kDay, 24));
    TEST_ASSERT_FALSE(isDue(kDay, 72));
    TEST_ASSERT_TRUE(isDue(kDay, 0));   // unset reads as 24 h
}

// ---- time source ----------------------------------------------------------------

void test_server_time_wins_when_clock_set(void) {
    // Stored server time 10 h ago; a local mark says 1 h ago: server wins.
    TEST_ASSERT_EQUAL_UINT32(10 * kHour,
        elapsedSinceCheckin(kNow, kNow - 10 * kHour, kNow - kHour));
}

void test_clock_reset_falls_back_to_uptime(void) {
    // Clock back at 5 h since reset, stored server time from before: the
    // time since the reset is the estimate.
    TEST_ASSERT_EQUAL_UINT32(5 * kHour, elapsedSinceCheckin(5 * kHour, kNow, 0));
    // With a local success mark on the reset clock, it counts from there.
    TEST_ASSERT_EQUAL_UINT32(2 * kHour, elapsedSinceCheckin(5 * kHour, kNow, 3 * kHour));
    // A set clock that went behind the stored time uses the local mark...
    TEST_ASSERT_EQUAL_UINT32(kHour, elapsedSinceCheckin(kNow - kDay, kNow, kNow - kDay - kHour));
    // ...and without one nothing is known: due.
    TEST_ASSERT_EQUAL_UINT32(kUnknownElapsed, elapsedSinceCheckin(kNow - kDay, kNow, 0));
    TEST_ASSERT_TRUE(isDue(kUnknownElapsed, 24));
    // Never checked in, clock set: due.
    TEST_ASSERT_TRUE(isDue(elapsedSinceCheckin(kNow, 0, 0), 24));
}

// ---- simulated hourly wakes (sleep path) -----------------------------------------

// Drives the timer-wake cycle the device runs, through the same functions
// the device calls: armDueSec() before each sleep (HAL keeps the value in
// RTC memory), applySchedule() for the decision on a due wake. One session
// per due wake is structural on the device (runHeadless runs once).
struct Sim {
    uint32_t now = kNow;
    Schedule schedule;
    Backoff backoff;
    uint32_t armed = 0;
    int checkins = 0;
    int attempts = 0;
    int storageWakes = 0;   // wakes that read settings (past the RTC compare)
    bool serverUp = true;
    int32_t vbatMv = 3900;

    Sim() {
        schedule.wifiSaved = true;
        schedule.linked = true;
        schedule.lastChk = kNow;   // checked in when it went to sleep
    }
    void arm() { armed = armDueSec(schedule, backoff, now); }
    void wake() {
        now += kHour;
        if (armed == 0 || now < armed) return;   // RTC compare only, no storage
        ++storageWakes;
        Inputs in;
        applySchedule(in, schedule, backoff, now);
        in.vbatMv = vbatMv;
        in.socPct = 70;
        const Verdict v = decide(Session::Daily, in);
        if (v == Verdict::Start) {
            ++attempts;
            if (serverUp) { schedule.lastChk = now; ++checkins; }
            backoff.record(serverUp, now);
        } else if (v == Verdict::LowBattery) {
            backoff.holdForLowBattery(now);
        }
        arm();
    }
};

void test_hourly_wakes_check_in_once_per_interval(void) {
    Sim sim;
    sim.arm();
    for (int h = 0; h < 72; ++h) sim.wake();
    TEST_ASSERT_EQUAL_INT(3, sim.checkins);
    TEST_ASSERT_EQUAL_UINT32(kNow + 72 * kHour, sim.schedule.lastChk);
}

void test_hourly_wakes_honour_longer_interval(void) {
    Sim sim;
    sim.schedule.intervalH = 72;
    sim.arm();
    for (int h = 0; h < 168; ++h) sim.wake();
    TEST_ASSERT_EQUAL_INT(2, sim.checkins);
}

void test_failed_wakes_back_off(void) {
    Sim sim;
    sim.serverUp = false;
    sim.arm();
    for (int h = 0; h < 24 + 48; ++h) sim.wake();
    // Due at 24 h, then retries after 1, 2, 4, 8, 16 h and 24 h: at 25, 27,
    // 31, 39, 55 h (plus the first at 24 h) within 72 h.
    TEST_ASSERT_EQUAL_INT(6, sim.attempts);
    TEST_ASSERT_EQUAL_INT(0, sim.checkins);
    sim.serverUp = true;
    for (int h = 0; h < 24; ++h) sim.wake();
    TEST_ASSERT_EQUAL_INT(1, sim.checkins);
    TEST_ASSERT_EQUAL_UINT8(0, sim.backoff.failures);
}

void test_policy_never_makes_no_attempts(void) {
    const Session automatic[] = {Session::Boot, Session::Daily, Session::Awake};
    for (Session s : automatic) {
        Inputs in = eligible();
        in.policy = Policy::Never;
        expect(Verdict::PolicyOff, s, in);
    }
}

// ---- awake deadline ----------------------------------------------------------

void test_awake_runs_only_at_idle_menu_with_bt_idle(void) {
    Inputs in = eligible();
    expect(Verdict::Start, Session::Awake, in);
    in.atIdleMenu = false;                     // an app is running
    expect(Verdict::NotIdle, Session::Awake, in);
    in = eligible(); in.promptOpen = true;
    expect(Verdict::NotIdle, Session::Awake, in);
    in = eligible(); in.btIdle = false;
    expect(Verdict::BtNotIdle, Session::Awake, in);
    in = eligible(); in.due = false;
    expect(Verdict::NotDue, Session::Awake, in);
    in = eligible(); in.backedOff = true;
    expect(Verdict::BackedOff, Session::Awake, in);
}

void test_awake_check_does_not_duplicate_next_wake(void) {
    // Awake past the interval: one check at the menu stores the server time.
    Sim sim;
    sim.now = kNow + 30 * kHour;
    Inputs in = eligible();
    applySchedule(in, sim.schedule, sim.backoff, sim.now);
    TEST_ASSERT_EQUAL_INT((int)Verdict::Start, (int)decide(Session::Awake, in));
    sim.schedule.lastChk = sim.now;
    sim.backoff.record(true, sim.now);
    // It then sleeps: the next due time is a full interval later, so the
    // hourly wakes do not check again until then.
    sim.arm();
    TEST_ASSERT_EQUAL_UINT32(sim.now + kDay, sim.armed);
    for (int h = 0; h < 23; ++h) sim.wake();
    TEST_ASSERT_EQUAL_INT(0, sim.checkins);
    sim.wake();
    TEST_ASSERT_EQUAL_INT(1, sim.checkins);
}

void test_next_due_never_before_backoff(void) {
    TEST_ASSERT_EQUAL_UINT32(kNow + 10 * kHour, nextDueSec(kNow, 14 * kHour, 24, 0));
    TEST_ASSERT_EQUAL_UINT32(kNow, nextDueSec(kNow, kDay, 24, 0));
    TEST_ASSERT_EQUAL_UINT32(kNow + 2 * kHour, nextDueSec(kNow, kDay, 24, kNow + 2 * kHour));
    TEST_ASSERT_EQUAL_UINT32(0xFFFFFFFFu, nextDueSec(0xFFFFFFF0u, 0, 24, 0));
}

// ---- review round: cold start, low battery, arm gating -------------------------

void test_cold_start_does_not_suppress_boot_window(void) {
    // A chip reset / brownout restarts the clock near 0: only the since-
    // reset fallback is known, which is not evidence of a recent check-in.
    Schedule sch;
    sch.wifiSaved = true; sch.linked = true;
    sch.lastChk = kNow;                  // from before the reset
    Backoff none;
    TEST_ASSERT_TRUE(timing(sch, none, 3).bootGapPassed);
    Inputs in = eligible();
    applySchedule(in, sch, none, 3);
    expect(Verdict::Start, Session::Boot, in);
    // Never checked in, clock set: nothing to compare, gap passed.
    sch.lastChk = 0;
    TEST_ASSERT_TRUE(timing(sch, none, kNow).bootGapPassed);
    // Real evidence still gates: a set clock 10 min after the stored time...
    sch.lastChk = kNow;
    TEST_ASSERT_FALSE(timing(sch, none, kNow + 600).bootGapPassed);
    TEST_ASSERT_TRUE(timing(sch, none, kNow + 3600).bootGapPassed);
    // ...or a local success mark this power-on on an unset clock.
    Backoff mark; mark.localLast = 100;
    TEST_ASSERT_FALSE(timing(sch, mark, 700).bootGapPassed);
    TEST_ASSERT_TRUE(timing(sch, mark, 100 + 3600).bootGapPassed);
}

void test_low_battery_wake_keeps_following_wakes_storage_free(void) {
    Sim sim;
    sim.vbatMv = 3500;
    sim.arm();
    for (int h = 0; h < 72; ++h) sim.wake();
    // Due at 24 h (low: held 24 h), again at 48 h and 72 h - one storage
    // read per day, not one per hour.
    TEST_ASSERT_EQUAL_INT(3, sim.storageWakes);
    TEST_ASSERT_EQUAL_INT(0, sim.attempts);
    TEST_ASSERT_EQUAL_UINT32(sim.now + kLowBatteryHoldSec, sim.armed);
    // Charged again: the next due wake checks in and clears the hold.
    sim.vbatMv = 3900;
    for (int h = 0; h < 24; ++h) sim.wake();
    TEST_ASSERT_EQUAL_INT(1, sim.checkins);
    TEST_ASSERT_EQUAL_UINT32(0, sim.backoff.lowBatteryUntil);
}

void test_arm_gating_and_server_backoff(void) {
    Schedule sch;
    sch.wifiSaved = true; sch.linked = true; sch.lastChk = kNow;
    Backoff none;
    TEST_ASSERT_EQUAL_UINT32(kNow + kDay, armDueSec(sch, none, kNow));
    Schedule off = sch; off.policy = Policy::Never;
    TEST_ASSERT_EQUAL_UINT32(0, armDueSec(off, none, kNow));
    Schedule noWifi = sch; noWifi.wifiSaved = false;
    TEST_ASSERT_EQUAL_UINT32(0, armDueSec(noWifi, none, kNow));
    Schedule unlinked = sch; unlinked.linked = false;
    TEST_ASSERT_EQUAL_UINT32(0, armDueSec(unlinked, none, kNow));
    // The site's Retry-After holds a due check back (set clock only).
    Schedule busy = sch; busy.lastChk = kNow - 2 * kDay; busy.serverBackoffTo = kNow + 3 * kHour;
    TEST_ASSERT_EQUAL_UINT32(kNow + 3 * kHour, armDueSec(busy, none, kNow));
    TEST_ASSERT_TRUE(timing(busy, none, kNow).backedOff);
    // An armed schedule is never 0 (0 means "nothing armed"), even at a
    // clock that restarted at 0: the since-reset count starts the interval.
    Schedule fresh = sch; fresh.lastChk = 0;
    TEST_ASSERT_EQUAL_UINT32(kDay, armDueSec(fresh, none, 0));
}

// ---- manual -----------------------------------------------------------------

void test_manual_ignores_policy_and_battery(void) {
    Inputs in;   // nothing saved, policy off, battery unreadable
    in.policy = Policy::Never;
    expect(Verdict::Start, Session::Manual, in);
    in.btIdle = false;
    expect(Verdict::RebootFirst, Session::Manual, in);
    in.sessionRunning = true;
    expect(Verdict::Busy, Session::Manual, in);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_policy_text_defaults_to_auto);
    RUN_TEST(test_boot_starts_when_eligible_without_being_due);
    RUN_TEST(test_boot_blocked_by_each_condition);
    RUN_TEST(test_boot_skips_a_recent_check_in_and_a_running_backoff);
    RUN_TEST(test_battery_gate_needs_both_values);
    RUN_TEST(test_low_battery_blocks_daily_and_awake_radio);
    RUN_TEST(test_daily_needs_due_and_no_backoff);
    RUN_TEST(test_backoff_grows_and_caps);
    RUN_TEST(test_failure_schedules_backoff_success_clears);
    RUN_TEST(test_interval_default_and_bounds);
    RUN_TEST(test_server_time_wins_when_clock_set);
    RUN_TEST(test_clock_reset_falls_back_to_uptime);
    RUN_TEST(test_hourly_wakes_check_in_once_per_interval);
    RUN_TEST(test_hourly_wakes_honour_longer_interval);
    RUN_TEST(test_failed_wakes_back_off);
    RUN_TEST(test_policy_never_makes_no_attempts);
    RUN_TEST(test_awake_runs_only_at_idle_menu_with_bt_idle);
    RUN_TEST(test_awake_check_does_not_duplicate_next_wake);
    RUN_TEST(test_next_due_never_before_backoff);
    RUN_TEST(test_manual_ignores_policy_and_battery);
    RUN_TEST(test_cold_start_does_not_suppress_boot_window);
    RUN_TEST(test_low_battery_wake_keeps_following_wakes_storage_free);
    RUN_TEST(test_arm_gating_and_server_backoff);
    return UNITY_END();
}
