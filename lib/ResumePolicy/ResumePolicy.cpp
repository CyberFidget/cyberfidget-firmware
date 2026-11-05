// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#include "ResumePolicy.h"
#include <string.h>

namespace ResumePolicy {
uint32_t buildId(const char* version, const char* built) {
    uint32_t hash = 2166136261u;
    const char* parts[] = {version, built};
    for (const char* text : parts) {
        while (*text) { hash = (hash ^ (uint8_t)*text++) * 16777619u; }
        hash = hash * 16777619u;
    }
    return hash;
}
bool valid(const Record& r, int appCount, uint32_t build) {
    return r.magic == kMagic && r.version == kVersion && r.appIndex >= 0 &&
           r.appIndex < appCount && r.buildId == build &&
           memchr(r.category, 0, sizeof(r.category)) != nullptr;
}
bool shouldArm(SleepReason reason, int appIndex, bool eligible) {
    return reason == SleepReason::Idle && appIndex >= 0 && eligible;
}
bool makeRecord(Record& out, int appIndex, const char* category, uint32_t build) {
    out = {};
    if (appIndex < 0 || appIndex > INT16_MAX || !category ||
        strlen(category) >= sizeof(out.category)) return false;
    out.version = kVersion;
    out.appIndex = (int16_t)appIndex;
    memcpy(out.category, category, strlen(category) + 1);
    out.buildId = build;
    out.magic = kMagic;
    return true;
}
Decision decide(const BootInputs& in) {
    Decision out;
    // The background path must leave the foreground resume untouched.
    if (in.wake == Wake::Timer) { out.reason = Reason::Timer; return out; }
    out.discardRecord = in.recordPresent || in.resumeOneShot;
    if (!out.discardRecord) return out;
    if (in.explicitOneShot) out.reason = Reason::ExplicitBoot;
    else if (in.imagePending || in.updateVerification) out.reason = Reason::Update;
    else if (in.factoryReset) out.reason = Reason::FactoryReset;
    else if (!in.recordValid || in.appIndex < 0) out.reason = Reason::InvalidRecord;
    else if (in.resumeOneShot) { out.appIndex = in.appIndex; out.reason = Reason::Handoff; }
    else if (in.reset != Reset::DeepSleep) out.reason = Reason::Reset;
    else if (in.wake != Wake::Button) out.reason = Reason::Wake;
    else { out.appIndex = in.appIndex; out.reason = Reason::Button; }
    return out;
}
const char* reasonName(Reason reason) {
    switch (reason) {
        case Reason::NoRecord: return "no-record";
        case Reason::Timer: return "timer";
        case Reason::InvalidRecord: return "invalid-record";
        case Reason::ExplicitBoot: return "explicit-boot";
        case Reason::Update: return "update";
        case Reason::FactoryReset: return "factory-reset";
        case Reason::Reset: return "reset";
        case Reason::Wake: return "wake";
        case Reason::Button: return "idle-button";
        case Reason::Handoff: return "checkin-press";
    }
    return "unknown";
}
} // namespace ResumePolicy
