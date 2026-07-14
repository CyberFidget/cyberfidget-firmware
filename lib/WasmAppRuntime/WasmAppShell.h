// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#ifndef WASM_APP_SHELL_H
#define WASM_APP_SHELL_H

#include <stddef.h>
#include <stdint.h>

#include "ButtonManager.h"
#include "wasm3.h"

// Runs one embedded .wasm app module through the standard begin/update/end
// app lifecycle. The module must export app_begin/app_update/app_end/
// app_handle_button (see wasm/device_module/shims/cf_app_glue.cpp).
//
// Buttons: the shell registers for all six hardware buttons on the real
// ButtonManager and forwards every event into the module's exported
// app_handle_button; callback registration happens guest-side.
//
// Events are NOT forwarded from the ButtonManager callback itself: they are
// queued and drained at the top of update(), so a guest call can never start
// while another guest call is in flight (nested m3_Call on one runtime
// corrupts interpreter state and stacks native frames). Today the AppManager
// loop already serializes callbacks and app updates, but the queue makes the
// no-nesting invariant structural rather than incidental.
//
// Frame stats (per app_update wall time) are kept for the `wasmstat` CLI
// command and reset on every begin().
class WasmAppShell {
public:
    // ~50 Hz app loop -> 20 ms budget per app_update call.
    static constexpr int64_t kFrameBudgetUs = 20000;

    struct FrameStats {
        uint32_t frames     = 0;
        uint32_t overBudget = 0;  // frames where app_update ran > kFrameBudgetUs
        int64_t  minUs      = 0;
        int64_t  maxUs      = 0;
        int64_t  sumUs      = 0;
    };

    WasmAppShell(const char* name, const uint8_t* wasmBytes, size_t wasmLen);

    void begin();
    void update();
    void end();

    static WasmAppShell* running();      // shell currently active, or nullptr
    static WasmAppShell* statsSource();  // last shell that ran (survives end())

    const char* name() const { return appName; }
    const FrameStats& stats() const { return frameStats; }
    bool hasError() const { return errored; }
    const char* errorText() const { return errBuf; }

private:
    struct PendingButtonEvent {
        int8_t buttonIndex;
        int8_t eventType;
    };
    // Ring holds worst-case bursts between 20 ms frames (6 buttons x
    // press+release with slack). Oldest events drop first on overflow.
    static constexpr int kMaxPendingButtonEvents = 16;

    static void buttonTrampoline(const ButtonEvent& ev);
    void enqueueButton(int buttonIndex, int eventType);
    void drainButtonQueue();
    void callGuestButton(int buttonIndex, int eventType);
    void fail(const char* what);
    void drawErrorScreen();

    const char*    appName;
    const uint8_t* bytes;
    size_t         len;

    PendingButtonEvent pendingButtons[kMaxPendingButtonEvents];
    uint8_t            pendingHead = 0;  // read index
    uint8_t            pendingCount = 0;
    bool               inGuestCall = false;

    IM3Function fnBegin  = nullptr;
    IM3Function fnUpdate = nullptr;
    IM3Function fnEnd    = nullptr;
    IM3Function fnButton = nullptr;

    FrameStats frameStats;
    bool       errored = false;
    char       errBuf[96] = {0};

    static WasmAppShell* s_running;
    static WasmAppShell* s_statsSource;
};

#endif  // WASM_APP_SHELL_H
