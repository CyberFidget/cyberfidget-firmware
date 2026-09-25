// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#ifndef CHECKIN_POLICY_H
#define CHECKIN_POLICY_H

// When the device may check in. Pure rules over plain inputs: no Arduino
// headers, no clock, no storage, so the native test_upd_policy suite runs
// every decision on the host. The device glue (CheckinScheduler) gathers the
// inputs and acts on the verdict.
//
// Sessions:
//   Boot   - once at power-on / wake, while the start-up animation plays.
//   Daily  - a timer wake from deep sleep whose check-in is due.
//   Awake  - a device kept awake past its interval, from the idle menu.
//   Manual - the person asked for a check now.

#include <stdint.h>

namespace CheckinPolicy {

/// Clock values below this (2020-01-01) mean the clock was never set.
constexpr uint32_t kPlausibleEpoch = 1577836800u;
constexpr uint16_t kDefaultIntervalH = 24;
constexpr uint16_t kMaxIntervalH = 24 * 365;
/// Battery floor for any automatic radio start: both must pass.
constexpr int32_t kMinVbatMv = 3600;
constexpr int32_t kMinSocPct = 20;
/// Failed automatic attempts back off 1 h, 2 h, 4 h ... capped at 24 h.
constexpr uint32_t kFirstBackoffSec = 3600;
constexpr uint32_t kMaxBackoffSec = 86400;
/// The boot window skips a start within this long of the last check-in:
/// a fidget is woken many times a day, and each wake is a start.
constexpr uint32_t kBootMinGapSec = 3600;
/// elapsedSinceCheckin() when nothing tells how long it has been.
constexpr uint32_t kUnknownElapsed = 0xFFFFFFFFu;

enum class Session : uint8_t { Boot, Daily, Awake, Manual };

enum class Policy : uint8_t { Auto, Never };

/// Stored `upd.policy` text: "never" turns automatic check-ins off; anything
/// else (missing, "auto", unknown) is Auto.
Policy parsePolicy(const char* stored);

/// "Check at start-up" (NVS `upd.boot_chk`, u8): missing reads as on, 0 is
/// off. Off only stops the boot session; the daily sleep check-in, a
/// Fidget kept awake past its interval, a manual check and dev mode are
/// unaffected.
constexpr const char* kKeyBootCheck = "boot_chk";
bool parseBootCheck(bool present, uint8_t stored);

enum class Verdict : uint8_t {
    Start,        ///< start the session now
    RebootFirst,  ///< Bluetooth is not idle: restart through the one-shot first
    PolicyOff,    ///< automatic check-ins are off
    BootCheckOff, ///< Boot: "Check at start-up" is off
    StayAwake,    ///< Boot / Awake: Stay awake is on (no automatic network)
    NoWifi,       ///< no saved network
    NotLinked,    ///< nothing to check in for (no link, no pending revoke)
    OneShotBoot,  ///< a restart that relaunches something else
    TimerWake,    ///< the boot window never runs on a timer wake
    LowBattery,   ///< VBAT or SOC below the floor (or unreadable)
    NotDue,       ///< the interval has not passed
    BackedOff,    ///< a failure or server backoff is still running
    NotIdle,      ///< an app or a prompt is in front; try later
    BtNotIdle,    ///< Bluetooth was used; an automatic check waits for sleep
    Busy,         ///< a session is already running
};

struct Inputs {
    Policy policy = Policy::Auto;
    bool wifiSaved = false;
    bool linked = false;        ///< a link, or a revoke still to send
    bool oneShotBoot = false;   ///< Boot: a restart that relaunches something
    bool timerWake = false;     ///< Boot: this start is a deep-sleep timer wake
    bool bootCheck = true;      ///< Boot: "Check at start-up" (upd.boot_chk) is on
    bool stayAwake = false;     ///< Stay awake is latched (Awake & dev mode)
    int32_t vbatMv = -1;        ///< -1 = unreadable
    int32_t socPct = -1;        ///< -1 = unreadable
    bool due = false;           ///< Daily / Awake: the interval has passed
    bool bootGapPassed = true;  ///< Boot: kBootMinGapSec since the last check-in
    bool backedOff = false;     ///< Boot / Daily / Awake: a backoff is still running
    bool atIdleMenu = false;    ///< Awake: the menu is the active app
    bool promptOpen = false;    ///< Awake: a prompt covers the menu
    bool btIdle = true;         ///< the Bluetooth controller never started
    bool sessionRunning = false;
};

/// The one decision every session start goes through.
Verdict decide(Session session, const Inputs& in);

/// Short name for the serial log.
const char* verdictName(Verdict verdict);

/// Both VBAT >= 3.6 V and SOC >= 20 %; an unreadable value fails.
bool batteryEligible(int32_t vbatMv, int32_t socPct);

bool clockPlausible(uint32_t nowSec);

/// 0 or out of range reads as the default (24 h).
uint16_t sanitizeInterval(uint32_t storedHours);

/// Seconds since the last check-in. The stored server time wins when the
/// clock is set and not behind it. Otherwise `localLastSec` (the local clock
/// at the last successful check-in this power-on, 0 = none) is used when the
/// clock has not gone behind it. With neither, an unset clock counts from
/// its reset (the clock runs through deep sleep, so that is time since
/// power-on); a set clock with nothing to compare is kUnknownElapsed.
uint32_t elapsedSinceCheckin(uint32_t nowSec, uint32_t lastChkSec,
                             uint32_t localLastSec);

bool isDue(uint32_t elapsedSec, uint16_t intervalH);

/// Backoff after `failures` consecutive failed automatic attempts (0 = none).
uint32_t backoffSec(uint8_t failures);

/// The clock value (same clock as nowSec) at which the next automatic attempt
/// is due: interval after the last check-in, but never before a running
/// backoff ends. `backoffUntil` is 0 when none. `now` when already due.
uint32_t nextDueSec(uint32_t nowSec, uint32_t elapsedSec, uint16_t intervalH,
                    uint32_t backoffUntil);

/// Bookkeeping across attempts of automatic sessions. Lives in memory that
/// survives deep sleep on the device; plain data here.
struct Backoff {
    uint8_t failures = 0;
    uint32_t until = 0;       ///< clock value; 0 = none
    uint32_t localLast = 0;   ///< clock value at the last success; 0 = none
    /// Clock value before which timer wakes do not look again after a due
    /// wake found the battery too low (0 = none). Only delays timer wakes.
    uint32_t lowBatteryUntil = 0;

