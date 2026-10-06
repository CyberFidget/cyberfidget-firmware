// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2026 Dismo Industries LLC
// Compatibility input for the existing additive guard's declaration parser.
#include "cf_params.h"
#define CF_ROW(since, name, ret, sig, policy, args) CF_IMPORT("cf." #name, since, sig) ret cf_##name(CF_PARAMS(DECL, args));
#define CF_STUB(since, module, name, ret, sig, fn, policy, args) CF_IMPORT(module "." #name, since, sig) ret cf_stub_##name(CF_PARAMS(DECL, args));
#include "cf_imports.def"
