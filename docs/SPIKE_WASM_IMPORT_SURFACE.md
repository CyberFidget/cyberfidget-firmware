# Spike: wasm3 apps on device (`spike/wasm3-apps`)

Status: spike / not for merge as-is. Measurements collected on hardware go at
the bottom (Results section left as TODO for the flash-and-measure pass).

Goal: can user apps ship as WebAssembly modules interpreted by
[wasm3](https://github.com/wasm3/wasm3) on the ESP32, reusing the same app
source that the browser emulator builds today?

## What was built

### Stage 1 — runtime + benchmark

- **`lib/wasm3/`** — vendored wasm3 interpreter, upstream commit
  `d77cd814aa0bc68cb1df917580a6304d34cfb30b` (2026-06-26, `source/` dir only,
  MIT — LICENSE copied alongside).
  - **Local patch** (the only source modification): `m3_core.c` gained a
    `d_m3CustomAllocator` guard so the host can supply
    `m3_Malloc_Impl`/`m3_Realloc_Impl`/`m3_Free_Impl`. Upstream has no
    allocator hook (default is plain `calloc`/`realloc`).
  - `library.json` compiles with `-O2` (overrides project `-Os` for
    interpreter speed), `-Dd_m3CustomAllocator`, and **`-DM3_IN_IRAM`**.
    The last one is critical and confusingly named: *without* it wasm3 tags
    every interpreter opcode handler `IRAM_ATTR` on ESP32 (~500 functions —
    would instantly blow the IRAM budget). Defining it makes `vectorcall`
    empty, so the whole interpreter lives in flash. Verified via `nm`: zero
    wasm3 symbols in `0x4008xxxx/0x4009xxxx`, all in flash-mapped IROM.
  - WASI/tracer/libc API files are excluded via `srcFilter` (dead weight).
- **`lib/WasmAppRuntime/`** — host wrapper:
  - `WasmAppRuntime` — env/runtime/module lifecycle, 48 KB interpreter stack
    default, links the host import table, `callI_I`/`callV_V` helpers.
  - **PSRAM-first allocator** (`WasmAppRuntime.cpp`): every wasm3 allocation
    (interpreter stack, compiled-code pages, linear memory) tries
    `heap_caps_calloc(MALLOC_CAP_SPIRAM)` first, falls back to internal heap.
    `-DCF_WASM3_PREFER_PSRAM=0` flips to internal RAM for perf comparison.
    Realloc replicates upstream's zero-fill of grown regions (linear-memory
    growth relies on fresh pages reading as zero).
  - Module byte arrays are **flash-resident** (`const` → rodata, memory-mapped
    on ESP32); wasm3 parses/executes them in place, no RAM copy of the image.
  - `WasmHostImports.cpp` — the "cf" import table (below) + WASI/env stubs.
  - `WasmAppShell` — runs a module through begin/update/end, forwards all six
    hardware buttons into the module's exported `app_handle_button`, tracks
    per-`app_update` frame stats for `wasmstat`, and handles the deferred
    exit-to-menu request. If a module fails to load, any button release
    escapes to the menu (the error text shows on the OLED).
- **`lib/WasmBenchApp/`** — "WasmBench" app (Tools menu) + `wasmbench` CLI.
  Embedded 412-byte `bench.wasm` (freestanding clang, no libc) exporting
  `fib`/`mix`/`hostcall`; identical native C++ implementations timed
  alongside. `bench.wasm` validated in node (fib(30)=832040 correct).
- **`platformio.ini`** — new `[env:spike_test]` = `local` +
  `-DCF_TEST_CLI=1`. No `local_test` env existed on main when this branched;
  test-only CLI verbs are gated on `CF_TEST_CLI`.

### Stage 2 — real apps as .wasm

