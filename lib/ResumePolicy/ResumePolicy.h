// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#ifndef RESUME_POLICY_H
#define RESUME_POLICY_H
#include <stddef.h>
#include <stdint.h>

enum class SleepKind : uint8_t { Deep, Light };
enum class SleepReason : uint8_t { Idle, UserOff, EmptyBattery };

namespace ResumePolicy {
constexpr uint32_t kMagic = 0x52534D31u;
constexpr uint16_t kVersion = 1;
constexpr size_t kCategorySize = 64;
constexpr const char* kBootKey = "bootresume";

// Fixed layout for RTC memory and the one-shot used across a software restart.
struct Record {
    uint32_t magic;
    uint16_t version;
    int16_t appIndex;
    char category[kCategorySize];
    uint32_t buildId;
};
static_assert(sizeof(Record) == 76, "Resume record layout changed");
uint32_t buildId(const char* version, const char* built);
bool valid(const Record& record, int appCount, uint32_t build);
bool shouldArm(SleepReason reason, int appIndex, bool eligible);
bool makeRecord(Record& out, int appIndex, const char* category, uint32_t build);

enum class Reset { DeepSleep, PowerOn, Software, Panic, Watchdog, Brownout, Other };
enum class Wake { Button, Timer, Other };
enum class Reason { NoRecord, Timer, InvalidRecord, ExplicitBoot, Update, FactoryReset,
                    Reset, Wake, Button, Handoff };
struct BootInputs {
    Reset reset = Reset::Other;
    Wake wake = Wake::Other;
    bool recordPresent = false;
    bool recordValid = false;
    bool resumeOneShot = false;
    bool explicitOneShot = false;
    bool imagePending = false;
    bool updateVerification = false;
    bool factoryReset = false;
    int appIndex = -1;
};
struct Decision {
    int appIndex = -1;
    bool discardRecord = false;
    Reason reason = Reason::NoRecord;
};
Decision decide(const BootInputs& in);
const char* reasonName(Reason reason);
} // namespace ResumePolicy
#endif
