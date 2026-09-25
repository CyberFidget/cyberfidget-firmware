// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#include "CheckinPolicy.h"

#include <string.h>

namespace CheckinPolicy {

Policy parsePolicy(const char* stored) {
    return stored && strcmp(stored, "never") == 0 ? Policy::Never : Policy::Auto;
}

bool parseBootCheck(bool present, uint8_t stored) { return !present || stored != 0; }

bool batteryEligible(int32_t vbatMv, int32_t socPct) {
    return vbatMv >= kMinVbatMv && socPct >= kMinSocPct;
}

Verdict decide(Session session, const Inputs& in) {
    if (in.sessionRunning) return Verdict::Busy;
    if (session == Session::Manual) {
        // Asked for by the person: policy and battery do not apply. After
        // Bluetooth use it must restart first (WiFi then Bluetooth is not
        // allowed in one power cycle).
        return in.btIdle ? Verdict::Start : Verdict::RebootFirst;
    }
    if (in.policy == Policy::Never) return Verdict::PolicyOff;
    // Stay awake never starts the network by itself; a manual check still
    // does (above), and it never sleeps, so no timer wake runs either.
    if (in.stayAwake && (session == Session::Boot || session == Session::Awake))
        return Verdict::StayAwake;
    if (session == Session::Boot) {
        if (!in.bootCheck) return Verdict::BootCheckOff;
        if (in.timerWake) return Verdict::TimerWake;
        if (in.oneShotBoot) return Verdict::OneShotBoot;
    }
    if (!in.wifiSaved) return Verdict::NoWifi;
    if (!in.linked) return Verdict::NotLinked;
    if (session == Session::Boot ? !in.bootGapPassed : !in.due) return Verdict::NotDue;
    if (in.backedOff) return Verdict::BackedOff;
    if (session == Session::Awake && (!in.atIdleMenu || in.promptOpen)) return Verdict::NotIdle;
    if (!in.btIdle) return Verdict::BtNotIdle;
    if (!batteryEligible(in.vbatMv, in.socPct)) return Verdict::LowBattery;
    return Verdict::Start;
}

const char* verdictName(Verdict verdict) {
    switch (verdict) {
        case Verdict::Start:        return "start";
        case Verdict::RebootFirst:  return "reboot-first";
        case Verdict::PolicyOff:    return "policy-off";
        case Verdict::BootCheckOff: return "boot-check-off";
        case Verdict::StayAwake:    return "stay-awake";
        case Verdict::NoWifi:       return "no-wifi";
        case Verdict::NotLinked:    return "not-linked";
        case Verdict::OneShotBoot:  return "one-shot-boot";
        case Verdict::TimerWake:    return "timer-wake";
        case Verdict::LowBattery:   return "low-battery";
        case Verdict::NotDue:       return "not-due";
        case Verdict::BackedOff:    return "backed-off";
        case Verdict::NotIdle:      return "not-idle";
        case Verdict::BtNotIdle:    return "bt-not-idle";
        case Verdict::Busy:         return "busy";
    }
    return "?";
}

bool clockPlausible(uint32_t nowSec) { return nowSec >= kPlausibleEpoch; }

uint16_t sanitizeInterval(uint32_t storedHours) {
    return storedHours == 0 || storedHours > kMaxIntervalH
        ? kDefaultIntervalH : (uint16_t)storedHours;
}

uint32_t elapsedSinceCheckin(uint32_t nowSec, uint32_t lastChkSec,
                             uint32_t localLastSec) {
    if (clockPlausible(nowSec) && lastChkSec && nowSec >= lastChkSec)
        return nowSec - lastChkSec;
    if (localLastSec && nowSec >= localLastSec) return nowSec - localLastSec;
    if (!clockPlausible(nowSec)) return nowSec;
    return kUnknownElapsed;
}

bool isDue(uint32_t elapsedSec, uint16_t intervalH) {
    return elapsedSec >= (uint32_t)sanitizeInterval(intervalH) * 3600u;
}

uint32_t backoffSec(uint8_t failures) {
    if (failures == 0) return 0;
    uint32_t sec = kFirstBackoffSec;
    for (uint8_t i = 1; i < failures && sec < kMaxBackoffSec; ++i) sec *= 2;
    return sec > kMaxBackoffSec ? kMaxBackoffSec : sec;
}

uint32_t nextDueSec(uint32_t nowSec, uint32_t elapsedSec, uint16_t intervalH,
                    uint32_t backoffUntil) {
    const uint32_t interval = (uint32_t)sanitizeInterval(intervalH) * 3600u;
    uint32_t due = nowSec;
    if (elapsedSec < interval) {
        const uint32_t left = interval - elapsedSec;
        due = nowSec > 0xFFFFFFFFu - left ? 0xFFFFFFFFu : nowSec + left;
    }
    return backoffUntil > due ? backoffUntil : due;
}

void Backoff::record(bool ok, uint32_t nowSec) {
    if (ok) {
        failures = 0;
        until = 0;
        lowBatteryUntil = 0;
        localLast = nowSec;
        return;
    }
    if (failures < 0xFF) ++failures;
    const uint32_t wait = backoffSec(failures);
    until = nowSec > 0xFFFFFFFFu - wait ? 0xFFFFFFFFu : nowSec + wait;
}

void Backoff::holdForLowBattery(uint32_t nowSec) {
    lowBatteryUntil = nowSec > 0xFFFFFFFFu - kLowBatteryHoldSec
        ? 0xFFFFFFFFu : nowSec + kLowBatteryHoldSec;
}

Timing timing(const Schedule& schedule, const Backoff& backoff, uint32_t nowSec) {
    Timing t;
    t.elapsed = elapsedSinceCheckin(nowSec, schedule.lastChk, backoff.localLast);
    t.due = isDue(t.elapsed, schedule.intervalH);
    const bool evidence =
        (clockPlausible(nowSec) && schedule.lastChk && nowSec >= schedule.lastChk) ||
        (backoff.localLast && nowSec >= backoff.localLast);
    t.bootGapPassed = !evidence || t.elapsed >= kBootMinGapSec;
    t.backedOff = backoff.running(nowSec) ||
                  (clockPlausible(nowSec) && schedule.serverBackoffTo > nowSec);
    return t;
}

void applySchedule(Inputs& in, const Schedule& schedule, const Backoff& backoff,
                   uint32_t nowSec) {
    in.policy = schedule.policy;
    in.bootCheck = schedule.bootCheck;
    in.stayAwake = schedule.stayAwake;
    in.wifiSaved = schedule.wifiSaved;
    in.linked = schedule.linked;
    const Timing t = timing(schedule, backoff, nowSec);
    in.due = t.due;
    in.bootGapPassed = t.bootGapPassed;
    in.backedOff = t.backedOff;
}

uint32_t armDueSec(const Schedule& schedule, const Backoff& backoff, uint32_t nowSec) {
    if (schedule.policy != Policy::Auto || !schedule.wifiSaved || !schedule.linked) return 0;
    uint32_t until = backoff.running(nowSec) ? backoff.until : 0;
    if (clockPlausible(nowSec) && schedule.serverBackoffTo > nowSec &&
        schedule.serverBackoffTo > until) until = schedule.serverBackoffTo;
    if (backoff.lowBatteryUntil > nowSec && backoff.lowBatteryUntil > until)
        until = backoff.lowBatteryUntil;
    const uint32_t elapsed = elapsedSinceCheckin(nowSec, schedule.lastChk, backoff.localLast);
    const uint32_t due = nextDueSec(nowSec, elapsed, schedule.intervalH, until);
    return due == 0 ? 1 : due;
}

} // namespace CheckinPolicy