- **`wasm/device_module/`** — guest-side toolchain:
  - `cf_hal_imports.h` — every host call an app module can make, declared
    `extern "C"` with `import_module("cf")` attributes.
  - `shims/` — guest versions of `Arduino.h`, `DisplayProxy.h`,
    `ButtonManager.h`, `AudioManager.h`, `MenuManager.h`, `RGBController.h`,
    `HAL.h`, `globals.h`, `esp_log.h` that shadow the firmware headers via
    include order and forward onto the `cf` imports. Modeled on `wasm/shims/`
    (browser emulator) but imports instead of in-module emulation.
    `Arduino.h` provides a fixed-buffer (64 B) `String` so modules carry no
    malloc-based string machinery; `esp_log.h` compiles logging out.
  - `shims/cf_app_glue.cpp` — defines the shimmed globals/singletons and
    exports `app_begin` / `app_update` / `app_end` / `app_handle_button`.
    Sensor globals (`millis_NOW`, `accelX/Y/Z`, slider) are refreshed from
    imports at the top of every exported entry. App selected at compile time
    (`-DCF_WASM_APP_REACTION` / `-DCF_WASM_APP_BREAKOUT`).
  - `build_device_modules.bat` — builds all three modules and regenerates the
    embedded headers. Headers are committed, so **firmware builds never need
    emsdk**.
- **`lib/WasmApps/`** — embedded `reaction.wasm` (2,473 B) and
  `breakout.wasm` (17,269 B) + `WasmAppShell` instances, registered in the
  manifest as **"WasmReaction"** and **"WasmBreakout"** under **Tools**
  (kept beside WasmBench so all spike artifacts group together).
  **The app sources (`lib/ReactionTimeGame/ReactionTimeGame.cpp`,
  `lib/BreakoutGame/BreakoutGame.cpp`) are compiled into the modules
  completely unmodified.**
- Both modules smoke-tested in node with mocked `cf` imports before
  embedding: ReactionTime full game loop (start → GO! → "Time: 234 ms" →
  exit) and Breakout menu + 250 simulated frames of gameplay (~30 host
  calls/frame) run clean.
- **Wave 2** added Starburst, Spaceship, SPHFluid, Splooty the same way
  (`wasm/device_module/smoke_test.js` scripts all six). 2026-07-03: the
  Spaceship module was rebuilt from the **T-114 asteroid-shooter** source
  (copied verbatim from the main repo working tree, replacing the old
  screensaver-only version); it needed one new import,
  `display_draw_triangle`. The Breakout smoke script now regression-tests
  the level-skip → victory path from the crash investigation below.

## The "cf" import surface

Import module `"cf"`. This is the complete list of host calls the two ported
apps needed — i.e. the measured minimum HAL surface for real apps.

| Import | Sig | Used by | Host forwarding |
|---|---|---|---|
| `nop(i32)->i32` | `i(i)` | bench | returns x+1 (call-overhead probe) |
| `display_clear()` | `v()` | both | `DisplayProxy::clear` |
| `display_show()` | `v()` | both | `DisplayProxy::display` |
| `display_set_color(i32)` | `v(i)` | breakout | `setColor` (0/1/2 = BLACK/WHITE/INVERSE) |
| `display_set_pixel(x,y)` | `v(ii)` | breakout | `setPixel` |
| `display_draw_line(x0,y0,x1,y1)` | `v(iiii)` | (offered) | `drawLine` |
| `display_draw_rect(x,y,w,h)` | `v(iiii)` | breakout | `drawRect` |
| `display_fill_rect(x,y,w,h)` | `v(iiii)` | breakout | `fillRect` |
| `display_draw_circle(x,y,r)` | `v(iii)` | (offered) | `drawCircle` |
| `display_fill_circle(x,y,r)` | `v(iii)` | (offered) | `fillCircle` |
| `display_draw_hline(x,y,len)` | `v(iii)` | breakout | `drawHorizontalLine` |
| `display_draw_vline(x,y,len)` | `v(iii)` | (offered) | `drawVerticalLine` |
| `display_draw_triangle(x0,y0,x1,y1,x2,y2)` | `v(iiiiii)` | spaceship (T-114) | `drawTriangle` (warning placard) |
| `display_set_align(i32)` | `v(i)` | both | `setTextAlignment` (0/1/2/3) |
| `display_set_font(i32)` | `v(i)` | both | 10/16/24 → `ArialMT_Plain_*` (fonts stay host-side; guests hold 1-byte ID handles) |
| `display_draw_string(x,y,ptr,len)` | `v(iiii)` | both | bounded copy out of guest memory → `drawString` |
| `display_string_width(ptr,len)->i32` | `i(ii)` | (offered) | `getStringWidth` |
| `display_draw_xbm(x,y,w,h,ptr)` | `v(iiiii)` | splooty | `drawXbm` — bits read in place from guest memory (host blits synchronously, keeps no pointer); length `((w+7)/8)*h`, w/h clamped to 128/64 |
| `led_set(idx,r,g,b,w)` | `v(iiiii)` | (offered) | `HAL::setRgbLed` (idx 0..3 per pixel map) |
| `led_all_off()` | `v()` | breakout | `HAL::setRgbLedsOff` |
| `tone_play(f32,ms)` | `v(fi)` | breakout | `AudioManager::playTone` |
| `tone_stop()` | `v()` | breakout | `stopTone` |
| `seq_play(ptr,count)` | `v(ii)` | breakout | steps copied to a host buffer (max 64) — guest memory can move on `memory.grow`, and `playSequence` keeps the pointer |
| `seq_stop()` | `v()` | breakout | `stopSequence` |
| `millis()->u32` | `i()` | both | `millis()` |
| `random(min,max)->i32` | `i(ii)` | both | `random(min,max)` |
| `slider_pct()->f32` | `f()` | breakout | `sliderPosition_Percentage_Filtered` |
| `accel_x/y/z()->f32` | `f()` | breakout | `accelX/Y/Z` |
| `exit_to_menu()` | `v()` | both | sets a **deferred** flag; shell calls `returnToMenu()` after the guest call returns (unloading mid-call would free the interpreter under our feet) |
| `keepalive()` | `v()` | breakout | `millis_APP_LASTINTERACTION = millis_NOW` (deep-sleep inhibit; glue forwards guest bumps of that global) |
| `log(ptr,len)` | `v(ii)` | (debug) | `[evt] wasm.log=...` on Serial |

