// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2026 Dismo Industries LLC

#include "WasmFsApp.h"

#include <Arduino.h>
#include <LittleFS.h>
#include <esp_expression_with_stack.h>
#include <esp_attr.h>
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
#include "LoadoutManifest.h"
#include "LoadoutStore.h"

namespace WasmFsApp {
namespace {

// Staged for the next begin(). setPending copies both so the caller's
// buffers need not outlive the call.
std::string s_pendingPath;
std::string s_pendingLabel;
int         s_pendingAbi = 0;
std::string s_pendingId;
// The last launch (kept after it ends, for dev mode's relaunch).
std::string s_lastId;
std::string s_lastPath;

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

// The interpreter's stack. wasm3 recurses on the native stack for every
// guest call AND nests one native frame per interpreted op (no tail calls
// on this CPU), so the depth an app needs depends on how much straight-line
// code it runs, not just on recursion. Bench, every catalog device app:
// worst used 54,756 B (Spaceship, 5 min of play; it levels off there),
// Breakout 44,180 B (level skips), the rest 4.8-31.3 KB. The guard below
// leaves 16 KB in reserve, so 128 KB gives an app 112 KB - just over twice
// the worst. That is far more than internal RAM can spare, so the stack
// lives in PSRAM (see guestMain; frames measured 12-19% slower than on the
// old internal stack). The native-stack guard (WasmAppRuntime) traps a guest
// that goes deeper, leaving its reserve for host calls and the trap itself.
#ifndef CF_WASM_GUEST_STACK_SIZE
#define CF_WASM_GUEST_STACK_SIZE (128 * 1024)
#endif
constexpr size_t kGuestStackBytes = CF_WASM_GUEST_STACK_SIZE;
// The guest task's own (internal RAM) stack: it only holds the frames that
// switch onto the PSRAM stack and back.
constexpr uint32_t kGuestTaskStackBytes = 4096;

uint8_t*          s_guestStack     = nullptr;   // PSRAM, kGuestStackBytes
SemaphoreHandle_t s_guestStackLock = nullptr;   // required by the stack switch

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

// The whole app runs here, on the PSRAM stack. s_shell is constructed by
// wasmFsAppBegin() before the task starts. Runs shell->begin() (deep
// app_begin), then one shell->update() per CMD_UPDATE, then shell->end()
// (deep app_end) on CMD_END. The END rendezvous is given by wasmTaskEntry
// only after the stack is switched back, so the loop task never frees the
// PSRAM stack while it is in use. Host calls made from here must not use the
// SPI flash driver at all - reads included (LittleFS, Preferences/NVS,
// esp_partition, OTA): every driver operation turns the cache (and PSRAM
// with it) off, and the SDK asserts when the caller's stack is in PSRAM
// (esp_task_stack_is_sane_cache_disabled). None do today. Flash work by
// OTHER tasks is fine: the SDK parks this task before the cache goes off.
void guestMain() {
    if (s_shell) s_shell->begin();
    selfSampleGuestStack();
    xSemaphoreGive(s_cmdDone);              // begin-done rendezvous
    for (;;) {
        xSemaphoreTake(s_cmdReady, portMAX_DELAY);
        WasmCmd c = s_cmd;
        if (c == CMD_UPDATE) {
            if (s_shell) s_shell->update();
            // The high-water mark scans the unused part of the PSRAM stack
            // (up to ~100 KB) and never goes back up, so every 32nd frame
            // (and at begin and end) loses nothing but freshness.
            static uint8_t frames = 0;
            if ((++frames & 31u) == 0) selfSampleGuestStack();
            xSemaphoreGive(s_cmdDone);
        } else {                            // CMD_END
            if (s_shell) s_shell->end();
            selfSampleGuestStack();
            return;
        }
    }
}

// The dedicated wasm task: switches onto the PSRAM stack (the switch fills
// it with the FreeRTOS pattern and points the task's stack bounds at it, so
// the high-water mark and the native-stack guard both measure it), runs the
// app, switches back, then self-deletes.
void wasmTaskEntry(void*) {
    esp_execute_shared_stack_function(s_guestStackLock, s_guestStack, kGuestStackBytes, guestMain);
    xSemaphoreGive(s_cmdDone);              // end-done rendezvous: the PSRAM stack is free
    vTaskDelete(nullptr);                   // self-delete; loop task already dropped the handle
}

void freeGuestStack() {
    if (s_guestStack) { heap_caps_free(s_guestStack); s_guestStack = nullptr; }
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

// The firmware release the website stamped for this app ("1.5.0"), or ""
// when the entry has none or it isn't a plain x.y.z. People are told a
// release number, never the HAL level.
std::string minFirmwareFor(const std::string& id) {
    if (id.empty()) return "";
    std::string json;
    LoadoutManifest::Loadout lo;
    {
        LoadoutStore::Guard guard;
        if (!LoadoutStore::load(json)) return "";
    }
    if (!LoadoutManifest::parseManifest(json.c_str(), lo)) return "";
    for (const auto& e : lo.entries) {
        if (e.id != id) continue;
        return LoadoutManifest::isReleaseVersion(e.minFirmware) ? e.minFirmware : "";
    }
    return "";
}

// Looked up once per launch: the refusal screen is redrawn every frame.
std::string s_minFw;
bool        s_minFwLoaded = false;

void drawAbiUnsupported() {
    if (!s_minFwLoaded) {
        s_minFw = minFirmwareFor(s_lastId);
        s_minFwLoaded = true;
    }
    const std::string& minFw = s_minFw;
    char line2[32];
    if (!minFw.empty()) snprintf(line2, sizeof(line2), "%s or newer", minFw.c_str());
    auto& d = HAL::displayProxy();
    d.clear();
    d.setTextAlignment(TEXT_ALIGN_CENTER);
    d.setFont(ArialMT_Plain_10);
    d.drawString(64, 18, minFw.empty() ? "This app needs" : "App needs firmware");
    d.drawString(64, 32, minFw.empty() ? "newer firmware" : line2);
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

// Every pre-shell failure (nothing staged, missing/empty/oversized file,
// out of memory, read error, no guest task, ABI refused) ends here: the
// error screen AND the any-button-returns-to-menu callbacks, together. The
// menu dropped its own callbacks when this app began, so an error screen
// without these would ignore every button until a hard reset.
void showLoadError(const char* line1, const char* line2) {
    drawLoadError(line1, line2);
    registerPreShellErrorCallbacks();
}

// The whole loadout partition is 1.5 MB; a single app image far smaller.
// Cap the read so a corrupt manifest path can't try to allocate the world.
constexpr size_t kMaxModuleBytes = 512 * 1024;

}  // namespace

void setPending(const char* blobPath, const char* label, int abi, const char* id) {
    s_pendingPath  = blobPath ? blobPath : "";
    s_pendingLabel = label ? label : "app";
    s_pendingAbi   = abi;
    s_pendingId    = id ? id : "";
}

bool lastLaunch(std::string& id, std::string& path) {
    if (s_lastId.empty() || s_lastPath.empty()) return false;
    id = s_lastId;
    path = s_lastPath;
    return true;
}

bool hasPending() { return !s_pendingPath.empty(); }

bool pendingAbiSupported() { return s_pendingAbi <= kDeviceHalAbi; }

bool pendingLaunch(std::string& id, std::string& label) {
    if (s_pendingPath.empty()) return false;
    id = s_pendingId;
    label = s_pendingLabel;
    return true;
}

static bool guestInternalFits() {
    // The task's small internal stack plus its control block.
    return heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) >=
           (size_t)kGuestTaskStackBytes + 512;
}

static bool guestPsramFits() {
    // The interpreter's stack.
    return heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) >= kGuestStackBytes;
}

bool guestStackFits() { return guestInternalFits() && guestPsramFits(); }

#ifdef CF_TEST_CLI
// Test builds: pretend internal RAM is short so the restart-with-resume
// fallback can be benched. Survives the software restart, so the bench also
// proves the resumed start does not restart again.
static RTC_NOINIT_ATTR uint32_t s_forceRestartMagic;
static constexpr uint32_t kForceRestartOn = 0x57A5C0DEu;
void testForceRestart(bool on) { s_forceRestartMagic = on ? kForceRestartOn : 0; }
#endif

bool guestRestartHelps() {
#ifdef CF_TEST_CLI
    if (s_forceRestartMagic == kForceRestartOn) return true;
#endif
    // A fresh start only defragments internal RAM. A PSRAM shortfall (or no
    // PSRAM at all) survives a restart, so the app shows its out-of-memory
    // screen instead of restarting forever.
    return !guestInternalFits() && guestPsramFits();
}

void wasmFsAppBegin() {
    s_loadErr[0] = '\0';
    s_failedAbi = 0;
    s_minFwLoaded = false;
    std::string path  = s_pendingPath;
    std::string label = s_pendingLabel;
    int abi = s_pendingAbi;
    s_lastId = s_pendingId;
    s_lastPath = path;
    // Consume the staged launch so a return-to-menu can't accidentally
    // relaunch the same blob.
    s_pendingPath.clear();
    s_pendingId.clear();
    s_pendingAbi = 0;

    if (path.empty()) {
        snprintf(s_loadErr, sizeof(s_loadErr), "no app staged");
        showLoadError("no app staged", nullptr);
        return;
    }

    // Refuse a blob whose declared cf.* surface exceeds the ABI this firmware
    // provides, before opening or parsing any guest bytes.
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
        showLoadError("file not found", path.c_str());
        return;
    }
    size_t size = f.size();
    if (size == 0 || size > kMaxModuleBytes) {
        f.close();
        snprintf(s_loadErr, sizeof(s_loadErr), "bad size %u", (unsigned)size);
        showLoadError(size == 0 ? "empty file" : "file too large", path.c_str());
        return;
    }

