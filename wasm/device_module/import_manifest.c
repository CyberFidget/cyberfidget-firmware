// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2026 Dismo Industries LLC
// Preprocess with any C preprocessor (-E -P -x c); stdout is a JSON array.
#include "cf_params.h"
#define CF_META_A(type, name, bytes) {"type": #type, "name": #name, "pointer": false}
#define CF_META_B(type, name, bytes) , CF_META_A(type, name, bytes)
#define CF_META_PA(type, name, bytes) {"type": #type, "name": #name, "pointer": true, "bytes": #bytes}
#define CF_META_PB(type, name, bytes) , CF_META_PA(type, name, bytes)
[
null
#define CF_ROW(name, ret, sig, policy, args) , {"module": "cf", "name": #name, "signature": sig, "returnType": #ret, "policy": #policy, "parameters": [CF_PARAMS(META, args)]}
#define CF_STUB(module, name, ret, sig, fn, policy, args) , {"module": module, "name": #name, "signature": sig, "returnType": #ret, "policy": #policy, "parameters": [CF_PARAMS(META, args)]}
#include "cf_imports.def"
]