Not needed by these two apps (candidates for later): battery state, display
XBM/image blit, progress bar, contrast/brightness, accelerometer temp, RTC,
persistent storage, WiFi/BLE anything.

Also linked (insurance for Emscripten-standalone output; the current modules
import none of them): `wasi_snapshot_preview1.{proc_exit, fd_write, fd_close,
fd_seek, environ_sizes_get, environ_get, clock_time_get, random_get}` and
`env.emscripten_notify_memory_growth`. Filesystem stubs fail with EBADF.

## Crash investigation — WasmBreakout level-skip reboot (2026-07-03)

**Symptom (hardware playtest):** WasmBreakout plays fine; pressing BottomRight
during play (playtest level-skip: `onBottomRight` → `test_winLevel()`, next
tick `checkVictory()` advances) hard-crashes/reboots the device. Attract mode
never hit this path.

### Hypothesis audit

**(a) Stale linear-memory base after `memory.grow` — EXCLUDED.**
- No host import or shell code caches a linear-memory pointer. Every import is
  an `m3ApiRawFunction` resolving guest pointers through the per-call `_mem`
  (`m3ApiGetArgMem` + `m3ApiCheckMem`); `seq_play` copies steps to a host
  buffer; `draw_xbm`/`draw_string` read synchronously and keep nothing.
- The vendored wasm3 (2026-06 upstream) refreshes `_mem` after every path that
  can move memory: `op_Call`/`op_CallIndirect` (after `Call` returns),
  `op_MemGrow` (after `ResizeMemory`), `op_CallRawFunction` (on trap). The
  custom-allocator patch zero-fills grown regions like upstream.
- Can the guest even grow? Link flags: `INITIAL_MEMORY=262144`,
  `ALLOW_MEMORY_GROWTH=1`, `MAXIMUM_MEMORY=1048576` — growth is *possible*,
  but Breakout allocates nothing dynamically (`loadLevel` only rewrites the
  static `cells[][]`). Instrumented V8 run (below): **zero growth events**
  across the full skip path. If malloc ever did fail, C++ `new` aborts →
  `unreachable` → wasm3 trap → `fail()` → error screen (verified path, no
  reboot).

