// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2026 Dismo Industries LLC
#include <emscripten.h>
#include <emscripten/heap.h>
#include <stdlib.h>
#include "AudioManager.h"
#include "WasmHostFunctions.h"
#include "../device_module/cf_params.h"

// A call must copy its guest data here synchronously. Sequence playback makes
// its own persistent copy in cf_host; no caller pointer is retained.
alignas(8) static uint8_t transferBuffer[2048];
extern "C" {
EMSCRIPTEN_KEEPALIVE uint8_t* wasm_module_buffer() { return transferBuffer; }
EMSCRIPTEN_KEEPALIVE int wasm_module_buffer_size() { return sizeof(transferBuffer); }
}

static void checkCoreRange(const void* ptr, uint32_t bytes) {
    if ((uint64_t)(uintptr_t)ptr + bytes > emscripten_get_heap_size()) abort();
}
#define CF_CHECK_A(type, name, bytes)
#define CF_CHECK_B(type, name, bytes)
#define CF_CHECK_PA(type, name, bytes) checkCoreRange(name, bytes);
#define CF_CHECK_PB(type, name, bytes) CF_CHECK_PA(type, name, bytes)
#define CF_POLICY_PLAIN
#define CF_POLICY_STRING95
#define CF_POLICY_STRING127
#define CF_POLICY_SEQUENCE count = cf_host::sequenceCount(count);
#define CF_POLICY_XBM if (!cf_host::xbmByteLength(w, h)) return;
#define CF_EXTRA_PLAIN
#define CF_EXTRA_STRING95
#define CF_EXTRA_STRING127
#define CF_EXTRA_SEQUENCE
#define CF_EXTRA_XBM , int32_t byteLen
#define CF_PASS_PLAIN
#define CF_PASS_STRING95
#define CF_PASS_STRING127
#define CF_PASS_SEQUENCE
#define CF_PASS_XBM , byteLen
extern "C" {
#define CF_ROW(name, ret, sig, policy, args) \
    EMSCRIPTEN_KEEPALIVE ret cf_##name(CF_PARAMS(DECL, args) CF_EXTRA_##policy) { \
        CF_POLICY_##policy CF_PARAMS(CHECK, args) \
        return cf_host::name(CF_PARAMS(CALL, args) CF_PASS_##policy); \
    }
#define CF_STUB(module, name, ret, sig, fn, policy, args)
#include "../device_module/cf_imports.def"
}
