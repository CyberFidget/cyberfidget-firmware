// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#ifndef CLOUD_SYNC_H
#define CLOUD_SYNC_H

#include <stddef.h>
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
// `applyWaiting` applies waiting app changes in this session even when app
// auto-apply is off (the prompt's "Get them now"); it is not kept across the
// restart after Bluetooth use.
bool runSession(Reason reason, bool applyWaiting = false,
                int32_t dailyVbatMv = -1, int32_t dailySocPct = -1);
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
// True while a scheduled or recovery check-in runs (Boot, Daily, Awake,
// Recovery) - not a Manual check, dev mode listening, or a link worker.
bool automaticSessionRunning();
// True while a session may be changing the app store: any session except
// dev mode listening between its check-ins. Serial transfers wait for it.
bool storeBusy();
// True once a session has switched WiFi on in this power cycle; Bluetooth
// must then wait for a reboot.
bool radioUsedThisPowerCycle();

// ---- Dev mode listening (lib/UpdatePrompt/AwakeMode) ------------------------
// runSession(Reason::Dev) starts a worker that stays joined to the saved
// network and checks in at the site's pace (next_poll_ms, with jitter and a
// backoff after failures) until cancelled. It applies deliveries as they
// arrive, whatever "Apply app changes automatically" says.
struct DevSnapshot {
    bool connected = false;      // joined at the last check-in
    uint32_t polls = 0;          // check-ins this worker has sent
    uint32_t deliveries = 0;     // applies that changed the menu
    uint8_t failures = 0;        // consecutive failed check-ins
    uint32_t lastPollMs = 0;     // millis() when the last check-in ended
    uint32_t heapMin = 0;        // lowest internal free heap over the worker
    uint32_t largestMin = 0;     // smallest largest free block over the worker
    uint32_t restFree = 0;       // internal free heap when the last check-in ended
    char lastBatch[41] = "-";    // the last delivered batch
    Result last;                 // the last check-in's result
};
// The dev worker is running.
bool devListening();
DevSnapshot devSnapshot();
// Ends the wait between check-ins: the next one starts now.
void devPollNow();
bool startLink();
bool startUnlink();
void answerLink(bool accept);
void answerClearApps(bool clear);
LinkSnapshot linkSnapshot();
bool linked(char account[40]);
bool linkStatus(char account[40], bool& fingerprint);
bool hadPreviousAccount();
void resetLinkStatus();

// ---- For the update session (lib/CloudSync/UpdateSession) ----------------
// Routes mbedTLS allocations to PSRAM (internal RAM cannot hold a TLS
// session beside the rest of the firmware); call before the first handshake
// of the power cycle.
bool useExternalTlsMemory();
// The update site: the compiled https site, or (test builds) `upd.base`.
bool siteBase(char* out, size_t len);

struct FetchReply {
    int status = 0;          // HTTP status, 0 when no answer arrived
    int64_t length = -1;     // Content-Length (0 or -1 when not sent)
    uint32_t received = 0;   // body bytes handed to the sink
    bool complete = false;   // the whole body arrived
};
using ChunkSink = bool (*)(void* arg, const uint8_t* data, size_t len);
// One GET without the device credential: the server must end in the
// trusted root list (test builds also allow a plain-HTTP LAN site). Every
// body piece goes to `sink` (false stops). True only when the whole body
// arrived before `deadlineMs` (a millis() value). No redirects.
bool fetchPublic(const char* url, uint32_t callTimeoutMs, uint32_t deadlineMs,
                 ChunkSink sink, void* arg, FetchReply& reply);

#ifdef CF_TEST_CLI
bool setBase(const char* url);
bool setToken(const char* token);
bool forgetLink();
bool setAutoapply(bool enabled);
// Bench only: scheduled sessions look for a network that is not there.
bool setAbsentSsidTest(bool enabled);
// Bench only (RAM): dev mode listening also GETs this public https URL after
// every check-in and prints the internal heap ("" = off).
bool setDevTlsProbe(const char* url);
// Bench only (RAM): the next dev check-in first blocks this long without
// looking at cancellation (a stuck network call); 0 = off.
bool setDevStallMs(uint32_t ms);
#endif

} // namespace CloudSync

#endif
