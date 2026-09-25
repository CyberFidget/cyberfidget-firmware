// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#include "SavedWifi.h"

#ifndef HOST_TEST

#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <string.h>

namespace SavedWifi {
namespace {

using WifiList::List;

// Every read-change-write of the list holds this: the portal, the Settings
// screen and a session's join can each change it.
SemaphoreHandle_t storeLock() {
    static SemaphoreHandle_t lock = xSemaphoreCreateMutex();
    return lock;
}

struct Locked {
    Locked() { xSemaphoreTake(storeLock(), portMAX_DELAY); }
    ~Locked() { xSemaphoreGive(storeLock()); }
};

class PrefsStore : public WifiList::Store {
public:
    explicit PrefsStore(Preferences& p) : p_(p) {}
    bool has(const char* key) override { return p_.isKey(key); }
    bool getString(const char* key, char* out, size_t len) override {
        out[0] = '\0';
        if (!p_.isKey(key)) return false;
        p_.getString(key, out, len);
        return true;
    }
    uint32_t getUInt(const char* key) override { return p_.isKey(key) ? p_.getUInt(key, 0) : 0; }
    bool putString(const char* key, const char* value) override {
        // An empty value (an open network's password) reports 0 bytes either way.
        return p_.putString(key, value) == strlen(value);
    }
    bool putUInt(const char* key, uint32_t value) override { return p_.putUInt(key, value) == sizeof(uint32_t); }
    bool remove(const char* key) override { return !p_.isKey(key) || p_.remove(key); }
private:
    Preferences& p_;
};

// Callers hold the lock.
bool loadLocked(List& out) {
    WifiList::wipe(out);
    Preferences prefs;
    if (!prefs.begin(WifiList::kNamespace, false)) return false;
    PrefsStore store(prefs);
    bool repair = false;
    WifiList::load(store, out, repair);
    if (repair && !WifiList::save(store, out)) Serial.println("[wifi] store=repair-failed");
    prefs.end();
    return true;
}

bool saveLocked(const List& list) {
    Preferences prefs;
    if (!prefs.begin(WifiList::kNamespace, false)) return false;
    PrefsStore store(prefs);
    const bool ok = WifiList::save(store, list);
    prefs.end();
    return ok;
}

// The station's current access point, when it has one.
bool currentPlace(uint8_t& channel, uint8_t address[6]) {
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) return false;
    channel = ap.primary;
    memcpy(address, ap.bssid, 6);
    return true;
}

// Remembers a join that worked, against the list as stored now (it may have
// changed while the join ran).
void remember(const char* name) {
    uint8_t channel = 0;
    uint8_t address[6] = {0};
    const bool place = currentPlace(channel, address);
    Locked lock;
    List list;
    if (!loadLocked(list)) return;
    const int at = WifiList::find(list, name);
    if (at >= 0 && WifiList::markJoined(list, at, place ? channel : 0, place ? address : nullptr) &&
        !saveLocked(list)) {
        Serial.println("[wifi] store=remember-failed");
    }
    WifiList::wipe(list);
}

// Drops the remembered place of `name` when it is still the first network.
bool forgetPlace(const char* name) {
    Locked lock;
    List list;
    bool ok = loadLocked(list) && list.count > 0 && strcmp(list.nets[0].name, name) == 0;
    if (ok && list.hint.valid) {
        list.hint = WifiList::Hint();
        ok = saveLocked(list);
    }
    WifiList::wipe(list);
    return ok;
}

enum class Wait : uint8_t { Joined, TimedOut, Absent, Stopped };

Wait waitJoin(const JoinOptions& opt, uint32_t limitMs, bool endOnAbsent) {
    const uint32_t at = millis();
    for (;;) {
        const wl_status_t st = WiFi.status();
        if (st == WL_CONNECTED) return Wait::Joined;
        if (opt.stop && opt.stop(opt.ctx)) return Wait::Stopped;
        if (millis() - at >= limitMs) return Wait::TimedOut;
        if (endOnAbsent && st == WL_NO_SSID_AVAIL) return Wait::Absent;
        if (opt.tick) opt.tick(opt.ctx);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

// Active scan, 120 ms a channel at most: ~1.5 s for the whole band.
constexpr uint32_t kScanChannelMs = 120;
constexpr uint32_t kScanLimitMs = 5000;
constexpr int kScanStartTries = 15;   // 100 ms apart

#ifdef CF_TEST_CLI
// Bench: the station's own events while a join runs (why an attempt ended,
// whether the scan finished).
volatile bool tracing = false;
void traceEvent(arduino_event_t* e) {
    if (!tracing || !e) return;
    if (e->event_id == ARDUINO_EVENT_WIFI_STA_DISCONNECTED)
        Serial.printf("[wifi] event=disconnected reason=%u\n",
                      (unsigned)e->event_info.wifi_sta_disconnected.reason);
    else if (e->event_id == ARDUINO_EVENT_WIFI_SCAN_DONE)
        Serial.printf("[wifi] event=scan-done status=%u count=%u\n",
                      (unsigned)e->event_info.wifi_scan_done.status,
                      (unsigned)e->event_info.wifi_scan_done.number);
}
void traceJoin(bool on) {
    static bool registered = false;
    if (on && !registered) {
        WiFi.onEvent(traceEvent);
        registered = true;
    }
    tracing = on;
}
#else
void traceJoin(bool) {}
#endif

const char* waitName(Wait w) {
    switch (w) {
        case Wait::Joined:   return "ok";
        case Wait::TimedOut: return "timeout";
        case Wait::Absent:   return "absent";
        case Wait::Stopped:  return "stopped";
    }
    return "?";
}

} // namespace

bool load(List& out) {
    Locked lock;
    return loadLocked(out);
}

bool anySaved() {
    Preferences prefs;
    if (!prefs.begin(WifiList::kNamespace, true)) return false;
    // Read-only: the same reading as load(), without the repair write (this
    // runs on every awake tick).
    PrefsStore store(prefs);
    List list;
    bool repair = false;
    WifiList::load(store, list, repair);
    prefs.end();
    const bool any = list.count > 0;
    WifiList::wipe(list);
    return any;
}

int names(char out[][WifiList::kNameMax + 1], int max) {
    List list;
    load(list);
    int n = 0;
    for (; n < list.count && n < max; n++) {
        memcpy(out[n], list.nets[n].name, sizeof(list.nets[n].name));
    }
    WifiList::wipe(list);
    return n;
}

WifiList::AddResult add(const char* name, const char* pass) {
    Locked lock;
    List list;
    loadLocked(list);
    const WifiList::AddResult r = WifiList::add(list, name, pass);
    if ((r == WifiList::AddResult::Added || r == WifiList::AddResult::Updated) && !saveLocked(list)) {
        WifiList::wipe(list);
        return WifiList::AddResult::Invalid;
    }
    WifiList::wipe(list);
    return r;
}

bool forget(const char* name) {
    Locked lock;
    List list;
    loadLocked(list);
    const bool ok = WifiList::forget(list, name) && saveLocked(list);
    WifiList::wipe(list);
    return ok;
}

bool useFirst(const char* name) {
    Locked lock;
    List list;
    loadLocked(list);
    const bool ok = WifiList::useFirst(list, name) && saveLocked(list);
    WifiList::wipe(list);
    return ok;
}

void noteJoined(const char* name) {
    if (name && name[0] && WiFi.status() == WL_CONNECTED) remember(name);
}

bool join(const JoinOptions& opt, JoinResult& out) {
    out = JoinResult();
    const uint32_t start = millis();
    List list;
    load(list);
    if (list.count == 0) {
        out.noneSaved = true;
        Serial.println("[wifi] join result=none-saved");
        return false;
    }

    traceJoin(true);
    const char* firstName = list.nets[0].name;
    out.hinted = list.hint.valid;
#ifdef CF_TEST_CLI
    if (opt.benchFirstName && opt.benchFirstName[0]) {
        firstName = opt.benchFirstName;
        out.hinted = false;
    }
#endif
    if (out.hinted) WiFi.begin(firstName, list.nets[0].pass, list.hint.channel, list.hint.address);
    else WiFi.begin(firstName, list.nets[0].pass);
    Wait first = waitJoin(opt, opt.firstMs,
                          WifiList::firstEndsOnAbsent(list.count, out.hinted, opt.scheduled));
    out.firstMs = millis() - start;
    Wait last = first;
    char joinedName[WifiList::kNameMax + 1] = {0};
    if (first == Wait::Joined) {
        out.joined = 0;
        memcpy(joinedName, firstName, strlen(firstName) + 1);
    } else if (first != Wait::Stopped && WifiList::fallbackScan(list.count, out.hinted)) {
        // Stop the attempt and its own retries before scanning: the station
        // keeps reconnecting after "not found" by itself, and the radio
        // refuses a scan while it does (bench: every start failed).
        WiFi.setAutoReconnect(false);
        WiFi.disconnect(false, false, 1000);
        out.scanned = true;
        const uint32_t scanAt = millis();
        // Retry briefly while the stopped attempt winds down.
        // Started directly so a refusal says why; the WiFi library's own
        // scan-done handler still collects the results (scanComplete() >= 0
        // once they are in - scanDelete() cleared the previous ones).
        WiFi.scanDelete();
        wifi_scan_config_t cfg = {};
        cfg.scan_type = WIFI_SCAN_TYPE_ACTIVE;
        cfg.scan_time.active.min = 100;
        cfg.scan_time.active.max = kScanChannelMs;
        esp_err_t startErr = ESP_FAIL;
        int starts = 0;
        while (startErr != ESP_OK && starts < kScanStartTries) {
            // A retry the station queued before the stop can still start a
            // connect after it; stop again before every try.
            esp_wifi_disconnect();
            startErr = esp_wifi_scan_start(&cfg, false);
            starts++;
            if (startErr == ESP_OK) break;
            if (opt.stop && opt.stop(opt.ctx)) { last = Wait::Stopped; break; }
            if (opt.tick) opt.tick(opt.ctx);
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        out.scanStarts = (uint8_t)starts;
        int16_t found = WIFI_SCAN_FAILED;
        bool finished = false;
        while (startErr == ESP_OK && last != Wait::Stopped) {
            found = WiFi.scanComplete();
            if (found >= 0) { finished = true; break; }
            if (opt.stop && opt.stop(opt.ctx)) { last = Wait::Stopped; break; }
            if (millis() - scanAt >= kScanLimitMs) break;
            if (opt.tick) opt.tick(opt.ctx);
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        if (startErr == ESP_OK && !finished) esp_wifi_scan_stop();
        WiFi.setAutoReconnect(true);   // the default every other user expects
        out.scanFailed = !finished;
        if (out.scanFailed) {
            Serial.printf("[wifi] scan=no-result start_err=%s status=%d\n", esp_err_to_name(startErr),
                          (int)WiFi.status());
            found = WIFI_SCAN_FAILED;
        }
        int pick = -1;
        uint8_t channel = 0;
        uint8_t address[6] = {0};
        // Every record is looked at (a busy place can list dozens); only
        // the best saved one so far and where it is are kept.
        WifiList::Pick best;
        for (int i = 0; i < found; i++) {
            const wifi_ap_record_t* rec =
                static_cast<const wifi_ap_record_t*>(WiFi.getScanInfoByIndex(i));
            if (!rec) continue;
            char seenName[WifiList::kNameMax + 1];
            memcpy(seenName, rec->ssid, WifiList::kNameMax);
            seenName[WifiList::kNameMax] = '\0';
            if (WifiList::consider(list, seenName, rec->rssi, best)) {
                channel = rec->primary;
                memcpy(address, rec->bssid, 6);
            }
        }
        pick = best.index;
        WiFi.scanDelete();
        out.scanMs = millis() - scanAt;
        if (last != Wait::Stopped) {
            if (out.scanFailed) {
                // No scan to choose from: a plain join of the first network,
                // whose own connect looks on every channel.
                const uint32_t joinAt = millis();
                WiFi.begin(list.nets[0].name, list.nets[0].pass);
                last = waitJoin(opt, opt.fallbackMs, false);
                out.fallbackMs = millis() - joinAt;
                if (last == Wait::Joined) {
                    out.joined = 0;
                    memcpy(joinedName, list.nets[0].name, sizeof(joinedName));
                }
            } else if (pick < 0) {
                last = Wait::Absent;
            } else {
                const uint32_t joinAt = millis();
                WiFi.begin(list.nets[pick].name, list.nets[pick].pass, channel, address);
                // The scan just saw it: wait the budget out rather than
                // trusting a status left over from the first attempt.
                last = waitJoin(opt, opt.fallbackMs, false);
                out.fallbackMs = millis() - joinAt;
                if (last == Wait::Joined) {
                    out.joined = (int8_t)pick;
                    memcpy(joinedName, list.nets[pick].name, sizeof(joinedName));
                }
            }
        }
    }
    const int count = list.count;
    char firstSaved[WifiList::kNameMax + 1];
    memcpy(firstSaved, list.nets[0].name, sizeof(firstSaved));
    WifiList::wipe(list);

    out.ok = last == Wait::Joined;
    out.stopped = last == Wait::Stopped;
    out.absent = last == Wait::Absent;
    // (A bench name is not in the list, so it is never remembered.)
    if (out.ok) remember(joinedName);
    // A remembered place that led nowhere is dropped: the next session's
    // first attempt is a plain join, which looks on every channel itself.
    else if (out.hinted && !out.stopped) forgetPlace(firstSaved);
    traceJoin(false);
    out.totalMs = millis() - start;
    Serial.printf("[wifi] join saved=%d first=%s first_result=%s first_ms=%u scan=%d scan_starts=%u "
                  "scan_ms=%u fallback_ms=%u result=%s slot=%d total_ms=%u\n",
                  count, out.hinted ? "remembered" : "plain", waitName(first),
                  (unsigned)out.firstMs, out.scanned ? (out.scanFailed ? 2 : 1) : 0,
                  (unsigned)out.scanStarts,
                  (unsigned)out.scanMs, (unsigned)out.fallbackMs, waitName(last), (int)out.joined,
                  (unsigned)out.totalMs);
    return out.ok;
}

#ifdef CF_TEST_CLI
void cliCommand(const char* args) {
    while (*args == ' ') args++;
    if (strcmp(args, "list") == 0) {
        List list;
        load(list);
        Serial.printf("[cmd] wifi.count=%d place=%d\n", list.count, list.hint.valid ? 1 : 0);
        for (int i = 0; i < list.count; i++)
            Serial.printf("[cmd] wifi.net=%d name=%s\n", i, list.nets[i].name);
        WifiList::wipe(list);
        return;
    }
    if (strncmp(args, "first ", 6) == 0) {
        Serial.printf("[cmd] wifi.first=%d\n", useFirst(args + 6) ? 1 : 0);
        return;
    }
    if (strncmp(args, "forget ", 7) == 0) {
        Serial.printf("[cmd] wifi.forget=%d\n", forget(args + 7) ? 1 : 0);
        return;
    }
    if (strcmp(args, "hint-bad") == 0) {
        // The remembered place points at an access point that is not there:
        // the quick join fails as it would with the network out of range.
        Locked lock;
        List list;
        bool ok = loadLocked(list) && list.count > 0 && list.hint.valid;
        if (ok) {
            static const uint8_t kNowhere[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01};
            memcpy(list.hint.address, kNowhere, 6);
            ok = saveLocked(list);
        }
        WifiList::wipe(list);
        Serial.printf("[cmd] wifi.hint_bad=%d\n", ok ? 1 : 0);
        return;
    }
    if (strcmp(args, "hint-clear") == 0) {
        // The next join is a plain one (the way every join worked before).
        char first[1][WifiList::kNameMax + 1];
        const bool ok = names(first, 1) == 1 && forgetPlace(first[0]);
        Serial.printf("[cmd] wifi.hint_clear=%d\n", ok ? 1 : 0);
        return;
    }
    Serial.println("[err] usage: wifi <name>|<pass> | wifi list | wifi first <name> | "
                   "wifi forget <name> | wifi hint-bad | wifi hint-clear");
}
#endif

} // namespace SavedWifi

#endif // HOST_TEST
