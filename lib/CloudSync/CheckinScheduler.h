// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#ifndef CHECKIN_SCHEDULER_H
#define CHECKIN_SCHEDULER_H

// Device glue for CheckinPolicy: reads the stored settings, battery and
// radio state, asks the policy, and starts CloudSync sessions. See
// lib/UpdatePolicy/README.md.

#include <stdint.h>

#include "CloudSync.h"

namespace CheckinScheduler {

/// Once, from the first loop pass (the battery has been read by then),
/// while the start-up animation plays. `oneShotBoot` is true for a restart
/// that relaunches something (portal, Music Player, link, cloud, skipanim).
void startBootWindow(bool oneShotBoot);

/// Every loop pass: routes finished sessions and runs the awake deadline.
void loop();

/// The on-demand check (root and Settings "Check for updates"). Allowed
/// when automatic checks are off; after Bluetooth use it restarts first.
/// False when a session is already running. `applyWaiting` is the
/// prompt's "Get them now" (see CloudSync::runSession).
bool checkNow(bool applyWaiting = false);

/// A boot-window result that arrived while the start-up animation was still
/// playing: the menu may show it as a popup once. A later result only goes
/// to the status bar.
bool takeBootResult(CloudSync::Result& out);

/// Arms the next background check-in before deep sleep (HAL hook).
void armBeforeSleep();

/// A timer wake with a check-in due: one headless session, then deep sleep
/// again whatever the outcome.
[[noreturn]] void runHeadless(int32_t vcellMv, int32_t socPct);

#ifdef CF_TEST_CLI
bool setIntervalHours(uint32_t hours);
/// Moves the stored last check-in so the next one is due in `seconds`.
bool setDueIn(uint32_t seconds);
/// Headless wall-clock guard override in ms (0 = the built-in value).
bool setGuardMs(uint32_t ms);
/// The next headless check-in behaves as if the wake button were pressed.
bool setPressTest();
#endif

} // namespace CheckinScheduler

#endif