**(b) Nested button-callback dispatch — NOT PRESENT, NOW STRUCTURALLY
IMPOSSIBLE.** Physical buttons are queued by `ButtonManager` (ring buffer)
and dispatched by `AppManager::loop()` → `processButtonEvents()` strictly
*between* `runActiveApp()` ticks, on the same task — so `m3_CallV(fnButton)`
never ran inside `app_update` today; the crash is not nested dispatch.
But the invariant was incidental (any future host import that pumps events
would break it), so the shell now queues trampoline events in its own ring
(`WasmAppShell::enqueueButton`) and drains them at the top of `update()`,
with an `inGuestCall` flag making a nested `m3_Call` unreachable by
construction.

**(c) Native stack depth — SUSPECTED ROOT CAUSE (uncontained crash class,
now guarded).** wasm3 recurses on the *native* C stack for every wasm→wasm
call (`op_Call` → nested `RunCode`), and upstream has **no native-stack
guard** — `m3StackCheck` is a logging facility (`d_m3LogNativeStack`), not
protection; the `op_Entry` check only covers the m3 *slot* stack. History on
this branch: the stock 8 KB loopTask stack already blew on fib(30) and
Breakout's `begin` (commit 86420b8, bumped to 32 KB — treating the symptom).
The skip path executes the deepest never-before-run call chains in the app
(level-advance, then victory screen: `String(float,1)` → musl
`vsnprintf`/`fmt_fp`) *and* first-call lazy compilation interleaved at depth
(`CompileFunction` recurses per block-nesting level; musl's printf core is
deeply nested). A native stack overrun = stack-canary panic = reboot — which
is exactly the reported failure, and the only hypothesis the static audit
could not exclude.
- **Fix (containment):** second vendored patch (`d_m3NativeStackGuard`, grep
  "CYBERFIDGET SPIKE PATCH"): `op_Entry` now also asks the host
  (`m3_NativeStackExhausted()`) whether native headroom is exhausted and
  traps `m3Err_trapStackOverflow` if so. The host arms a floor
  (task stack base + `CF_WASM_NATIVE_STACK_RESERVE`, default 10 KB) in
  `WasmAppRuntime::load()`, disarms on `unload()`. Result: runaway/deep guest
  recursion → OLED error screen + exit to menu, **not** a device reset.
  This closes the containment gap for *all* future app bugs of this class
  (e.g. unbounded recursion in a user app).
- **Diagnostic for the hardware pass:** `wasmstat` now prints
  `loop_stack_min_free` (loopTask stack high-water mark). Play Breakout,
  skip levels via `btn 5 tap`, read `wasmstat`: a small number (<~2 KB
  pre-guard) confirms the diagnosis; the guard trap firing shows up as
  `wasmstat.error=[trap] stack overflow`.

**(d) Host audio imports on the victory path — EXCLUDED.** `seq_play` bounds
`count` to 64 steps, `m3ApiCheckMem`s the guest range, memcpys into a static
host buffer (`s_seqBuf`) before `playSequence` (host keeps only the static
pointer); `tone_play/stop`, `seq_stop` are stateless pass-throughs;
`AudioManager::loop()` indexes `currentSequence[idx]` only within
`currentSequenceLen`. Guest/host `ToneStep` layouts static-asserted equal.

### Guest-side repro (V8, exonerates the module)

`breakout.wasm` run in node with mocked `cf` imports through the exact
hardware scenario — menu → input select → 50 frames → BottomRight
press/release skip × 9 (25 frames between) → 100 victory-screen frames
(String float formatting every frame) → restart → skip again → back-exit:
**no trap, no memory growth (262,144 B constant), WIN_JINGLE seq_play fires
once, all strings well-formed.** The game logic and shims are clean; the
failure is host/interpreter-side, consistent with (c).

### Trap containment status (all paths audited)

- Trap in `app_update` → `fail()` → error screen; buttons drain to the
  errored branch; any Release returns to menu. No reboot.
- Trap in `app_handle_button` (mid-callback) → same `fail()` path. The shell
  never unloads the runtime while a guest call is in flight (`exit_to_menu`
  is deferred; `end()` additionally refuses `fnEnd` if `inGuestCall`).
- `proc_exit` → `m3Err_trapExit` → contained. Unlinked imports trap on first
  call (`function lookup failed`) → contained. malloc-fail/abort →
  `unreachable` trap → contained.
- Native stack exhaustion → **was a reboot, now** `m3Err_trapStackOverflow`
  → contained (the fix above).
