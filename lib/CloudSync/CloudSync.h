// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#ifndef CLOUD_SYNC_H
#define CLOUD_SYNC_H

#include <stdint.h>

namespace CloudSync {

// Boot, Daily and Awake are the scheduled sessions: they never hold WiFi on
// to wait out the server's spacing (the report is deferred to the next
// session) and they give up early when the saved network is not in range.
enum class Reason : uint8_t { Boot, Daily, Manual, Dev, Recovery, Awake };
const char* reasonName(Reason reason);

enum class LinkState : uint8_t { Idle, Starting, Code, Confirm, ClearApps, Confirming, Linked, Unlinked, Declined, Expired, Error };
struct LinkSnapshot {
    LinkState state = LinkState::Idle;
    char code[7] = {0};
    char account[40] = {0};
    char error[24] = {0};
    uint32_t generation = 0;
};

struct Result {
    bool ok = false;
    bool none = false;
    bool waiting = false;
    bool appliedNow = false;   // this session changed the menu
    bool manifestChanged = false;
    bool appsClearFailed = false;
    bool mismatchNotice = false;
    bool unlinkedNotice = false;
    char err[32] = "none";
    char applied[41] = "-";
    char offered[8] = "-";
    uint32_t nextMs = 0;
    uint32_t checkInSec = 0;
    uint32_t heapMin = 0;
    uint32_t largestMin = 0;   // smallest largest-free internal block seen
    uint32_t joinMs = 0;       // 0 when the join did not finish
    uint32_t totalMs = 0;
    Reason reason = Reason::Manual;
};

// Starts one plain FreeRTOS worker or sets a boot one-shot and restarts if
// Bluetooth has already initialized. Completion is consumed from loop().
bool runSession(Reason reason);
// True when a check-in session finished in this call; its result is then
// lastResult() (consumeResult() still hands it to one other reader).
bool poll();
const Result& lastResult();
void recoverFailure();
bool consumeResult(Result& out);
// Stops a running session and waits for WiFi to be off. False when it did
// not stop in time (the caller reboots instead of starting a radio).
bool cancelPending();
void requestCancel();
bool busy();
// True once a session has switched WiFi on in this power cycle; Bluetooth
// must then wait for a reboot.
bool radioUsedThisPowerCycle();
bool startLink();
bool startUnlink();
void answerLink(bool accept);
void answerClearApps(bool clear);
LinkSnapshot linkSnapshot();
bool linked(char account[40]);
bool linkStatus(char account[40], bool& fingerprint);
bool hadPreviousAccount();
void resetLinkStatus();

#ifdef CF_TEST_CLI
bool setBase(const char* url);
bool setToken(const char* token);
bool forgetLink();
bool setAutoapply(bool enabled);
// Bench only: scheduled sessions look for a network that is not there.
bool setAbsentSsidTest(bool enabled);
#endif

} // namespace CloudSync

#endif