    bool running(uint32_t nowSec) const { return until != 0 && nowSec < until; }
    /// A finished automatic session (a real result, not a refused start or
    /// a cancellation). Success clears the backoff and the low-battery hold;
    /// failure grows the backoff. Manual sessions are not recorded here.
    void record(bool ok, uint32_t nowSec);
    /// A due timer wake skipped for low battery: the following wakes stay
    /// storage-free for kLowBatteryHoldSec.
    void holdForLowBattery(uint32_t nowSec);
};

/// How long a low-battery timer wake postpones the next look (a cell under
/// the floor recovers only by charging; the interval is 24 h by default).
constexpr uint32_t kLowBatteryHoldSec = 86400;

/// Stored settings the timing rules read (NVS `upd`, `wificfg`, `pair`).
struct Schedule {
    Policy policy = Policy::Auto;
    bool bootCheck = true;         ///< "Check at start-up" (upd.boot_chk; missing = on)
    bool stayAwake = false;        ///< Stay awake latched (upd.awake == 1)
    bool wifiSaved = false;
    bool linked = false;
    uint16_t intervalH = kDefaultIntervalH;
    uint32_t lastChk = 0;          ///< server time of the last check-in; 0 = none
    uint32_t serverBackoffTo = 0;  ///< the site's Retry-After end; 0 = none
};

struct Timing {
    uint32_t elapsed = kUnknownElapsed;
    bool due = false;
    bool bootGapPassed = true;
    bool backedOff = false;
};

/// Due flag, boot gap and running backoffs at `nowSec`. The boot gap is
/// unmet only with real evidence of a recent check-in (a set clock with a
/// stored server time, or a local success mark this power-on); the
/// since-reset fallback after a cold start counts as passed.
Timing timing(const Schedule& schedule, const Backoff& backoff, uint32_t nowSec);

/// Copies the stored settings and the timing into decision inputs.
void applySchedule(Inputs& in, const Schedule& schedule, const Backoff& backoff,
                   uint32_t nowSec);

/// The due time armed before deep sleep (a clock value, never 0 when
/// armed): interval after the last check-in, not before a running failure
/// or server backoff, nor before a low-battery hold. 0 = nothing to arm
/// (automatic check-ins off, no saved network, or no link).
uint32_t armDueSec(const Schedule& schedule, const Backoff& backoff, uint32_t nowSec);

} // namespace CheckinPolicy

#endif