- Still *not* containable at this layer: a host import itself crashing (all
  imports are bounds-checked precisely for that) and hardware watchdog/
  brownout.

Crash-fix cost: +516 B flash, +200 B static RAM (guard + queue + wasmstat
line).

## Build instructions

Firmware (no emsdk needed — modules are committed as C arrays):

```
pio run -e spike_test            # test build with CF_TEST_CLI verbs
pio run -e spike_test -t upload  # flash
pio run -e local                 # production env still builds (verified)
```

Rebuild the wasm modules after touching bench.c, shims, or the wrapped apps
(needs the workspace emsdk; Emscripten 3.1.51):

```
cd wasm\device_module
build_device_modules.bat
```

Module compile recipe (what the script does):

- bench: `clang --target=wasm32 -O3 -nostdlib -Wl,--no-entry -Wl,--strip-all`
- apps: `em++ -O2 -fno-exceptions -fno-rtti --no-entry -sSTANDALONE_WASM=1
  -sERROR_ON_UNDEFINED_SYMBOLS=0 -sINITIAL_MEMORY=262144
  -sMAXIMUM_MEMORY=1048576 -sALLOW_MEMORY_GROWTH=1 -sSTACK_SIZE=32768
  -mnontrapping-fptoint -mbulk-memory -msign-ext -I shims -I .
  -I ../../lib/<App> -DCF_WASM_APP_<APP> shims/cf_app_glue.cpp
  ../../lib/<App>/<App>.cpp -o <app>.wasm`
- `-sERROR_ON_UNDEFINED_SYMBOLS=0` because the `cf.*` imports resolve
  on-device, not at link time (they carry explicit `import_module`
  attributes, so wasm-ld emits proper imports).
- The feature flags (`nontrapping-fptoint`, `bulk-memory`, `sign-ext`) were
  checked against the vendored wasm3's opcode tables — all supported.
  Nontrapping float→int matters: Breakout does float→int casts every frame
  and the trapping variant would kill the app on a NaN.

## Size / memory measurements (build-time)

`pio run -e spike_test`, all vs the same env built from the branch point
(fb8e2fd) with only the env stanza added:

| Build | Flash | RAM (static) |
|---|---|---|
| Baseline | 2,798,501 B (83.7%) | 93,484 B (28.5%) |
| + Stage 1 (wasm3 + runtime + bench) | 2,878,869 B (86.1%) | 93,644 B |
| + Stage 2 (host imports + shell + 2 app modules) | 2,925,877 B (87.5%) | 94,548 B |

- **Stage 1 cost: +80,368 B flash, +160 B static RAM.** (wasm3 at `-O2` +
  runtime + embedded bench; the oft-quoted ~64 KB is `-Os` without cascaded
  opcodes.)
- **Stage 2 adds: +47,008 B flash, +1,064 B static RAM** (host import table,
  shell, 2.4 KB + 16.9 KB embedded modules, WASI stubs).
- **Total spike: +127,376 B flash. Remaining app-partition headroom: ~407 KB.**
- 2026-07-03 update (wave-2 apps + crash containment + `btn` CLI + T-114
  Spaceship module): **3,003,613 B flash (89.9%), 96,116 B static RAM** —
  ~338 KB under the 3,342,336 B ceiling, so the embedded bench module stays.
  The T-114 `spaceship.wasm` is 20,532 B embedded (old screensaver-only
  build was 5,967 B).
- IRAM: **zero added** (verified with `nm`; `M3_IN_IRAM` keeps the
  interpreter in flash).
- **wasm3 `-O2` vs `-Os` (2026-07-03 A/B, library-scoped flag in
  `lib/wasm3/library.json` — the rest of the firmware stays `-Os`):**
  `-O2` = 3,003,613 B flash, `-Os` = 2,995,377 B → **`-O2` costs +8,236 B
  flash, identical static RAM**. Verified on the verbose compile line that
  the library flags land *after* the project `-Os` (last `-O` wins) and
  `-DM3_IN_IRAM`/`-Dd_m3CustomAllocator`/`-Dd_m3NativeStackGuard` stay
  intact. `-O2` is the committed state; flip the one flag in
  `lib/wasm3/library.json` to A/B interpreter frame times on hardware.
