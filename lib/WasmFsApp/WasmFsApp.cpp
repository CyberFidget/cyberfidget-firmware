// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2026 Dismo Industries LLC

#include "WasmFsApp.h"

#include <Arduino.h>
#include <LittleFS.h>
#include <esp_heap_caps.h>

#include <string>

#include "DisplayProxy.h"
#include "HAL.h"
#include "WasmAppShell.h"

namespace WasmFsApp {
namespace {

// Staged for the next begin(). setPending copies both so the caller's
// buffers need not outlive the call.
std::string s_pendingPath;
std::string s_pendingLabel;

// Live launch state. The shell parses/executes the module IN PLACE, so the
// byte buffer must outlive it - both are torn down together in end().
WasmAppShell* s_shell    = nullptr;
uint8_t*      s_bytes    = nullptr;
size_t        s_len      = 0;
char          s_loadErr[96] = {0};
// The shell keeps only a const char* to its name (no copy), so the backing
// string must outlive the shell - hold it here, not in a begin() local.
std::string   s_runningLabel;

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

// The whole loadout partition is 1.5 MB; a single app image far smaller.
// Cap the read so a corrupt manifest path can't try to allocate the world.
constexpr size_t kMaxModuleBytes = 512 * 1024;

}  // namespace

void setPending(const char* blobPath, const char* label) {
    s_pendingPath  = blobPath ? blobPath : "";
    s_pendingLabel = label ? label : "app";
}

bool hasPending() { return !s_pendingPath.empty(); }

void wasmFsAppBegin() {
    s_loadErr[0] = '\0';
    std::string path  = s_pendingPath;
    std::string label = s_pendingLabel;
    // Consume the staged launch so a return-to-menu can't accidentally
    // relaunch the same blob.
    s_pendingPath.clear();

    if (path.empty()) {
        snprintf(s_loadErr, sizeof(s_loadErr), "no app staged");
        drawLoadError("no app staged", nullptr);
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
    s_shell->begin();
}

void wasmFsAppRun() {
    if (s_shell) { s_shell->update(); return; }
    // Pre-shell load failure: hold the error screen; any button escapes via
    // the menu's own Back handling (the app manager still owns navigation).
    drawLoadError(s_loadErr[0] ? s_loadErr : "load failed", nullptr);
}

void wasmFsAppEnd() {
    if (s_shell) { s_shell->end(); delete s_shell; s_shell = nullptr; }
    freeBuffer();
    HAL::setRgbLedsOff();
}

void statCli() {
    const char* src = s_shell ? s_shell->name() : "none";
    if (!s_shell) {
        Serial.printf("[cmd] wasmstat source=%s frames=0 error=%s\n",
                      src, s_loadErr[0] ? s_loadErr : "none");
        return;
    }
    const auto& st = s_shell->stats();
    long avgUs = st.frames ? (long)(st.sumUs / st.frames) : 0;
    Serial.printf("[cmd] wasmstat source=%s frames=%u over_budget=%u "
                  "avg_us=%ld max_us=%ld error=%s\n",
                  src, st.frames, st.overBudget, avgUs, (long)st.maxUs,
                  s_shell->hasError() ? s_shell->errorText() : "none");
}

}  // namespace WasmFsApp
