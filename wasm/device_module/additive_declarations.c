// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2026 Dismo Industries LLC
// Compatibility input for the existing additive guard's declaration parser.
#include "cf_params.h"
#define CF_ROW(name, ret, sig, policy, args) CF_IMPORT(#name) ret cf_##name(CF_PARAMS(DECL, args));
#define CF_STUB(module, name, ret, sig, fn, policy, args)
#include "cf_imports.def"