- Runtime heap cost is reported by the device itself (`wasmbench` prints
  `load_heap_cost`/`load_psram_cost`; `wasmstat` prints live free heap/PSRAM).
  Expect roughly: 48 KB interpreter stack + 4 pages (256 KB) initial linear
  memory for app modules + compiled-code pages — nearly all of it in PSRAM
  thanks to the custom allocator. bench.wasm declares 1 page (64 KB).

## Design notes / caveats (honest list)

- **wasm3 allocator patch**: upstream has no hook; the `#if
  defined(d_m3CustomAllocator)` guard in `m3_core.c` is the one vendored-file
  edit. If wasm3 is ever updated, re-apply (grep for "CYBERFIDGET SPIKE
  PATCH").
- **PSRAM interpreter state is a perf trade**: stack + compiled code +
  linear memory in PSRAM protects the ~200 KB internal heap but every
  interpreter memory access rides the PSRAM cache. `wasmbench` with
  `-DCF_WASM3_PREFER_PSRAM=0` quantifies the gap if needed.
- **Button callbacks**: real callback *registration* never crosses the wasm
  boundary. The shell subscribes to all six buttons and forwards
  `(index, eventType)` into the module; the guest-side ButtonManager shim
  dispatches to the app's registered handlers. Semantics preserved, host
  stays dumb.
- **exit_to_menu is deferred** to after the in-flight guest call returns —
  `returnToMenu()` ends the app, which unloads the runtime; doing that from
  inside a host function called *by* the interpreter would be use-after-free.
- **`wasmbench` blocks the main loop** for the duration (fib(30) under
  interpretation is seconds). Buttons/display freeze during a run — expected,
  test-CLI-only. The loopTask WDT doesn't watch core 1's idle task in this
  config, so no watchdog trips.
- **First-frame cost**: wasm3 compiles functions lazily on first call, so the
  first `app_update` includes compile time; `wasmstat` max reflects that.
  Consider `m3_CompileModule` (eager) if it matters.
- **ESP_LOG in guests compiles out** (printf would drag ~10 KB of libc into
  each module and its output path is a WASI stub anyway). `cf.log` exists for
  targeted guest debugging.
- **Fixed-buffer String (64 B)** truncates silently; fine for display lines.
  Apps that build longer strings would need the cap raised or real
  allocation.
- **`millis_APP_LASTINTERACTION`** is guest-local; the glue forwards bumps to
  the host via `cf.keepalive` so accel/slider-only play still inhibits deep
  sleep (Breakout relies on this).
- **Not forwarded (silently no-op in guests)**: `AudioManager::setVolume`,
  `isSequencePlaying` (returns false), display overlay mode. None of the two
  apps care; a real port should decide per-API.
- **Emulator/docs impact**: none yet — nothing in `wasm/` (browser build) or
  public HAL headers changed. If this graduates beyond a spike, the "cf"
  import surface becomes a shared hardware contract and needs the full
  cross-repo treatment (emulator, docs, App Builder).

## CLI quick reference (spike_test build, 921600 baud)

```
help                      → [cmd] help=version,info,help,wasmbench,wasmstat,wasmapp,btn
wasmbench                 → runs the full suite (blocks ~10-30 s), prints
                            [cmd] wasmbench.load_us / .fib30_us / .fib30_native_us
                            / .mix_us / .mix_native_us / .hostcall_us
                            / .hostcall_ns_per / .fib30_ok / .mix_ok
                            / .heap_free / .psram_free / .load_heap_cost
                            / .load_psram_cost
wasmapp reaction          → launch WasmReaction (also: breakout | starburst |
                            spaceship | sphfluid | splooty | bench | menu)
wasmstat                  → [cmd] wasmstat.app/.running/.frames/.frame_us_min
                            /.frame_us_avg/.frame_us_max/.frames_over_budget
                            /.heap_free/.psram_free/.loop_stack_min_free/.error
btn <index> <press|release|tap>
                          → inject a button event at ButtonManager level (works
                            for native and wasm apps). tap = press + release
                            ~50 ms later, scheduled without blocking. Indices:
                            0=TopLeft(Up) 1=TopRight(Down) 2=MiddleLeft(Left)
                            3=MiddleRight(Right) 4=BottomLeft(Back)
                            5=BottomRight(Enter). Held is not injectable (fires
                            naturally if a `press` is left held >1 s).
```

`wasmstat` stats reset each time a wasm app begins; they survive exit so you
can `wasmapp menu` then read the final numbers.

Headless crash repro for the Breakout skip bug:
`wasmapp breakout` → `btn 5 tap` (confirm input mode) → wait → `btn 5 tap`
per level skip (x9 to reach the victory screen) → `wasmstat`.

## Results (hardware) — 2026-07-11 measurement pass

Bench: HIL-B (T-010 power-mod unit, COM36), PPK2 sourcing 3.7 V at the
battery port, `spike_test` @ 1.2.2+19ae409 (version ritual verified). Runs
were serial-driven (`btn` injection; the menu was navigated blind before
rediscovering `wasmapp` further up this file - both work).

### wasmbench (reproduced across two runs, identical numbers)

| Metric | Value |
|---|---|
| module load | 12,346 us |
| fib(30) wasm / native | 5,991,252 us / 138,005 us = **43.4x** |
| mix wasm / native | 3,087,558 us / 71,148 us = **43.4x** |
| host call round-trip | **1,088 ns** |
| load heap cost / PSRAM cost | **0 B** / 185,012 B (PSRAM-first allocator doing its job) |
| heap_free / psram_free after | 70,156 / 1,908,368 |

### WasmBreakout `wasmstat` (the number that decides the approach)

~800 frames of play including 3 serial level-skips:

| Metric | Early play | After skips |
|---|---|---|
| frame_us avg | 2,689-2,895 | 3,266 |
| frame_us max | ~18,000 | 18,105 |
| frames_over_budget | **0** | **0** |
| heap_free | 68,700 (stable) | 68,700 |
| loop_stack_min_free | 53,724 -> 34,668 | **31,356** |

**Verdict: interpreted wasm is comfortably real-time for this app class** -
a full Breakout frame (brick field redraw + physics + ~30 host calls) costs
~2.7-3.3 ms of the 20 ms budget. The 43x raw-compute ratio doesn't matter
when frames are host-call- and budget-dominated.

### Level-skip crash: diagnosis CONFIRMED, guard holds

3 skips on hardware: no reboot, `wasmstat.error=none`, CLI alive after.
`loop_stack_min_free` fell 53.7 KB -> 31.4 KB through the skips - peak
native-stack use ~33 KB, which is exactly why the 32 KB loop stack rebooted
(commit 19ae409's subject) and 64 KB + the native-stack guard survives.
Hypothesis (c) is quantitatively confirmed.

### Power (PPK2 timeline, 3.7 V; segments cross-checked against the serial log)

| State | Current |
|---|---|
| WasmBreakout gameplay (steady, double-confirmed live) | **~61-62 mA** |
| native Breakout gameplay (steady segments) | ~63-65 mA |
| native Breakout audio bursts (tone peaks) | ~290 mA peaks |
| menu idle | ~63-65 mA |
| deep sleep (idle timeout) | **0.25 mA** |
| boot inrush | ~600 mA max |

**Interpretation adds no measurable steady-state power** - display/system
draw dominates and native vs wasm gameplay are within segment noise of each
other. (Caveat: native/menu segment attribution is inferred from the hold
timeline + choreography; the wasm figure was confirmed live during known
play. A clean 3-capture comparison is scripted and easy to redo.)

### Not yet run

- [ ] WasmReaction 30 s pass (expected easier than Breakout)
- [ ] The full x9-skip VICTORY-screen path (String float formatting depth);
      3 skips exercised the skip path itself
- [ ] `-DCF_WASM3_PREFER_PSRAM=0` perf comparison

### Bench ops learned (also in the cyberfidget-hil README)

- Hard-killing a `ppk on` hold mid-stream garbles the PPK2 protocol state
  (garbled calibration metadata on next open); recover with `cf-bench ppk
  off` twice. Let holds expire naturally.
- The unbuffered hold's ~1 Hz live-current lines double as a free
  per-second power timeline - run holds with PYTHONUNBUFFERED=1 and keep
  the log.
