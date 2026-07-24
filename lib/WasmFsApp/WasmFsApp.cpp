// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2026 Dismo Industries LLC

#include "WasmFsApp.h"

#include <Arduino.h>
#include <LittleFS.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

#include <string>
#include <string.h>

#include "ButtonManager.h"
#include "DisplayProxy.h"
#include "HAL.h"
#include "MenuManager.h"
#include "WasmAppShell.h"
#include "WasmHostImports.h"

namespace WasmFsApp {
namespace {

// Staged for the next begin(). setPending copies both so the caller's
// buffers need not outlive the call.
std::string s_pendingPath;
std::string s_pendingLabel;
int         s_pendingAbi = 0;

// Live launch state. The shell parses/executes the module IN PLACE, so the
// byte buffer must outlive it - both are torn down together in end().
WasmAppShell* s_shell    = nullptr;
uint8_t*      s_bytes    = nullptr;
size_t        s_len      = 0;
char          s_loadErr[96] = {0};
int           s_failedAbi = 0;
bool          s_preShellErrorCallbacks = false;
// The shell keeps only a const char* to its name (no copy), so the backing
// string must outlive the shell - hold it here, not in a begin() local.
std::string   s_runningLabel;

enum WasmCmd : uint8_t { CMD_UPDATE, CMD_END };
TaskHandle_t       s_wasmTask  = nullptr;
SemaphoreHandle_t  s_cmdReady  = nullptr;   // loop task -> guest task: a command is posted
SemaphoreHandle_t  s_cmdDone   = nullptr;   // guest task -> loop task: command finished
volatile WasmCmd   s_cmd       = CMD_UPDATE;
volatile uint32_t  s_guestStackFreeMin = 0xFFFFFFFF;  // min free bytes seen; survives teardown for wasmstat

#ifndef CF_WASM_GUEST_STACK_SIZE
#define CF_WASM_GUEST_STACK_SIZE (64 * 1024)
#endif

// Called ONLY on the guest task (samples its own stack; no dangling-handle risk).
void selfSampleGuestStack() {
    uint32_t freeBytes = (uint32_t)uxTaskGetStackHighWaterMark(nullptr) * sizeof(StackType_t);
    if (freeBytes < s_guestStackFreeMin) s_guestStackFreeMin = freeBytes;
}

void ensureSyncPrimitives() {
    if (!s_cmdReady) s_cmdReady = xSemaphoreCreateBinary();
    if (!s_cmdDone)  s_cmdDone  = xSemaphoreCreateBinary();
    // Drain any stale signal from a previous launch so each launch starts clean.
    while (xSemaphoreTake(s_cmdReady, 0) == pdTRUE) {}
    while (xSemaphoreTake(s_cmdDone,  0) == pdTRUE) {}
}

// The dedicated wasm task. s_shell is constructed by wasmFsAppBegin() before this
// task starts. Runs shell->begin() (deep app_begin), then one shell->update() per
// CMD_UPDATE, then shell->end() (deep app_end) on CMD_END, then self-deletes.
void wasmTaskEntry(void*) {
    if (s_shell) s_shell->begin();
    selfSampleGuestStack();
    xSemaphoreGive(s_cmdDone);              // begin-done rendezvous
    for (;;) {
        xSemaphoreTake(s_cmdReady, portMAX_DELAY);
        WasmCmd c = s_cmd;
        if (c == CMD_UPDATE) {
            if (s_shell) s_shell->update();
            selfSampleGuestStack();
            xSemaphoreGive(s_cmdDone);
        } else {                            // CMD_END
            if (s_shell) s_shell->end();
            selfSampleGuestStack();
            xSemaphoreGive(s_cmdDone);
            break;
        }
    }
    vTaskDelete(nullptr);                   // self-delete; loop task already dropped the handle
}

void freeBuffer() {
    if (s_bytes) { heap_caps_free(s_bytes); s_bytes = nullptr; }
    s_len = 0;
}

// Draw a self-contained load-failure screen. The runtime shell owns
// RUNTIME error containment; this covers the pre-shell failures (missing
// file, empty, oversized, out of memory) where no shell exists yet.
void drawLoadError(const char* line1, const char* line2) {
    auto& d = HAL::displayProxy();
    d.clear();
    d.setTextAlignment(TEXT_ALIGN_LEFT);
    d.setFont(ArialMT_Plain_10);
    d.drawString(2, 2, "App failed to load");
    if (line1) d.drawString(2, 16, line1);
    if (line2) d.drawString(2, 28, line2);
    d.drawString(2, 52, "press any button");
    d.display();
}

void drawAbiUnsupported() {
    char firmwareLine[32];
    snprintf(firmwareLine, sizeof(firmwareLine), "firmware (ABI %d)", kDeviceHalAbi);
    auto& d = HAL::displayProxy();
    d.clear();
    d.setTextAlignment(TEXT_ALIGN_CENTER);
    d.setFont(ArialMT_Plain_10);
    d.drawString(64, 18, "App needs newer");
    d.drawString(64, 32, firmwareLine);
    d.drawString(64, 52, "press any button");
    d.display();
}

void preShellErrorButton(const ButtonEvent& event) {
    if (event.eventType == ButtonEvent_Released) {
        MenuManager::instance().returnToMenu();
    }
}

void registerPreShellErrorCallbacks() {
    if (s_preShellErrorCallbacks) return;
    for (int i = 0; i < 6; ++i) {
        HAL::buttonManager().registerCallback(i, preShellErrorButton);
    }
    s_preShellErrorCallbacks = true;
}

void unregisterPreShellErrorCallbacks() {
    if (!s_preShellErrorCallbacks) return;
    for (int i = 0; i < 6; ++i) {
        HAL::buttonManager().unregisterCallback(i);
    }
    s_preShellErrorCallbacks = false;
}

// The whole loadout partition is 1.5 MB; a single app image far smaller.
// Cap the read so a corrupt manifest path can't try to allocate the world.
constexpr size_t kMaxModuleBytes = 512 * 1024;

}  // namespace

void setPending(const char* blobPath, const char* label, int abi) {
    s_pendingPath  = blobPath ? blobPath : "";
    s_pendingLabel = label ? label : "app";
    s_pendingAbi   = abi;
}

bool hasPending() { return !s_pendingPath.empty(); }

bool pendingAbiSupported() { return s_pendingAbi <= kDeviceHalAbi; }

void wasmFsAppBegin() {
    s_loadErr[0] = '\0';
    s_failedAbi = 0;
    std::string path  = s_pendingPath;
    std::string label = s_pendingLabel;
    int abi = s_pendingAbi;
    // Consume the staged launch so a return-to-menu can't accidentally
    // relaunch the same blob.
    s_pendingPath.clear();
    s_pendingAbi = 0;

    if (path.empty()) {
        snprintf(s_loadErr, sizeof(s_loadErr), "no app staged");
        drawLoadError("no app staged", nullptr);
        return;
    }

    // Refuse a blob whose declared cf.* surface exceeds the ABI this firmware
    // provides, before opening or parsing any guest bytes (REQ-063).
    if (abi > kDeviceHalAbi) {
        snprintf(s_loadErr, sizeof(s_loadErr), "abi_unsupported");
        s_failedAbi = abi;
        drawAbiUnsupported();
        registerPreShellErrorCallbacks();
        return;
    }

    File f = LittleFS.open(path.c_str(), "r");
    if (!f || f.isDirectory()) {
        if (f) f.close();
        snprintf(s_loadErr, sizeof(s_loadErr), "missing: %s", path.c_str());
        drawLoadError("file not found", path.c_str());
        return;
    }
    size_t size = f.size();
    if (size == 0 || size > kMaxModuleBytes) {
        f.close();
        snprintf(s_loadErr, sizeof(s_loadErr), "bad size %u", (unsigned)size);
        drawLoadError(size == 0 ? "empty file" : "file too large", path.c_str());
        return;
    }

    // PSRAM-first, like the runtime's own allocator; fall back to internal.
    s_bytes = (uint8_t*)heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    if (!s_bytes) s_bytes = (uint8_t*)heap_caps_malloc(size, MALLOC_CAP_8BIT);
    if (!s_bytes) {
        f.close();
        snprintf(s_loadErr, sizeof(s_loadErr), "out of memory (%u B)", (unsigned)size);
        drawLoadError("out of memory", nullptr);
        return;
    }
    size_t got = f.read(s_bytes, size);
    f.close();
    if (got != size) {
        freeBuffer();
        snprintf(s_loadErr, sizeof(s_loadErr), "read %u/%u", (unsigned)got, (unsigned)size);
        drawLoadError("read failed", path.c_str());
        return;
    }
    s_len = size;

    // Hand the owned buffer to the shell. From here EVERY failure mode -
    // parse error, missing exports, begin() trap, native-stack exhaustion -
    // is the shell's error screen + button-to-menu, never a reboot.
    // s_runningLabel backs the shell's non-owning name pointer for its life.
    s_runningLabel = label;
    s_shell = new WasmAppShell(s_runningLabel.c_str(), s_bytes, s_len);
    ensureSyncPrimitives();
    s_guestStackFreeMin = 0xFFFFFFFF;
    UBaseType_t prio = uxTaskPriorityGet(nullptr);   // run at the loop task's priority
    BaseType_t  core = xPortGetCoreID();             // pin to the loop task's core
    // NOTE: on ESP-IDF, xTaskCreate* stack size is in BYTES (not words).
    BaseType_t created = xTaskCreatePinnedToCore(
        wasmTaskEntry, "wasm_guest", CF_WASM_GUEST_STACK_SIZE, nullptr,
        prio, &s_wasmTask, core);
    if (created != pdPASS) {
        s_wasmTask = nullptr;
        delete s_shell; s_shell = nullptr;
        freeBuffer();
        snprintf(s_loadErr, sizeof(s_loadErr), "guest task alloc failed");
        drawLoadError("out of memory", "guest task");
        registerPreShellErrorCallbacks();
        return;
    }
    xSemaphoreTake(s_cmdDone, portMAX_DELAY);         // wait for shell->begin() (deep app_begin)
}

void wasmFsAppRun() {
    if (s_shell && s_wasmTask) {
        s_cmd = CMD_UPDATE;
        xSemaphoreGive(s_cmdReady);
        xSemaphoreTake(s_cmdDone, portMAX_DELAY);     // frame runs on the guest task; loop task blocks
        if (s_shell && s_shell->wantsExit()) {
            // Teardown on THIS (loop) task only — never on the guest task.
            MenuManager::instance().returnToMenu();   // -> switchToApp(MENU) -> wasmFsAppEnd()
        }
        return;
    }
    // Pre-shell load failure: hold the error screen; any button escapes via
    // the menu's own Back handling (the app manager still owns navigation).
    if (strcmp(s_loadErr, "abi_unsupported") == 0) {
        drawAbiUnsupported();
        return;
    }
    drawLoadError(s_loadErr[0] ? s_loadErr : "load failed", nullptr);
}

void wasmFsAppEnd() {
    unregisterPreShellErrorCallbacks();
    if (s_wasmTask) {
        s_cmd = CMD_END;
        xSemaphoreGive(s_cmdReady);
        xSemaphoreTake(s_cmdDone, portMAX_DELAY);     // shell->end() done on the guest task
        s_wasmTask = nullptr;                         // task self-deletes via vTaskDelete(nullptr)
    }
    if (s_shell) { delete s_shell; s_shell = nullptr; }  // safe: end() ran, guest task no longer touches it
    freeBuffer();
    HAL::setRgbLedsOff();
}

void statCli() {
    const char* src = s_shell ? s_shell->name() : "none";
    if (!s_shell) {
        if (strcmp(s_loadErr, "abi_unsupported") == 0) {
            Serial.printf("[cmd] wasmstat source=%s frames=0 error=abi_unsupported abi=%d abimax=%d\n",
                          src, s_failedAbi, kDeviceHalAbi);
            return;
        }
        Serial.printf("[cmd] wasmstat source=%s frames=0 error=%s\n",
                      src, s_loadErr[0] ? s_loadErr : "none");
        return;
    }
    const auto& st = s_shell->stats();
    long avgUs = st.frames ? (long)(st.sumUs / st.frames) : 0;
    uint32_t gsMin = (s_guestStackFreeMin == 0xFFFFFFFF) ? 0 : s_guestStackFreeMin;
    Serial.printf("[cmd] wasmstat source=%s frames=%u over_budget=%u "
                  "avg_us=%ld max_us=%ld guest_stack_free_min=%u error=%s\n",
                  src, st.frames, st.overBudget, avgUs, (long)st.maxUs,
                  (unsigned)gsMin,
                  s_shell->hasError() ? s_shell->errorText() : "none");
}

}  // namespace WasmFsApp
