// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#ifndef UPDATE_PROMPT_H
#define UPDATE_PROMPT_H

// Device side of the update prompt and Settings > Updates: reads and writes
// NVS `upd`, opens the prompts, and runs the two screens. The rules (what
// is offered, what an answer stores) are lib/UpdatePolicy/PromptPolicy;
// both are described in lib/UpdatePolicy/README.md.

namespace UpdatePrompt {

/// setup(): the start-up animation plays this boot, so the menu that
/// follows it may show the post-boot popup once.
void armBootPopup();

/// Every loop pass: shows the post-boot popup when the menu first appears.
void loop();

// Root menu "Check for updates" (APP_CHECK_UPDATES).
void checkBegin();
void checkEnd();
void checkUpdate();
/// setup(), after the check screen began: the check that continues after the
/// restart for Bluetooth started (`sessionStarted`), so the screen shows its
/// result once instead of starting another.
void resumeCheck(bool sessionStarted);

// Settings > Updates (APP_UPDATES).
void settingsBegin();
void settingsEnd();
void settingsUpdate();

#ifdef CF_TEST_CLI
/// `upd`: every key in NVS `upd` with its type and value. Read-only.
void printState();
/// `upd offer <version> [source]`: opens the firmware prompt for a stand-in
/// offer (kept in RAM; nothing is stored until an answer says so).
void injectOffer(const char* args);
#endif

} // namespace UpdatePrompt

#endif
