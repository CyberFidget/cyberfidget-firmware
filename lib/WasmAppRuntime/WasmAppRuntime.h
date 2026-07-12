// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#ifndef WASM_APP_RUNTIME_H
#define WASM_APP_RUNTIME_H

#include <stddef.h>
#include <stdint.h>

#include "wasm3.h"

// Minimal wasm3 host for the spike/wasm3-apps spike.
//
// Owns one wasm3 environment + runtime + module at a time. Modules are
// expected to be flash-resident byte arrays (rodata is memory-mapped on
// ESP32, so wasm3 parses and executes them in place — no RAM copy of the
// module image is made; wasm3's own allocations (interpreter stack, compiled
// code pages, linear memory) go through the d_m3CustomAllocator hook in
// lib/wasm3 and prefer PSRAM, falling back to internal RAM. See
// docs/SPIKE_WASM_IMPORT_SURFACE.md for details and caveats.
//
// All host functions live in the "cf" import module. Imports that a loaded
// module does not declare are silently skipped, so the bench module and the
// (Stage 2) app modules share one link table.
class WasmAppRuntime {
public:
    static WasmAppRuntime& instance();

    // Interpreter stack for the wasm3 runtime (bytes). Allocated from PSRAM
    // when available.
    static constexpr uint32_t kDefaultStackBytes = 48 * 1024;

    // Parse + load + link a module. `wasmBytes` must stay valid for the
    // lifetime of the module (embed it as a const array in flash).
    // Returns false and sets lastError() on failure.
    bool load(const uint8_t* wasmBytes, size_t len,
              uint32_t stackBytes = kDefaultStackBytes);

    // Tear down module + runtime + environment. Safe to call when unloaded.
    void unload();

    bool isLoaded() const { return module != nullptr; }

    // Microseconds spent in the last successful load() (parse+load+link).
    int64_t lastLoadMicros() const { return loadMicros; }

    // Human-readable error from the last failed call ("" if none).
    const char* lastError() const { return errorMsg ? errorMsg : ""; }

    // Look up an exported function; nullptr (+ lastError) if missing.
    IM3Function findFunction(const char* name);

    // Call helpers for the signatures the spike needs.
    bool callI_I(IM3Function fn, int32_t arg, int32_t* result);  // i32 -> i32
    bool callV_V(IM3Function fn);                                // void -> void

private:
    WasmAppRuntime() = default;
    WasmAppRuntime(const WasmAppRuntime&) = delete;
    WasmAppRuntime& operator=(const WasmAppRuntime&) = delete;

    // Links every "cf" host function into the loaded module (missing imports
    // are tolerated). Returns false + lastError on a real link failure.
    bool linkHostFunctions();

    IM3Environment env     = nullptr;
    IM3Runtime     runtime = nullptr;
    IM3Module      module  = nullptr;  // owned by runtime after m3_LoadModule

    int64_t     loadMicros = 0;
    const char* errorMsg   = nullptr;
};

#endif  // WASM_APP_RUNTIME_H
