// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#ifndef UPDATE_SESSION_H
#define UPDATE_SESSION_H

#include <stddef.h>
#include <stdint.h>

// Installing an update on the Fidget, and checking a freshly installed one.
// The rules are pure (lib/OtaUpdate); this is the device glue. The flow and
// the bench proof are described in lib/OtaUpdate/README.md.
//
// 1. "Install now" arms a `bootcfg` one-shot and the device restarts.
// 2. That boot runs the update session (runSession) instead of the menu:
//    WiFi station only, never Bluetooth, a wall-clock deadline and the task
//    watchdog. It streams the new image into the other app slot, checks its
//    SHA-256 against the manifest, stores the expected identity
//    (`upd.pend_img`), selects the new slot and restarts. Any failure leaves
//    the running slot selected and restarts into it.
// 3. The new image boots "pending". Its first work in setup() is the
//    self-test (beginSelfTest before hardware start-up, finishBoot after);
//    only a full pass keeps it. A failure, a crash or a watchdog reset before
//    that returns the Fidget to the previous image.

namespace UpdateSession {

/// First line of setup(): notes whether this image is pending verification
/// and, if so, puts setup under the task watchdog until the self-test ends.
void beginSelfTest();

/// Right after hardware start-up. A pending image runs its checks here (and
/// restarts into the previous image on any failure); passing them is not
/// yet kept (see loopTick). Any other image reports a leftover record or a
/// failed session on the status bar.
void finishBoot();

/// Every main-loop pass, first thing. A pending image that passed its
/// checks is kept (marked valid, watchdog back to normal, "Updated to X")
/// only when `frameDrawnLastPass`: the previous pass ran the active app
/// once. Until then it feeds the watchdog and rolls back when the
/// wall-clock budget runs out. Does nothing on a confirmed image.
void loopTick(bool frameDrawnLastPass);

/// True from the first line of pending-image setup until the image is kept.
bool imagePending();

/// Gate every HAL deep-sleep entry. False defers ordinary sleep while the
/// image is pending; a critical-voltage shutdown may keep passed checks or
/// discard the pending record so battery loss cannot set fail_ver.
bool prepareDeepSleep(bool criticalVoltage);

/// App-slot capability sampled once at the start of setup.
bool hasUpdateSlot();

/// The one-shot for the update session, consumed (read and removed).
/// True when this boot must run the session for `version`.
bool takeSessionRequest(char* version, size_t len);

/// The update session. Never returns: it ends in a restart.
[[noreturn]] void runSession(const char* version);

/// Whether this Fidget may install updates today: `upd.unsig_ok`, set only
/// over USB serial (`upd allow-unsigned on`). Until updates are signed, a
/// Fidget without it keeps the "update from the website" message.
bool installAllowed();

/// "Install now": stores the one-shot for `version`. The caller restarts.
/// False (and nothing stored) when installing is not allowed or the
/// version is unusable; `why` then names the reason.
bool armInstall(const char* version, const char** why);

/// Check-in worker, when the site offers firmware: reads the update
/// manifest, runs every gate, and stores `upd.avail` (what the prompt may
/// offer) - or removes it when a gate refuses the release. A fetch that
/// fails leaves the stored offer as it was. Nothing is downloaded.
void refreshOffer(uint32_t deadlineMs);

/// Serial `upd allow-unsigned on|off` (every build: holding the USB cable is
/// the proof). Never settable over the network.
bool setAllowUnsigned(bool allow);

/// Serial `upd slot`: the running and other app slots with their states.
void printSlots();

#ifdef CF_TEST_CLI
/// Serial `upd fault <name>`: a one-shot fault the next pending image (or
/// update session) injects: none | crash | hang | hal-hang | loop-crash |
/// version | mount | session-hang. Stored in the test-only namespace `cftest`.
bool setTestFault(const char* name);
/// Serial `upd seen-clear`: forgets every stored freshness (`upd.seen_*`),
/// so a bench case can start from no update history. Returns the count
/// removed, -1 on a storage error.
int clearSeen();
#endif

}  // namespace UpdateSession

#endif  // UPDATE_SESSION_H
