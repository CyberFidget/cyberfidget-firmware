// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC
#ifndef CF_HOST_FUNCTIONS_H
#define CF_HOST_FUNCTIONS_H
#include <stdint.h>

bool wasmHostConsumeExitRequest();
void wasmHostClearExitRequest();

namespace cf_host {
// Invalid XBM dimensions are a no-op, before checking even a zero-byte range.
inline uint32_t xbmByteLength(int32_t w, int32_t h) {
    return (w > 0 && h > 0 && w <= 128 && h <= 64) ? ((w + 7) / 8) * h : 0;
}
inline int32_t sequenceCount(int32_t count) {
    return count < 0 ? 0 : (count > 64 ? 64 : count);
}
#include "../../wasm/device_module/cf_params.h"
#define CF_EXTRA_PLAIN
#define CF_EXTRA_STRING95
#define CF_EXTRA_STRING127
#define CF_EXTRA_SEQUENCE
#define CF_EXTRA_XBM , int32_t byteLen
#define CF_ROW(name, ret, sig, policy, args) ret name(CF_PARAMS(DECL, args) CF_EXTRA_##policy);
#define CF_STUB(module, name, ret, sig, fn, policy, args)
#include "../../wasm/device_module/cf_imports.def"
#undef CF_STUB
#undef CF_ROW
#undef CF_EXTRA_XBM
#undef CF_EXTRA_SEQUENCE
#undef CF_EXTRA_STRING127
#undef CF_EXTRA_STRING95
#undef CF_EXTRA_PLAIN
}
#endif
