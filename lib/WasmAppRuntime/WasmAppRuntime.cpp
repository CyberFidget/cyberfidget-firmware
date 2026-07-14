// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#include "WasmAppRuntime.h"

#include <string.h>

#include <esp_heap_caps.h>
#include <esp_timer.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "WasmHostImports.h"

// ---------------------------------------------------------------------------
// d_m3CustomAllocator implementation (see the spike patch in
// lib/wasm3/src/m3_core.c). Routes every wasm3 heap allocation — interpreter
// stack, compiled-code pages, linear memory — to PSRAM first, falling back to
// the default (internal) heap when PSRAM is absent or exhausted.
//
// Set -DCF_WASM3_PREFER_PSRAM=0 to benchmark with internal RAM instead
// (faster, but eats the ~200KB internal heap the rest of the firmware needs).
// ---------------------------------------------------------------------------

#ifndef CF_WASM3_PREFER_PSRAM
#define CF_WASM3_PREFER_PSRAM 1
#endif

static constexpr bool kPreferPsram = (CF_WASM3_PREFER_PSRAM != 0);

extern "C" void* m3_Malloc_Impl(size_t i_size) {
    void* ptr = nullptr;
    if (kPreferPsram) {
        ptr = heap_caps_calloc(1, i_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (!ptr) {
        ptr = calloc(i_size, 1);
    }
    return ptr;
}

extern "C" void m3_Free_Impl(void* io_ptr) {
    free(io_ptr);  // valid for both internal and PSRAM heap_caps pointers
}

extern "C" void* m3_Realloc_Impl(void* i_ptr, size_t i_newSize, size_t i_oldSize) {
    if (i_newSize == i_oldSize) return i_ptr;

    void* newPtr = nullptr;
    if (kPreferPsram) {
        newPtr = heap_caps_realloc(i_ptr, i_newSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (!newPtr) {
        newPtr = realloc(i_ptr, i_newSize);
    }
    if (newPtr && i_newSize > i_oldSize) {
        // wasm3's stock allocator zero-fills grown regions; linear-memory
        // growth relies on it (fresh wasm pages must read as zero).
        memset(static_cast<uint8_t*>(newPtr) + i_oldSize, 0, i_newSize - i_oldSize);
    }
    return newPtr;
}

// ---------------------------------------------------------------------------
// d_m3NativeStackGuard implementation (see the spike patch in
// lib/wasm3/src/m3_core.h / m3_exec.h). wasm3 recurses on the native (C)
// stack for every wasm->wasm call; without a guard, a deep guest call chain
// (runaway recursion, or just a deep freshly-compiled printf-style chain)
// overruns the task stack -> stack-canary panic -> device reboot. op_Entry
// calls m3_NativeStackExhausted() once per wasm function invocation; when the
// current stack pointer sinks below the armed floor the interpreter traps
// with m3Err_trapStackOverflow, which surfaces through m3_Call* like any
// other trap and lands in WasmAppShell's error path (OLED error screen)
// instead of resetting the device.
//
// The floor sits kNativeStackReserveBytes above the running task's stack
// base, reserving headroom for: host imports called at max guest depth
// (display text rendering is the deepest), wasm3's lazy CompileFunction
// recursion (per block-nesting level, runs before the callee's op_Entry
// check), and the panic/trap unwind path itself.
// ---------------------------------------------------------------------------

#ifndef CF_WASM_NATIVE_STACK_RESERVE
#define CF_WASM_NATIVE_STACK_RESERVE (10 * 1024)
#endif

static volatile uintptr_t s_nativeStackFloor = 0;

extern "C" bool m3_NativeStackExhausted(void) {
    char probe;  // stack grows down on Xtensa: current SP ~= &probe
    return s_nativeStackFloor != 0 && (uintptr_t)&probe <= s_nativeStackFloor;
}

static void armNativeStackGuard() {
#if defined(INCLUDE_pxTaskGetStackStart) && (INCLUDE_pxTaskGetStackStart == 1)
    // Exact: lowest legal address of the calling task's stack.
    uint8_t* base = (uint8_t*)pxTaskGetStackStart(nullptr);
    s_nativeStackFloor = (uintptr_t)base + CF_WASM_NATIVE_STACK_RESERVE;
#else
    // Fallback: budget below the current stack pointer at load time. load()
    // runs at roughly the same depth as the per-frame guest calls (both under
    // AppManager::loop), so this still bounds guest call depth usefully.
    char anchor;
    const uintptr_t budget = 18 * 1024;  // of the 32 KB spike loopTask stack
    s_nativeStackFloor = (uintptr_t)&anchor - budget;
#endif
}

static void disarmNativeStackGuard() { s_nativeStackFloor = 0; }

// ---------------------------------------------------------------------------
// WasmAppRuntime
// ---------------------------------------------------------------------------

WasmAppRuntime& WasmAppRuntime::instance() {
    static WasmAppRuntime singleton;
    return singleton;
}

bool WasmAppRuntime::load(const uint8_t* wasmBytes, size_t len, uint32_t stackBytes) {
    unload();
    errorMsg = nullptr;

    // Guard stays armed for the lifetime of the loaded module; all guest
    // calls happen on the same task (loopTask) that loads modules.
    armNativeStackGuard();

    int64_t t0 = esp_timer_get_time();

    env = m3_NewEnvironment();
    if (!env) {
        errorMsg = "m3_NewEnvironment failed";
        return false;
    }

    runtime = m3_NewRuntime(env, stackBytes, nullptr);
    if (!runtime) {
        errorMsg = "m3_NewRuntime failed";
        unload();
        return false;
    }

    M3Result res = m3_ParseModule(env, &module, wasmBytes, len);
    if (res) {
        errorMsg = res;
        module = nullptr;
        unload();
        return false;
    }

    res = m3_LoadModule(runtime, module);
    if (res) {
        errorMsg = res;
        m3_FreeModule(module);  // runtime did not take ownership on failure
        module = nullptr;
        unload();
        return false;
    }

    if (!linkHostFunctions()) {
        unload();
        return false;
    }

    loadMicros = esp_timer_get_time() - t0;
    return true;
}

void WasmAppRuntime::unload() {
    // Module memory is owned by the runtime once loaded; m3_FreeRuntime
    // releases it.
    if (runtime) {
        m3_FreeRuntime(runtime);
        runtime = nullptr;
    }
    if (env) {
        m3_FreeEnvironment(env);
        env = nullptr;
    }
    module = nullptr;
    disarmNativeStackGuard();
}

bool WasmAppRuntime::linkHostFunctions() {
    int count = 0;
    const WasmHostImport* table = wasmHostImportTable(&count);
    for (int i = 0; i < count; i++) {
        const WasmHostImport& h = table[i];
        M3Result res = m3_LinkRawFunction(module, h.module, h.name, h.sig, h.fn);
        if (res && res != m3Err_functionLookupFailed) {
            errorMsg = res;
            return false;
        }
    }
    return true;
}

IM3Function WasmAppRuntime::findFunction(const char* name) {
    if (!isLoaded()) {
        errorMsg = "no module loaded";
        return nullptr;
    }
    IM3Function fn = nullptr;
    M3Result res = m3_FindFunction(&fn, runtime, name);
    if (res) {
        errorMsg = res;
        return nullptr;
    }
    return fn;
}

bool WasmAppRuntime::callI_I(IM3Function fn, int32_t arg, int32_t* result) {
    if (!fn) return false;
    M3Result res = m3_CallV(fn, arg);
    if (res) {
        errorMsg = res;
        return false;
    }
    res = m3_GetResultsV(fn, result);
    if (res) {
        errorMsg = res;
        return false;
    }
    return true;
}

bool WasmAppRuntime::callV_V(IM3Function fn) {
    if (!fn) return false;
    M3Result res = m3_CallV(fn);
    if (res) {
        errorMsg = res;
        return false;
    }
    return true;
}
