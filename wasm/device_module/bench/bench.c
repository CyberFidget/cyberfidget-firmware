// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC
//
// wasm3 spike benchmark module (runs ON DEVICE inside the wasm3 interpreter).
//
// Freestanding wasm32 — no stdlib, no Emscripten runtime. Build with the
// emsdk-bundled clang (see wasm/device_module/build_device_modules.bat):
//
//   clang --target=wasm32 -O3 -nostdlib -Wl,--no-entry -Wl,--strip-all \
//         -o bench.wasm bench.c
//
// Exports:  fib(n)      recursive call benchmark
//           mix(n)      integer ALU loop benchmark
//           hostcall(n) import-call-overhead benchmark (n calls to cf.nop)
// Imports:  cf.nop(i32)->i32  provided by lib/WasmAppRuntime on device.

__attribute__((import_module("cf"), import_name("nop")))
extern int cf_nop(int x);

// noinline: keep this an honest function-call benchmark. Without it clang -O3
// self-inlines several recursion levels, which would make the wasm/native
// comparison measure different instruction streams.
__attribute__((export_name("fib"), noinline))
int fib(int n) {
    if (n < 2) return n;
    return fib(n - 1) + fib(n - 2);
}

// Integer math loop: LCG + xorshift-style mixing, no memory traffic.
// Must match nativeMix() in lib/WasmBenchApp/WasmBenchApp.cpp exactly.
__attribute__((export_name("mix"), noinline))
int mix(int n) {
    unsigned int a = 0x12345678u;
    unsigned int b = 0x9e3779b9u;
    for (int i = 0; i < n; i++) {
        a = a * 1664525u + 1013904223u;
        b ^= a;
        b = (b << 13) | (b >> 19);
        a += b ^ (a >> 7);
    }
    return (int)(a ^ b);
}

// Import-call overhead: cf.nop returns x+1 so the loop carries a data
// dependency and cannot be elided.
__attribute__((export_name("hostcall"), noinline))
int hostcall(int n) {
    int acc = 0;
    for (int i = 0; i < n; i++) {
        acc = cf_nop(acc);
    }
    return acc;
}
