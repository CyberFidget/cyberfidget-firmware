// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#ifndef WASM_HOST_IMPORTS_H
#define WASM_HOST_IMPORTS_H

#include "../../wasm/device_module/cf_hal_abi.h"
#include "wasm3.h"

// Firmware-provided cf.* HAL ABI. This is bound directly to the guest's
// source-of-truth definition so the two sides cannot silently drift.
constexpr int kDeviceHalAbi = CF_HAL_ABI;

// One row of the host import table. Every loaded module gets the full table
// offered; imports the module doesn't declare are skipped.
struct WasmHostImport {
    const char* module;  // wasm import module ("cf", "wasi_snapshot_preview1", "env")
    const char* name;
    const char* sig;     // wasm3 signature string
    M3RawCall   fn;
};

// The full host surface (see docs/SPIKE_WASM_IMPORT_SURFACE.md).
const WasmHostImport* wasmHostImportTable(int* outCount);

// cf.exit_to_menu sets a deferred flag (unloading the runtime mid-guest-call
// would free the interpreter under our feet). The shell polls + clears it
// after each guest call returns.
bool wasmHostConsumeExitRequest();
void wasmHostClearExitRequest();

#endif  // WASM_HOST_IMPORTS_H
