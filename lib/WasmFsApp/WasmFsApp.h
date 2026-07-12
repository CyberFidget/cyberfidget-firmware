// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2026 Dismo Industries LLC

#ifndef WASM_FS_APP_H
#define WASM_FS_APP_H

#include <stddef.h>
#include <stdint.h>

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
// wasmFsAppBegin(). Path must be a confined loadout path (/apps/...).
void setPending(const char* blobPath, const char* label);

// True if a pending launch is staged (used by the test CLI for reporting).
bool hasPending();

// Registry glue (wired via APP_ENTRY in AppManifest.h).
void wasmFsAppBegin();
void wasmFsAppRun();
void wasmFsAppEnd();

// `wasmstat` CLI: one [cmd] line with source/frames/budget/error state.
void statCli();

}  // namespace WasmFsApp

#endif  // WASM_FS_APP_H
