<!-- SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception -->
<!-- Copyright (c) 2026 Dismo Industries LLC -->
# Device-module emulator core

With Emscripten active and `wasm/generated/version.h` available:

```sh
emcmake cmake -S wasm -B wasm/build/module-host -G Ninja \
  -DCF_WASM_MODULE_HOST=ON -DCMAKE_BUILD_TYPE=Release
cmake --build wasm/build/module-host
node wasm/module_host/smoke_core.mjs wasm/build/module-host/cyberfidget-core.js \
  path/to/app.device.wasm wasm/build/module-host/cf-imports.json
```

The target links no app. It produces `cyberfidget-core.{js,wasm}` and
`cf-imports.json`, preprocessed from `device_module/cf_imports.def`.
The normal Demo, per-app and Custom builds keep their existing behavior.

`attachGuest(core, bytes, manifest)` creates imports using that table. Install
frame/LED/serial callbacks on the core as usual, then call
`wasm_module_start()`, `wasm_module_frame()` at 50 Hz, and
`wasm_module_end()` when unloading. The frame entry polls hardware inputs,
forwards button events, services audio, calls the guest update, and flushes
its display. Exit requests run guest end after the current call returns and
notify `onAppExit`. Guest end is responsible for its usual LED/audio cleanup.

All `cf_*` C exports call the same `cf_host` behavior functions as the device.
Strings pass pointer + length; sequences pass pointer + clamped step count;
XBM exports take one additional byte length after the pointer. Guest XBM
signatures are unchanged. The adapter validates the complete guest range
before copying (including long strings), recreates views after memory growth,
and transfers through a 2048-byte aligned core buffer. Sequences then copy to
a separate persistent 64-step host buffer. No guest pointer is retained.

The smoke check runs 300 real-time frames, scripted inputs, pointer boundary
and memory-growth checks, persistent sequence-copy and deferred/idempotent exit checks. Node mocks the
Web Audio device; audible output still requires browser/manual testing.
The manifest includes the allowed WASI and env stubs. Contract verification
compares binary function types with each allowed (module, name, signature).
No fixed clock or prototype tracing is part of this target.

The additive ABI CI guard uses `device_module/check_interface_additive.py` to
preprocess declarations for its existing comparison logic; no parallel
handwritten import list is maintained.
