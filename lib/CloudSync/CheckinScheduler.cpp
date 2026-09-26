// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

// Device only: the pure rules it applies are lib/UpdatePolicy (host-tested).
#ifndef HOST_TEST

#include "CheckinScheduler.h"
#include "CheckinPolicy.h"
#include "AwakePolicy.h"

#include <Arduino.h>
#include <Preferences.h>
#include <esp_bt.h>
#include <time.h>
#ifdef CF_TEST_CLI
#include <WiFi.h>
#endif

#include "AppDefs.h"
#include "AppManager.h"
#include "DeviceIdentity.h"
#include "HAL.h"
#include "LoadoutStore.h"
#include "ModalPrompt.h"
#include "SavedWifi.h"
#include "UpdateSession.h"

namespace CheckinScheduler {
namespace {
using namespace CheckinPolicy;

// Wall-clock guard for the headless wake: the session's own 60 s budget
// plus room for its last call and WiFi shutdown. Past it the session is
// cancelled and the device goes back to sleep regardless.
constexpr uint32_t kHeadlessGuardMs = 75000;
constexpr uint32_t kHeadlessCancelMs = 10000;
// A press of the wake button during a headless check-in stops it and starts
// normally: the person wants the Fidget, not a silent check.
constexpr uint32_t kPressRestartCancelMs = 5000;
// How often the awake deadline is looked at.
constexpr uint32_t kAwakeTickMs = 30000;

// Automatic-session backoff and the last success on the local clock. RTC
// memory: kept through deep sleep, cleared by a power cycle or restart.
constexpr uint32_t kBackoffMagic = 0x43484B42u;
RTC_DATA_ATTR uint32_t rtcMagic;
RTC_DATA_ATTR Backoff rtcBackoff;

Backoff& backoff() {
    if (rtcMagic != kBackoffMagic) {
        rtcBackoff = Backoff();
        rtcMagic = kBackoffMagic;
    }
    return rtcBackoff;
}

bool earlyPending = false;
CloudSync::Result earlyResult;
uint32_t lastAwakeTick = 0;
Verdict lastAwakeLogged = Verdict::Start;

Schedule readStored() {
    Schedule st;
    Preferences upd;
    if (upd.begin("upd", true)) {
        char policy[8] = {0};
        if (upd.isKey("policy")) upd.getString("policy", policy, sizeof(policy));
        st.policy = parsePolicy(policy);
        const bool hasBootCheck = upd.isKey(kKeyBootCheck);
        st.bootCheck = parseBootCheck(hasBootCheck, hasBootCheck ? upd.getUChar(kKeyBootCheck, 1) : 1);
        st.stayAwake = upd.isKey(AwakePolicy::kKeyMode) &&
                       upd.getUChar(AwakePolicy::kKeyMode, 0) == (uint8_t)AwakePolicy::Mode::StayAwake;
        st.intervalH = sanitizeInterval(upd.getUInt("interval_h", kDefaultIntervalH));
        st.lastChk = upd.getUInt("last_chk", 0);
        st.serverBackoffTo = upd.getUInt("backoff_to", 0);
        upd.end();
    }
    st.wifiSaved = SavedWifi::anySaved();
    // Read-only check (no clean-up writes): this runs on every awake tick.
    char account[40];
    bool fingerprint = false;
    st.linked = CloudSync::linkStatus(account, fingerprint);
    return st;
}

uint32_t nowSec() { return (uint32_t)time(nullptr); }

// Stored settings, timing (CheckinPolicy::applySchedule) and radio state.
Inputs baseInputs(const Schedule& st) {
    Inputs in;
    applySchedule(in, st, backoff(), nowSec());
    in.btIdle = esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_IDLE;
    in.sessionRunning = CloudSync::busy();
    return in;
}

int32_t awakeVbatMv() {
    return batteryVoltage >= 2.0f && batteryVoltage <= 4.6f
        ? (int32_t)(batteryVoltage * 1000.0f + 0.5f) : -1;
}
int32_t awakeSocPct() {
    return batteryVoltagePercentage >= 0.0f && batteryVoltagePercentage <= 110.0f
        ? (int32_t)batteryVoltagePercentage : -1;
}

void logSkip(const char* reason, Verdict v, int32_t vbat, int32_t soc) {
    Serial.printf("[checkin] reason=%s skip=%s vbat_mv=%ld soc=%ld\n",
                  reason, verdictName(v), (long)vbat, (long)soc);
}

bool automatic(CloudSync::Reason r) {
    return r == CloudSync::Reason::Boot || r == CloudSync::Reason::Daily ||
           r == CloudSync::Reason::Awake;
}

// A finished session: automatic ones feed the backoff (a session stopped
// for a radio app, or otherwise cancelled, is not a failure); an early boot
// result is kept for the post-animation popup.
void finished(const CloudSync::Result& r) {
    if (automatic(r.reason) && strcmp(r.err, "cancelled") != 0)
        backoff().record(r.ok, nowSec());
    if (r.reason == CloudSync::Reason::Boot) {
        const bool early = AppManager::instance().activeApp() == APP_BOOT_ANIMATION;
        if (early) { earlyResult = r; earlyPending = true; }
        Serial.printf("[checkin] boot-result=%s\n", early ? "early" : "late");
    }
}

uint32_t guardMs() {
#ifdef CF_TEST_CLI
    Preferences test;
    uint32_t ms = 0;
    if (test.begin("cftest", true)) {
        ms = test.getUInt("guard_ms", 0);
        test.end();
    }
    if (ms) return ms;
#endif
    return kHeadlessGuardMs;
}

#ifdef CF_TEST_CLI
// Bench: a one-shot stand-in for a press during the session (`cloud press`),
// read once per headless wake and fired once the session has WiFi on.
bool fakePressArmed = false;
void loadFakePress() {
    Preferences test;
    if (!test.begin("cftest", false)) return;
    fakePressArmed = test.getBool("press", false);
    if (fakePressArmed) test.remove("press");
    test.end();
}
#endif

// The wake button (the ext0 wake pin), active low.
bool wakeButtonPressed(bool duringSession) {
#ifdef CF_TEST_CLI
    if (fakePressArmed && duringSession && WiFi.getMode() != WIFI_OFF) return true;
#else
    (void)duringSession;
#endif
    if (digitalRead(button_BottomRight) != LOW) return false;
    delay(30);
    return digitalRead(button_BottomRight) == LOW;
}

[[noreturn]] void restartForPress() {
    CloudSync::requestCancel();
    const uint32_t at = millis();
    while (CloudSync::busy() && millis() - at < kPressRestartCancelMs) delay(20);
    Serial.printf("[checkin] reason=daily button=pressed stopped=%d restart=normal\n",
                  CloudSync::busy() ? 0 : 1);
    Serial.flush();
    // A software restart is not a timer wake: the next start is a normal one.
    esp_restart();
    for (;;) {}
}
} // namespace

void startBootWindow(bool oneShotBoot) {
    const Schedule st = readStored();
    Inputs in = baseInputs(st);
    in.oneShotBoot = oneShotBoot || AppManager::instance().activeApp() != APP_BOOT_ANIMATION;
    in.imagePending = UpdateSession::imagePending();
    in.timerWake = strcmp(HAL::bootWakeupCauseName(), "timer") == 0;
    in.vbatMv = awakeVbatMv();
    in.socPct = awakeSocPct();
    const Verdict v = decide(Session::Boot, in);
    if (v != Verdict::Start) { logSkip("boot", v, in.vbatMv, in.socPct); return; }
    // The reading also gates the once-a-day battery data upload.
    if (!CloudSync::runSession(CloudSync::Reason::Boot, false, in.vbatMv, in.socPct))
        logSkip("boot", Verdict::Busy, in.vbatMv, in.socPct);
}

void loop() {
    if (CloudSync::poll()) finished(CloudSync::lastResult());
    if (millis() - lastAwakeTick < kAwakeTickMs) return;
    lastAwakeTick = millis();
    if (CloudSync::busy()) return;
    const Schedule st = readStored();
    Inputs in = baseInputs(st);
    in.atIdleMenu = AppManager::instance().activeApp() == APP_MENU;
    in.promptOpen = ModalPrompt::instance().isOpen();
    in.vbatMv = awakeVbatMv();
    in.socPct = awakeSocPct();
    const Verdict v = decide(Session::Awake, in);
    // Only an overdue check that is being held back is worth a line, and
    // only when the reason changes.
    const bool heldBack = in.due &&
        (v == Verdict::NotIdle || v == Verdict::BtNotIdle || v == Verdict::LowBattery);
    if (heldBack && v != lastAwakeLogged) logSkip("awake", v, in.vbatMv, in.socPct);
    lastAwakeLogged = heldBack ? v : Verdict::Start;
    if (v != Verdict::Start) return;
    // A refused start (busy serial transfer or probe) is not a failure.
    CloudSync::runSession(CloudSync::Reason::Awake);
}

bool checkNow(bool applyWaiting) {
    Inputs in;
    in.sessionRunning = CloudSync::busy();
    in.btIdle = esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_IDLE;
    if (decide(Session::Manual, in) == Verdict::Busy) return false;
    // After Bluetooth use runSession restarts through the one-shot first.
    return CloudSync::runSession(CloudSync::Reason::Manual, applyWaiting);
}

bool takeBootResult(CloudSync::Result& out) {
    if (!earlyPending) return false;
    out = earlyResult;
    earlyPending = false;
    return true;
}

void armBeforeSleep() {
    // A cell that was charged while awake no longer needs the low-battery
    // hold (a headless wake has no awake reading, so it never clears here).
    if (batteryEligible(awakeVbatMv(), awakeSocPct())) backoff().lowBatteryUntil = 0;
    HAL::setTimerCheckinDue(armDueSec(readStored(), backoff(), nowSec()));
}

void runHeadless(int32_t vcellMv, int32_t socPct) {
    const uint32_t started = millis();
    pinMode(button_BottomRight, INPUT_PULLUP);
#ifdef CF_TEST_CLI
    loadFakePress();
#endif
    if (wakeButtonPressed(false)) restartForPress();
    // Decide from settings and RTC state first: the filesystem is mounted
    // and the link cleaned up only for a session that will run.
    const Schedule st = readStored();
    Inputs in = baseInputs(st);
    in.vbatMv = vcellMv;
    in.socPct = socPct;
    const Verdict v = decide(Session::Daily, in);
    bool flushOk = true;
    if (v != Verdict::Start) {
        logSkip("daily", v, vcellMv, socPct);
        // Keep the following wakes storage-free while the cell is low.
        if (v == Verdict::LowBattery) backoff().holdForLowBattery(nowSec());
    } else {
        DeviceIdentity::checkStored();
        LoadoutStore::begin();
        if (!CloudSync::runSession(CloudSync::Reason::Daily, false, vcellMv, socPct)) {
            // Refused to start: not a failed session, nothing recorded.
            logSkip("daily", Verdict::Busy, vcellMv, socPct);
        } else {
            const uint32_t guard = guardMs();
            while (CloudSync::busy() && millis() - started < guard) {
                if (wakeButtonPressed(true)) restartForPress();
                delay(100);
            }
            if (CloudSync::busy()) {
                // The session overran its own deadline: stop it, and sleep
                // regardless (deep sleep powers the radio down).
                CloudSync::requestCancel();
                const uint32_t cancelAt = millis();
                while (CloudSync::busy() && millis() - cancelAt < kHeadlessCancelMs) delay(50);
                const bool stopped = !CloudSync::busy();
                Serial.printf("[checkin] reason=daily guard=fired guard_ms=%u stopped=%d\n",
                              (unsigned)guard, stopped ? 1 : 0);
                backoff().record(false, nowSec());
                // A worker that is still running may be writing: no diary flush.
                flushOk = stopped;
            } else if (CloudSync::poll()) {
                finished(CloudSync::lastResult());
            }
        }
    }
    armBeforeSleep();
    Serial.printf("[checkin] headless_ms=%u\n", (unsigned)(millis() - started));
    HAL::resleepAfterTimerCheckin(flushOk);
}

#ifdef CF_TEST_CLI
bool setIntervalHours(uint32_t hours) {
    if (hours == 0 || hours > kMaxIntervalH) return false;
    Preferences upd;
    if (!upd.begin("upd", false)) return false;
    const bool ok = upd.putUInt("interval_h", hours) != 0;
    upd.end();
    return ok;
}

bool setDueIn(uint32_t seconds) {
    const uint32_t now = nowSec();
    if (!clockPlausible(now) || CloudSync::busy()) return false;
    Preferences upd;
    if (!upd.begin("upd", false)) return false;
    const uint32_t interval = (uint32_t)sanitizeInterval(upd.getUInt("interval_h", kDefaultIntervalH)) * 3600u;
    const uint32_t last = seconds >= interval ? now : now - interval + seconds;
    const bool ok = upd.putUInt("last_chk", last) != 0;
    upd.end();
    // A bench reset of the schedule: no backoff and no local success mark.
    backoff() = Backoff();
    return ok;
}

bool setPressTest() {
    Preferences test;
    if (!test.begin("cftest", false)) return false;
    const bool ok = test.putBool("press", true) != 0;
    test.end();
    return ok;
}

bool setGuardMs(uint32_t ms) {
    Preferences test;
    if (!test.begin("cftest", false)) return false;
    const bool ok = ms ? test.putUInt("guard_ms", ms) != 0
                       : (!test.isKey("guard_ms") || test.remove("guard_ms"));
    test.end();
    return ok;
}
#endif

} // namespace CheckinScheduler

#endif // HOST_TEST
