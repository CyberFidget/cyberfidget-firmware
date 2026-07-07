// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#include "SerialCli.h"

#include <Arduino.h>
#include <esp_system.h>

#include "globals.h"  // getFirmwareVersionString() and friends
#include "version.h"  // FW_GIT_DIRTY for the `info` command

#ifdef CF_TEST_CLI
#include <Preferences.h>
#include <WiFi.h>

#include <esp_heap_caps.h>

#include "AppDefs.h"
#include "AppManager.h"
#include "MicCapture.h"
#endif

namespace {
// Case-insensitive C-string compare (avoids depending on platform strcasecmp,
// which has different headers across toolchains).
bool ieq(const char* a, const char* b) {
    while (*a && *b) {
        char ca = *a++;
        char cb = *b++;
        if (ca >= 'A' && ca <= 'Z') ca = ca - 'A' + 'a';
        if (cb >= 'A' && cb <= 'Z') cb = cb - 'A' + 'a';
        if (ca != cb) return false;
    }
    return *a == 0 && *b == 0;
}

#ifdef CF_TEST_CLI
// True if `line` starts with `verb` followed by a space; sets *arg to the
// first character after the space run. Case-insensitive on the verb.
bool verbWithArg(const char* line, const char* verb, const char** arg) {
    size_t n = strlen(verb);
    for (size_t i = 0; i < n; ++i) {
        char ca = line[i];
        char cb = verb[i];
        if (ca >= 'A' && ca <= 'Z') ca = ca - 'A' + 'a';
        if (ca != cb) return false;
    }
    if (line[n] != ' ') return false;
    const char* p = line + n;
    while (*p == ' ') ++p;
    if (*p == '\0') return false;
    *arg = p;
    return true;
}
#endif
}  // namespace

SerialCli& SerialCli::instance() {
    static SerialCli singleton;
    return singleton;
}

void SerialCli::poll() {
    while (Serial.available() > 0) {
        int byte = Serial.read();
        if (byte < 0) break;
        char c = static_cast<char>(byte);
        if (c == '\n' || c == '\r') {
            if (overflow) {
                Serial.println("[err] line too long");
                overflow = false;
                bufferLen = 0;
                continue;
            }
            if (bufferLen == 0) continue;  // ignore empty lines / lone \r before \n
            buffer[bufferLen] = '\0';
            dispatch(buffer);
            bufferLen = 0;
            continue;
        }
        if (bufferLen + 1 >= kBufferSize) {
            // No room for char + null terminator. Mark overflow; drain until newline.
            overflow = true;
            continue;
        }
        buffer[bufferLen++] = c;
    }
}

void SerialCli::dispatch(const char* line) {
    if (ieq(line, "version")) { cmdVersion(); return; }
    if (ieq(line, "info"))    { cmdInfo();    return; }
    if (ieq(line, "help"))    { cmdHelp();    return; }
#ifdef CF_TEST_CLI
    const char* arg = nullptr;
    if (ieq(line, "apps")) { cmdApps(); return; }
    if (ieq(line, "app"))  { cmdApp();  return; }
    if (ieq(line, "net"))  { cmdNet();  return; }
    if (ieq(line, "mic"))  { cmdMic();  return; }
    if (verbWithArg(line, "launch", &arg)) { cmdLaunch(arg); return; }
    if (verbWithArg(line, "wifi", &arg))   { cmdWifi(arg);   return; }
#endif
    Serial.printf("[err] unknown command: %s\n", line);
}

void SerialCli::cmdVersion() {
    Serial.printf("[cmd] version=%s\n", getFirmwareVersionString());
}

void SerialCli::cmdInfo() {
    uint64_t mac = ESP.getEfuseMac();
    Serial.printf("[cmd] info.fw=%s\n",      getFirmwareVersionString());
    Serial.printf("[cmd] info.type=%s\n",    getFirmwareBuildType());
    Serial.printf("[cmd] info.built=%s\n",   getFirmwareBuildTimestamp());
    Serial.printf("[cmd] info.git=%s\n",     getFirmwareGitHash());
    Serial.printf("[cmd] info.dirty=%d\n",   FW_GIT_DIRTY);
    Serial.printf("[cmd] info.chip=%s rev %d\n",
                  ESP.getChipModel(), ESP.getChipRevision());
    Serial.printf("[cmd] info.mac=%02X:%02X:%02X:%02X:%02X:%02X\n",
                  static_cast<uint8_t>((mac >> 40) & 0xFF),
                  static_cast<uint8_t>((mac >> 32) & 0xFF),
                  static_cast<uint8_t>((mac >> 24) & 0xFF),
                  static_cast<uint8_t>((mac >> 16) & 0xFF),
                  static_cast<uint8_t>((mac >>  8) & 0xFF),
                  static_cast<uint8_t>((mac >>  0) & 0xFF));
    Serial.printf("[cmd] info.uptime_ms=%lu\n", static_cast<unsigned long>(millis()));
}

void SerialCli::cmdHelp() {
#ifdef CF_TEST_CLI
    Serial.println("[cmd] help=version,info,help,apps,launch <name|index>,app,net,mic,wifi <ssid>|<pass>");
#else
    Serial.println("[cmd] help=version,info,help");
#endif
}

#ifdef CF_TEST_CLI
// =========================================================================
// Test-mode device-control commands (-DCF_TEST_CLI=1, `local_test` env).
// Output keeps the stable [cmd]/[err] line prefixes so a harness can parse
// without regex acrobatics. The flow these exist for: a test agent flashes
// the device, `wifi <ssid>|<pass>` saves LAN credentials, `launch <portal>`
// opens the web portal, `net` reports the IP to point a browser at.
// =========================================================================

