// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2026 Dismo Industries LLC

#ifndef WASM_FS_APP_H
#define WASM_FS_APP_H

#include <stddef.h>
#include <stdint.h>
#include <string>

// Runs a ferried .wasm app from the LittleFS loadout storage through the
// standard begin/update/end lifecycle. This is the single registry slot
// (APP_ENTRY WASM_HOST) behind EVERY manifest blob entry: the menu (or the
// test CLI's `launch`) stages a pending path+label via setPending() and
// switches to the host app; begin() reads the whole file into a PSRAM
// buffer that stays alive for the module's lifetime (wasm3 parses and
// executes in place - it never copies the image), then hands it to a
// WasmAppShell, which owns all trap containment: any load or runtime
// failure lands on the shell's error screen and a button press returns to
// the menu. Nothing on this path can reboot the device short of a hardware
// watchdog.
namespace WasmFsApp {

// Stage the next launch. Safe from any context; takes effect at the next
// wasmFsAppBegin(). Path must be a confined loadout path (/apps/...). `id`
// is the manifest id (dev mode relaunches a new version of the same id).
void setPending(const char* blobPath, const char* label, int abi, const char* id = "");

// The last app begun in this power cycle (it may have ended since): its
// manifest id and file. False when none was, or it had no id.
bool lastLaunch(std::string& id, std::string& path);

// True if a pending launch is staged (used by the test CLI for reporting).
bool hasPending();

// Whether the currently staged blob ABI is provided by this firmware.
bool pendingAbiSupported();
// Includes unsupported imports discovered while linking the last launch.
bool abiUnsupported();

// The staged launch's manifest id and label (false when nothing is staged).
bool pendingLaunch(std::string& id, std::string& label);

// Whether the guest task (a small internal-RAM stack) and the interpreter's
// stack (PSRAM) can be allocated right now. When they cannot, the app opens
// in a fresh start instead (AppManager).
bool guestStackFits();
/// True when only internal RAM is short, so a fresh start would let the app open.
bool guestRestartHelps();
#ifdef CF_TEST_CLI
/// Test builds: force guestRestartHelps() true (kept across a software restart).
void testForceRestart(bool on);
#endif

// Registry glue (wired via APP_ENTRY in AppManifest.h).
void wasmFsAppBegin();
void wasmFsAppRun();
void wasmFsAppEnd();

// `wasmstat` CLI: one [cmd] line with source/frames/budget/error state.
void statCli();

}  // namespace WasmFsApp

#endif  // WASM_FS_APP_H
