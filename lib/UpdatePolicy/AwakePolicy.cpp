// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#include "AwakePolicy.h"

#include <stdio.h>
#include <string.h>

namespace AwakePolicy {

bool sameSetting(const Setting& a, const Setting& b) {
    return a.mode == b.mode && a.stop == b.stop;
}

namespace {
Mode modeFrom(uint8_t v) {
    return v == 1 ? Mode::StayAwake : v == 2 ? Mode::Dev : Mode::Off;
}
Stop stopFrom(uint8_t v) {
    return v == 1 ? Stop::UntilStopped : Stop::AfterIdle;
}
} // namespace

Parsed parseStored(const Stored& st) {
    Parsed p;
    if (st.hasMode) {
        p.setting.mode = modeFrom(st.mode);
        p.setting.stop = st.hasStop ? stopFrom(st.stop) : Stop::AfterIdle;
    } else if (st.hasLegacyDev) {
        if (st.legacyDev == 1) p.setting = {Mode::Dev, Stop::AfterIdle};
        else if (st.legacyDev == 2) p.setting = {Mode::Dev, Stop::UntilStopped};
    }
    p.migrate = !st.hasMode && st.hasLegacyDev;
    return p;
}

bool keepsAwake(const Setting& s) { return s.mode != Mode::Off; }

const char* wireMode(const Setting& s) {
    if (s.mode != Mode::Dev) return "normal";
    return s.stop == Stop::UntilStopped ? "always" : "dev";
}

bool listensThisBoot(const Setting& s, bool bluetoothAppBoot) {
    return s.mode == Mode::Dev && !bluetoothAppBoot;
}

bool releasesBluetoothAtBoot(const Setting& s, bool bluetoothAppBoot) {
    return listensThisBoot(s, bluetoothAppBoot);
}

bool bluetoothAppNeedsRestart(bool listening) { return listening; }

bool batteryLow(int32_t vbatMv, int32_t socPct, bool charging) {
    if (charging) return false;
    return (vbatMv >= 0 && vbatMv < kFloorVbatMv) || (socPct >= 0 && socPct < kFloorSocPct);
}

End checkEnd(const Setting& s, const Activity& a, uint32_t idleStopMs, uint32_t safetyNetMs) {
    if (!keepsAwake(s)) return End::None;
    if (a.lowBatteryForMs >= kLowBatteryHoldMs) return End::Battery;
    if (a.sincePressMs >= safetyNetMs) return End::SafetyNet;
    if (s.stop == Stop::AfterIdle && a.sinceUseMs >= idleStopMs) return End::Idle;
    return End::None;
}

const char* endTitle(Mode mode) {
    return mode == Mode::Dev ? "Dev mode is off" : "Stay awake is off";
}

const char* endReason(End end) {
    switch (end) {
        case End::Battery:   return "Battery low";
        case End::SafetyNet: return "No button press for 2 days";
        case End::Idle:      return "Not used for 30 min";
        case End::RestartLoop: return "It kept restarting";
        default:             return "";
    }
}

const char* endName(End end) {
    switch (end) {
        case End::Battery:   return "battery";
        case End::SafetyNet: return "safety-net";
        case End::Idle:      return "idle";
        case End::RestartLoop: return "restart-loop";
        default:             return "none";
    }
}

Apply applyEffect(const Setting& before, const Setting& after) {
    if (sameSetting(before, after)) return Apply::Nothing;
    if (before.mode == Mode::Dev || after.mode == Mode::Dev) return Apply::SaveAndRestart;
    return Apply::Save;
}

bool listensDuring(const char* name) {
    static const char* const kAllowed[] = {
        "APP_MENU", "APP_BOOT_ANIMATION", "APP_STATUS", "APP_UPDATES", "APP_CHECK_UPDATES",
        "APP_AWAKE", "APP_SPH_FLUID_GAME", "APP_SNAKE",
    };
    if (!name) return false;
    for (const char* a : kAllowed)
        if (strcmp(a, name) == 0) return true;
    return false;
}

uint8_t nextLoopCount(uint8_t previous, bool abnormalReset, bool awakeLatched) {
    if (!awakeLatched || !abnormalReset) return 0;
    return previous < 0xFF ? previous + 1 : previous;
}

bool loopEndsMode(uint8_t count) { return count >= kLoopLimit; }

bool loopCountClears(uint32_t uptimeMs, bool devCheckedIn) {
    return devCheckedIn || uptimeMs >= kLoopCleanMs;
}

namespace {
constexpr const char* kAfterIdle = "After 30 min without use";
constexpr const char* kUntilStopped = "Until I stop it";

const Choice kChoiceTable[kChoices] = {
    {{Mode::Off, Stop::AfterIdle}, "Off", "",
     "Your Fidget sleeps after a minute without use and checks for changes once a day. "
     "Best for battery."},
    {{Mode::StayAwake, Stop::AfterIdle}, "Stay awake", kAfterIdle,
     "Your Fidget never sleeps on its own. It uses more battery. Bluetooth apps work as "
     "usual. It checks for changes only when you ask. Turns off after 30 min without use, "
     "or when the battery is low."},
    {{Mode::StayAwake, Stop::UntilStopped}, "Stay awake", kUntilStopped,
     "Your Fidget never sleeps on its own. It uses more battery. Bluetooth apps work as "
     "usual. It checks for changes only when you ask. Turns off after 2 days without a "
     "button press, or when the battery is low."},
    {{Mode::Dev, Stop::AfterIdle}, "Dev mode", kAfterIdle,
     "Dev mode keeps your Fidget awake and connected, so apps you send arrive in seconds. "
     "It uses more battery. Bluetooth apps need a restart. Turns off after 30 min without "
     "use, or when the battery is low."},
    {{Mode::Dev, Stop::UntilStopped}, "Dev mode", kUntilStopped,
     "Dev mode keeps your Fidget awake and connected, so apps you send arrive in seconds. "
     "It uses more battery. Bluetooth apps need a restart. Turns off after 2 days without "
     "a button press, or when the battery is low."},
};
} // namespace

const Choice& choice(int index) {
    return kChoiceTable[index >= 0 && index < kChoices ? index : 0];
}

int choiceIndex(const Setting& s) {
    for (int i = 0; i < kChoices; i++) {
        const Setting& c = kChoiceTable[i].setting;
        if (c.mode == s.mode && (s.mode == Mode::Off || c.stop == s.stop)) return i;
    }
    return 0;
}

int stepChoice(int index, int direction) {
    const int i = index >= 0 && index < kChoices ? index : 0;
    return (i + (direction < 0 ? kChoices - 1 : 1)) % kChoices;
}

const char* modeName(Mode mode) {
    switch (mode) {
        case Mode::StayAwake: return "Stay awake";
        case Mode::Dev:       return "Dev mode";
        default:              return "Off";
    }
}

void bluetoothPromptTitle(char* out, size_t len, const char* appName) {
    snprintf(out, len, "%s uses Bluetooth. Restart without dev mode? "
             "Dev mode comes back next restart.", appName && appName[0] ? appName : "This app");
}

Marker marker(const Setting& s, bool listening) {
    if (!keepsAwake(s)) return Marker::None;
    return s.mode == Mode::Dev && listening ? Marker::Listening : Marker::Awake;
}

uint8_t breathLevel(uint32_t nowMs) {
    const uint32_t half = kBreathPeriodMs / 2;
    const uint32_t t = nowMs % kBreathPeriodMs;
    const uint32_t up = t < half ? t : kBreathPeriodMs - t;
    return (uint8_t)((up * kBreathMax) / half);
}

} // namespace AwakePolicy