    // PSRAM-first, like the runtime's own allocator; fall back to internal.
    s_bytes = (uint8_t*)heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    if (!s_bytes) s_bytes = (uint8_t*)heap_caps_malloc(size, MALLOC_CAP_8BIT);
    if (!s_bytes) {
        f.close();
        snprintf(s_loadErr, sizeof(s_loadErr), "out of memory (%u B)", (unsigned)size);
        showLoadError("out of memory", nullptr);
        return;
    }
    size_t got = f.read(s_bytes, size);
    f.close();
    if (got != size) {
        freeBuffer();
        snprintf(s_loadErr, sizeof(s_loadErr), "read %u/%u", (unsigned)got, (unsigned)size);
        showLoadError("read failed", path.c_str());
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
    if (!s_guestStackLock) s_guestStackLock = xSemaphoreCreateMutex();
    // Plain malloc (the aligned allocator would pull ~0.7 KB into IRAM, which
    // has no room): the stack switch aligns the top to 16 bytes itself.
    s_guestStack = (uint8_t*)heap_caps_malloc(kGuestStackBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    UBaseType_t prio = uxTaskPriorityGet(nullptr);   // run at the loop task's priority
    BaseType_t  core = xPortGetCoreID();             // pin to the loop task's core
    // NOTE: on ESP-IDF, xTaskCreate* stack size is in BYTES (not words).
    BaseType_t created = (s_guestStack && s_guestStackLock)
        ? xTaskCreatePinnedToCore(wasmTaskEntry, "wasm_guest", kGuestTaskStackBytes, nullptr,
                                  prio, &s_wasmTask, core)
        : pdFAIL;
    if (created != pdPASS) {
        s_wasmTask = nullptr;
        freeGuestStack();
        delete s_shell; s_shell = nullptr;
        freeBuffer();
        snprintf(s_loadErr, sizeof(s_loadErr), "guest task alloc failed");
        showLoadError("out of memory", nullptr);
        return;
    }
    xSemaphoreTake(s_cmdDone, portMAX_DELAY);         // wait for shell->begin() (deep app_begin)
    if (s_shell->hasError() && strcmp(s_shell->errorText(), "abi_unsupported") == 0) {
        // Reuse the pre-shell refusal outcome, including CLI reporting and
        // button handling, after safely joining the failed guest task.
        wasmFsAppEnd();
        snprintf(s_loadErr, sizeof(s_loadErr), "abi_unsupported");
        s_failedAbi = abi;
        drawAbiUnsupported();
        registerPreShellErrorCallbacks();
    }
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
    // Pre-shell load failure: hold the error screen. No shell means nothing
    // else owns the buttons, so (re)assert the any-button-returns-to-menu
    // callbacks every frame (idempotent): no path into this state can leave
    // the buttons dead.
    registerPreShellErrorCallbacks();
    if (strcmp(s_loadErr, "abi_unsupported") == 0) {
        drawAbiUnsupported();
        return;
    }
    drawLoadError(s_loadErr[0] ? s_loadErr : "load failed", nullptr);
}

bool abiUnsupported() { return strcmp(s_loadErr, "abi_unsupported") == 0; }

void wasmFsAppEnd() {
    unregisterPreShellErrorCallbacks();
    if (s_wasmTask) {
        s_cmd = CMD_END;
        xSemaphoreGive(s_cmdReady);
        xSemaphoreTake(s_cmdDone, portMAX_DELAY);     // shell->end() done, back on the task's own stack
        s_wasmTask = nullptr;                         // task self-deletes via vTaskDelete(nullptr)
    }
    freeGuestStack();
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
