// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#ifndef SESSION_STATUS_H
#define SESSION_STATUS_H

#include <stdint.h>

namespace CloudSync {

enum class SessionPhase : uint8_t {
    Idle, JoiningWifi, CheckingIn, LookingForUpdate, GettingApps, Waiting, Done, Failed
};
enum class SessionOutcome : uint8_t { Complete, NoChanges, AppsApplied, AppsWaiting, UpdateFound };
struct SessionSnapshot {
    SessionPhase phase = SessionPhase::Idle;
    SessionPhase step = SessionPhase::JoiningWifi; // step held during a wait
    uint32_t current = 0;
    uint32_t total = 0;
    uint32_t secondsLeft = 0;
    bool serverWait = true;
    uint32_t startedMs = 0;
    SessionOutcome outcome = SessionOutcome::Complete;
    char error[32] = "none";
};

} // namespace CloudSync

#endif
