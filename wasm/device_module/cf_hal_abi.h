// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2026 Dismo Industries LLC

#ifndef CF_HAL_ABI_H
#define CF_HAL_ABI_H

// HAL ABI version the cf.* surface implements. ADD-ONLY within a major:
// adding a new cf.* import keeps this number; removing/renumbering an
// import is a MAJOR bump = an announced deprecation. The device runtime
// (WasmHostImports.cpp) provides exactly this ABI; a ferried blob's
// manifest `abi` field must be <= this or the device refuses it. See
// REQ-063 / N-067.
#define CF_HAL_ABI 1

#endif  // CF_HAL_ABI_H
