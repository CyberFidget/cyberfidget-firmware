// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2026 Dismo Industries LLC

#ifndef CF_HAL_ABI_H
#define CF_HAL_ABI_H

// Highest HAL level provided, equal to max(since) in cf_imports.def.
// The surface is add-only forever. New rows take since = the new CF_HAL_ABI.
// Breaking changes add a new import name; never change or remove an import.
// Each app declares only the highest level among its actual imports.
#define CF_HAL_ABI 2

#ifdef __cplusplus
namespace cf_hal {
constexpr int levels[] = {
#define CF_ROW(since, name, ret, sig, policy, args) since,
#define CF_STUB(since, module, name, ret, sig, fn, policy, args) since,
#include "cf_imports.def"
#undef CF_STUB
#undef CF_ROW
};
constexpr int maxLevel(int a, int b) { return a > b ? a : b; }
constexpr int maxSince(unsigned count) {
    return count ? maxLevel(levels[count - 1], maxSince(count - 1)) : 0;
}
static_assert(CF_HAL_ABI == maxSince(sizeof(levels) / sizeof(levels[0])),
              "CF_HAL_ABI must equal the highest import since level");
}
#endif

#endif  // CF_HAL_ABI_H
