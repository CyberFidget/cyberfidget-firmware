// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#include "WasmAppShell.h"

#include <stdio.h>
#include <string.h>

#include <Arduino.h>
#include <esp_timer.h>

#include "HAL.h"
#include "MenuManager.h"
#include "WasmAppRuntime.h"
#include "WasmHostImports.h"

WasmAppShell* WasmAppShell::s_running     = nullptr;
WasmAppShell* WasmAppShell::s_statsSource = nullptr;

WasmAppShell::WasmAppShell(const char* name, const uint8_t* wasmBytes, size_t wasmLen)
    : appName(name), bytes(wasmBytes), len(wasmLen) {}

WasmAppShell* WasmAppShell::running() { return s_running; }
WasmAppShell* WasmAppShell::statsSource() { return s_statsSource; }

void WasmAppShell::fail(const char* what) {
    errored = true;
    snprintf(errBuf, sizeof(errBuf), "%s", what ? what : "unknown");
    Serial.printf("[err] wasmapp %s: %s\n", appName, errBuf);
}

void WasmAppShell::begin() {
    errored    = false;
    errBuf[0]  = '\0';
    frameStats = FrameStats();
    fnBegin = fnUpdate = fnEnd = fnButton = nullptr;
    pendingHead  = 0;
    pendingCount = 0;
    inGuestCall  = false;

    s_running     = this;
    s_statsSource = this;
    wasmHostClearExitRequest();

    // Register buttons before loading so a failed module still leaves the
    // user an exit path (any button release returns to menu when errored).
    for (int i = 0; i < 6; i++) {
        HAL::buttonManager().registerCallback(i, &WasmAppShell::buttonTrampoline);
    }

    WasmAppRuntime& rt = WasmAppRuntime::instance();
    if (!rt.load(bytes, len)) {
        fail(rt.lastError());
        return;
    }

    // Emscripten standalone (reactor) modules export _initialize to run
    // global ctors. Optional: bare clang modules may not have it.
    IM3Function fnInit = rt.findFunction("_initialize");
    if (fnInit && !rt.callV_V(fnInit)) {
        fail(rt.lastError());
        return;
    }

    fnBegin  = rt.findFunction("app_begin");
    fnUpdate = rt.findFunction("app_update");
    fnEnd    = rt.findFunction("app_end");
    fnButton = rt.findFunction("app_handle_button");
    if (!fnBegin || !fnUpdate || !fnEnd || !fnButton) {
        fail("module missing app_* exports");
        return;
    }

    inGuestCall = true;
    bool ok = rt.callV_V(fnBegin);
    inGuestCall = false;
    if (!ok) {
        fail(rt.lastError());
        return;
    }
}

void WasmAppShell::update() {
    // Deliver queued button events first so the guest sees button -> frame
    // ordering, one guest call at a time. Draining may end the app (exit
    // request or errored-state escape), in which case we're done.
    drainButtonQueue();
    if (s_running != this) return;

    if (errored) {
        drawErrorScreen();
        return;
    }

    WasmAppRuntime& rt = WasmAppRuntime::instance();
    int64_t t0 = esp_timer_get_time();
    inGuestCall = true;
    bool ok = rt.callV_V(fnUpdate);
    inGuestCall = false;
    int64_t dt = esp_timer_get_time() - t0;

    if (!ok) {
        fail(rt.lastError());
        return;
    }

    if (frameStats.frames == 0 || dt < frameStats.minUs) frameStats.minUs = dt;
    if (dt > frameStats.maxUs) frameStats.maxUs = dt;
    if (dt > kFrameBudgetUs) frameStats.overBudget++;
    frameStats.sumUs += dt;
    frameStats.frames++;

    if (wasmHostConsumeExitRequest()) {
        MenuManager::instance().returnToMenu();  // triggers our end() via AppManager
    }
}

void WasmAppShell::end() {
    for (int i = 0; i < 6; i++) {
        HAL::buttonManager().unregisterCallback(i);
    }

    WasmAppRuntime& rt = WasmAppRuntime::instance();
    if (!errored && fnEnd && rt.isLoaded() && !inGuestCall) {
        inGuestCall = true;
        rt.callV_V(fnEnd);  // best effort; module is going away regardless
        inGuestCall = false;
    }
    rt.unload();
    fnBegin = fnUpdate = fnEnd = fnButton = nullptr;
    pendingHead  = 0;
    pendingCount = 0;
    HAL::setRgbLedsOff();  // app-lifecycle contract: LEDs off on exit
    if (s_running == this) s_running = nullptr;
}

void WasmAppShell::buttonTrampoline(const ButtonEvent& ev) {
    // Queue only — never call into the guest from here. The queue is drained
    // by update(), which guarantees exactly one guest call is in flight at a
    // time (nested m3_Call on one runtime = interpreter-state corruption).
    if (s_running) s_running->enqueueButton(ev.buttonIndex, (int)ev.eventType);
}

void WasmAppShell::enqueueButton(int buttonIndex, int eventType) {
    if (pendingCount >= kMaxPendingButtonEvents) {
        // Drop the oldest so the guest still sees the most recent state.
        pendingHead = (uint8_t)((pendingHead + 1) % kMaxPendingButtonEvents);
        pendingCount--;
    }
    uint8_t w = (uint8_t)((pendingHead + pendingCount) % kMaxPendingButtonEvents);
    pendingButtons[w].buttonIndex = (int8_t)buttonIndex;
    pendingButtons[w].eventType   = (int8_t)eventType;
    pendingCount++;
}

void WasmAppShell::drainButtonQueue() {
    while (pendingCount > 0) {
        PendingButtonEvent ev = pendingButtons[pendingHead];
        pendingHead = (uint8_t)((pendingHead + 1) % kMaxPendingButtonEvents);
        pendingCount--;
        callGuestButton(ev.buttonIndex, ev.eventType);
        if (s_running != this) return;  // app ended while dispatching
    }
}

void WasmAppShell::callGuestButton(int buttonIndex, int eventType) {
    if (errored || !fnButton) {
        // Errored state: any button release exits back to the menu.
        if (errored && eventType == (int)ButtonEvent_Released) {
            MenuManager::instance().returnToMenu();
        }
        return;
    }
    if (inGuestCall) return;  // structural no-nesting guarantee

    inGuestCall = true;
    M3Result res = m3_CallV(fnButton, (int32_t)buttonIndex, (int32_t)eventType);
    inGuestCall = false;
    if (res) {
        fail(res);
        return;
    }
    if (wasmHostConsumeExitRequest()) {
        MenuManager::instance().returnToMenu();
    }
}

void WasmAppShell::drawErrorScreen() {
    DisplayProxy& d = HAL::displayProxy();
    d.clear();
    d.setTextAlignment(TEXT_ALIGN_CENTER);
    d.setFont(ArialMT_Plain_10);
    d.drawString(64, 10, appName);
    d.drawString(64, 26, "wasm error:");
    d.drawString(64, 40, errBuf);
    d.display();
}
