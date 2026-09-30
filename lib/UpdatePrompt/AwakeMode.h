// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#ifndef AWAKE_MODE_H
#define AWAKE_MODE_H

// Device side of "Awake & dev mode" (rules: lib/UpdatePolicy/AwakePolicy,
// described in lib/UpdatePolicy/README.md): reads and writes the setting,
// keeps the Fidget awake, ends the mode by itself, runs dev mode listening
// (starts the CloudSync dev worker, relaunches a delivered app, breathes the
// back LED, keeps the status bar marker), asks before a Bluetooth app, and
// draws the Settings screen.

#include "AppDefs.h"
#include "AwakePolicy.h"

namespace AwakeMode {

/// setup(), after the start-up one-shots are read and before the first app
/// begins. `bluetoothAppBoot`: this start relaunches the Music Player.
/// In a dev mode power cycle it frees the Bluetooth memory here.
void beginBoot(bool bluetoothAppBoot);

/// Every loop pass, before the check-in scheduler.
void loop();

/// Every button event (a press is use; 48 h without one ends the mode).
void noteButton();

/// AppManager::switchToApp, before anything else: true when the switch is
/// handled here instead (a Bluetooth app asks to restart first).
bool interceptSwitch(AppIndex newApp);

AwakePolicy::Setting setting();
/// Dev mode listens in this power cycle (the worker may be between starts).
bool listening();

// Settings > Awake & dev mode (APP_AWAKE).
void screenBegin();
void screenEnd();
void screenUpdate();

#ifdef CF_TEST_CLI
/// `awake ...` bench verbs (see lib/SerialCli/README.md).
void cliCommand(const char* args);
#endif

} // namespace AwakeMode

#endif
