// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC
//
// The "cf" wasm import surface — every host call an app module can make.
// Device side: lib/WasmAppRuntime/WasmHostImports.cpp implements these and
// forwards to the real HAL. Guest side: the shims in
// wasm/device_module/shims/ forward the firmware HAL API onto these.
//
// Keep docs/SPIKE_WASM_IMPORT_SURFACE.md in sync when adding entries.

#ifndef CF_HAL_IMPORTS_H
#define CF_HAL_IMPORTS_H

#include "cf_hal_abi.h"

#include <stdint.h>

#define CF_IMPORT(NAME) \
    __attribute__((import_module("cf"), import_name(NAME)))

#ifdef __cplusplus
extern "C" {
#endif

// Declarations are expanded directly from the shared device interface table.
#include "cf_params.h"
#define CF_ROW(name, ret, sig, policy, args) CF_IMPORT(#name) ret cf_##name(CF_PARAMS(DECL, args));
#define CF_STUB(module, name, ret, sig, fn, policy, args)
#include "cf_imports.def"
#undef CF_STUB
#undef CF_ROW

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // CF_HAL_IMPORTS_H
