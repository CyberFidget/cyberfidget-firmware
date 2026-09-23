// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#ifndef CLOUD_SYNC_H
#define CLOUD_SYNC_H

#include <stdint.h>

namespace CloudSync {

enum class Reason : uint8_t { Boot, Daily, Manual, Dev, Recovery };

struct Result {
    bool ok = false;
    bool none = false;
    bool waiting = false;
    bool appliedNow = false;   // this session changed the menu
    char err[32] = "none";
    char applied[41] = "-";
    char offered[8] = "-";
    uint32_t nextMs = 0;
    uint32_t checkInSec = 0;
    uint32_t heapMin = 0;
};

// Starts one plain FreeRTOS worker or sets a boot one-shot and restarts if
// Bluetooth has already initialized. Completion is consumed from loop().
bool runSession(Reason reason);
void poll();
void recoverFailure();
bool consumeResult(Result& out);
// Stops a running session and waits for WiFi to be off. False when it did
// not stop in time (the caller reboots instead of starting a radio).
bool cancelPending();
bool busy();
// True once a session has switched WiFi on in this power cycle; Bluetooth
// must then wait for a reboot.
bool radioUsedThisPowerCycle();

#ifdef CF_TEST_CLI
bool setBase(const char* url);
bool setToken(const char* token);
bool setAutoapply(bool enabled);
#endif

} // namespace CloudSync

#endif