void SerialCli::cmdApps() {
    for (int i = 0; i < APP_COUNT; ++i) {
        // The menu has an empty label; report it as "menu" so it stays
        // addressable.
        const char* name = (appDefs[i].name[0] != '\0') ? appDefs[i].name : "menu";
        Serial.printf("[cmd] apps.%d=%s\n", i, name);
    }
}

void SerialCli::cmdLaunch(const char* arg) {
    int target = -1;
    if (arg[0] >= '0' && arg[0] <= '9') {
        target = atoi(arg);
        if (target < 0 || target >= APP_COUNT) target = -1;
    } else if (ieq(arg, "menu")) {
        target = APP_MENU;
    } else {
        for (int i = 0; i < APP_COUNT; ++i) {
            if (ieq(arg, appDefs[i].name)) { target = i; break; }
        }
    }
    if (target < 0) {
        Serial.printf("[err] unknown app: %s (try `apps`)\n", arg);
        return;
    }
    AppManager::instance().switchToApp((AppIndex)target);
    Serial.printf("[cmd] launch.ok=%d\n", target);
}

void SerialCli::cmdApp() {
    AppIndex idx = AppManager::instance().activeApp();
    const char* name = (appDefs[idx].name[0] != '\0') ? appDefs[idx].name : "menu";
    Serial.printf("[cmd] app.index=%d\n", (int)idx);
    Serial.printf("[cmd] app.name=%s\n", name);
    Serial.printf("[cmd] app.uptime_ms=%lu\n", static_cast<unsigned long>(millis()));
}

void SerialCli::cmdNet() {
    wifi_mode_t mode = WiFi.getMode();
    Serial.printf("[cmd] net.mode=%d\n", (int)mode);
    if (mode == WIFI_AP || mode == WIFI_AP_STA) {
        Serial.printf("[cmd] net.ap_ip=%s\n", WiFi.softAPIP().toString().c_str());
        Serial.printf("[cmd] net.ap_clients=%d\n", WiFi.softAPgetStationNum());
    }
    if (mode == WIFI_STA || mode == WIFI_AP_STA) {
        bool up = (WiFi.status() == WL_CONNECTED);
        Serial.printf("[cmd] net.sta_connected=%d\n", up ? 1 : 0);
        if (up) {
            Serial.printf("[cmd] net.sta_ssid=%s\n", WiFi.SSID().c_str());
            Serial.printf("[cmd] net.sta_ip=%s\n", WiFi.localIP().toString().c_str());
        }
    }
}

// Mic pipeline diagnostic: acquire the shared capture service in the
// CURRENT app context (whatever is running — that's the point: it can
// reproduce a context-dependent open failure), read the post-gain peak for
// ~300ms, release. Refuses politely if an app holds the mic.
void SerialCli::cmdMic() {
    MicCapture& mic = MicCapture::instance();
    // Internal-heap picture first: acquire needs ~4KB DMA + a 4KB task
    // stack from INTERNAL ram, so free vs largest-block tells apart
    // "exhausted" from "fragmented" when it fails.
    Serial.printf("[cmd] mic.heap_free=%u largest=%u min_ever=%u\n",
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                  (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (mic.acquired()) {
        Serial.printf("[cmd] mic.held_by=%s\n", mic.ownerTag());
        return;
    }
    const char* err = nullptr;
    // The live stream's heap-diet config — this diagnostic exists to prove
    // the portal context can open exactly this.
    if (!mic.acquire("cli", 16000, &err, 4)) {
        Serial.printf("[err] mic acquire failed: %s\n", err ? err : "?");
        return;
    }
    Serial.println("[cmd] mic.acquired=1");
    Serial.printf("[cmd] mic.heap_after=%u largest=%u\n",
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    mic.clearVuPeak();
    mic.startStreaming(0);
    delay(300);
    uint16_t peak = mic.vuPeakExchange();
    uint32_t avail = mic.ring().available();
    mic.stopStreaming();
    mic.release();
    Serial.printf("[cmd] mic.peak=%u\n", (unsigned)peak);
    Serial.printf("[cmd] mic.ring_bytes_300ms=%lu\n", (unsigned long)avail);
    Serial.println("[cmd] mic.released=1");
}

void SerialCli::cmdWifi(const char* arg) {
    // `wifi <ssid>|<pass>` — '|' separates because SSIDs may contain spaces.
    // An omitted pass ("wifi MyNet|") saves an open network.
    const char* sep = strchr(arg, '|');
    if (sep == nullptr || sep == arg) {
        Serial.println("[err] usage: wifi <ssid>|<pass>");
        return;
    }
    char ssid[33];
    size_t n = (size_t)(sep - arg);
    if (n > sizeof(ssid) - 1) n = sizeof(ssid) - 1;
    memcpy(ssid, arg, n);
    ssid[n] = '\0';
    const char* pass = sep + 1;

    // Same NVS keys the web portal reads at startup (loadWifiCreds), so the
    // next `launch` of the portal auto-connects to this network.
    Preferences prefs;
    if (!prefs.begin("wificfg", false)) {
        Serial.println("[err] wifi store failed");
        return;
    }
    prefs.putString("ssid", ssid);
    prefs.putString("pass", pass);
    prefs.end();
    Serial.printf("[cmd] wifi.saved=%s\n", ssid);
}
#endif  // CF_TEST_CLI
