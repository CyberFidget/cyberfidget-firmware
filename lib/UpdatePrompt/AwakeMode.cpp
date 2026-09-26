// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#include "AwakeMode.h"

#include <Arduino.h>
#include <Preferences.h>
#include <ctype.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>

#include "AppManager.h"
#include "CloudSync.h"
#include "DisplayProxy.h"
#include "HAL.h"
#include "LoadoutManifest.h"
#include "LoadoutStore.h"
#include "MenuManager.h"
#include "ModalPrompt.h"
#include "RGBController.h"
#include "ScrollLabel.h"
#include "StatusService.h"
#include "WasmFsApp.h"
#include "WebPortalApp.h"
#include "globals.h"

namespace AwakeMode {
namespace {
using namespace AwakePolicy;

auto& display = HAL::displayProxy();

constexpr const char* kNamespace = "upd";
// bootcfg one-shot: why dev mode turned itself off before the restart.
constexpr const char* kKeyEnded = "awk_end";
// A dev worker that ended by itself (not linked, no saved network) is
// started again after this long.
constexpr uint32_t kWorkerRetryMs = 30000;
// Consecutive failed check-ins before the bar says "not connected".
constexpr uint8_t kShowNotConnectedAfter = 3;
constexpr uint32_t kBreathStepMs = 50;

Setting current;
bool listenBoot = false;       // dev mode listens in this power cycle
bool ending = false;
uint32_t lastPressMs = 0;
uint32_t lastUseMs = 0;
uint32_t lowSinceMs = 0;       // 0 = the battery is not low

// Dev worker management.
bool workerStarted = false;
bool pausedCancel = false;     // cancelled for an app that needs the radio or the link
uint32_t workerStartMs = 0;
uint32_t seenDeliveries = 0;
// Listening beside a delivered app: decided when it opens, withdrawn by a
// check-in trough below the floor while it runs (AwakePolicy).
bool besideAppOk = true;
uint32_t besideAppSinceMs = 0;

enum class Shown : uint8_t { None, Listening, NotConnected, NotLinked, NoWifi };
Shown shown = Shown::None;
constexpr const char* kNotConnected = "Dev mode: not connected";
constexpr const char* kNotLinked = "Dev mode: link this Fidget first";
constexpr const char* kNoWifi = "Dev mode: no saved network";

bool ledOn = false;
uint32_t ledAt = 0;

#ifdef CF_TEST_CLI
uint32_t testIdleMs = 0;       // 0 = built-in
uint32_t testSafetyMs = 0;     // 0 = built-in
bool testBatteryLow = false;
#endif

uint32_t idleStopMs() {
#ifdef CF_TEST_CLI
    if (testIdleMs) return testIdleMs;
#endif
    return kIdleStopMs;
}

uint32_t safetyNetMs() {
#ifdef CF_TEST_CLI
    if (testSafetyMs) return testSafetyMs;
#endif
    return kSafetyNetMs;
}

const char* stopName(Stop s) { return s == Stop::UntilStopped ? "until" : "idle"; }
const char* modeKey(Mode m) {
    return m == Mode::Dev ? "dev" : m == Mode::StayAwake ? "stay" : "off";
}

// ---- storage ---------------------------------------------------------------------

// The legacy keys are never removed: an image that rolls back still reads
// its own `dev` key.
bool writeSetting(const Setting& s) {
    Preferences upd;
    if (!upd.begin(kNamespace, false)) return false;
    const bool ok = upd.putUChar(kKeyMode, (uint8_t)s.mode) != 0 &&
                    upd.putUChar(kKeyStop, (uint8_t)s.stop) != 0;
    upd.end();
    return ok;
}

Setting readSetting() {
    Stored st;
    Preferences upd;
    if (upd.begin(kNamespace, true)) {
        st.hasMode = upd.isKey(kKeyMode);
        st.mode = st.hasMode ? upd.getUChar(kKeyMode, 0) : 0;
        st.hasStop = upd.isKey(kKeyStop);
        st.stop = st.hasStop ? upd.getUChar(kKeyStop, 0) : 0;
        st.hasLegacyDev = upd.isKey(kLegacyKeyDev);
        st.legacyDev = st.hasLegacyDev ? upd.getUChar(kLegacyKeyDev, 0) : 0;
        st.hasLegacyIdle = upd.isKey(kLegacyKeyDevIdle);
        upd.end();
    }
    const Parsed p = parseStored(st);
    if (p.migrate) {
        const bool ok = writeSetting(p.setting);
        Serial.printf("[awake] migrated legacy=%u mode=%s stop=%s write=%s\n",
                      (unsigned)st.legacyDev, modeKey(p.setting.mode), stopName(p.setting.stop),
                      ok ? "ok" : "error");
    }
    return p.setting;
}

// ---- screen helpers ------------------------------------------------------------------

void drawMessage(const char* first, const char* second) {
    display.clear();
    display.setColor(WHITE);
    display.setFont(ArialMT_Plain_10);
    display.setTextAlignment(TEXT_ALIGN_CENTER);
    display.drawString(64, 20, first);
    if (second && second[0]) display.drawString(64, 34, second);
    display.display();
    display.setTextAlignment(TEXT_ALIGN_LEFT);
}

[[noreturn]] void restartNow(const char* message, const char* detail, bool bluetoothApp,
                             uint8_t endedWhy) {
    // Every restart out of a listening power cycle switches WiFi off first.
    CloudSync::cancelPending();
    Preferences boot;
    if (boot.begin("bootcfg", false)) {
        boot.putBool("skipanim", true);
        if (bluetoothApp) boot.putBool("bootmusic", true);
        if (endedWhy) boot.putUChar(kKeyEnded, endedWhy);
        boot.end();
    }
    ModalPrompt::instance().closeForTeardown();
    drawMessage(message, detail);
    Serial.flush();
    delay(endedWhy ? 2500 : 300);
    ESP.restart();
    for (;;) {}
}

// ---- visible state -----------------------------------------------------------------------

void postShown(Shown want) {
    StatusService& svc = StatusService::instance();
    if (want == shown) return;
    switch (shown) {
        case Shown::Listening:    svc.clear(StatusKind::Listening); break;
        case Shown::NotConnected: svc.clearEntry(StatusKind::Warning, kNotConnected); break;
        case Shown::NotLinked:    svc.clearEntry(StatusKind::Warning, kNotLinked); break;
        case Shown::NoWifi:       svc.clearEntry(StatusKind::Warning, kNoWifi); break;
        default: break;
    }
    const uint32_t now = millis();
    switch (want) {
        case Shown::Listening:
            svc.post(StatusKind::Listening, nullptr, StatusService::defaultPriority(StatusKind::Listening),
                     true, now);
            break;
        case Shown::NotConnected:
            svc.post(StatusKind::Warning, kNotConnected, StatusPriority::Normal, true, now);
            break;
        case Shown::NotLinked:
            svc.post(StatusKind::Warning, kNotLinked, StatusPriority::Normal, true, now);
            break;
        case Shown::NoWifi:
            svc.post(StatusKind::Warning, kNoWifi, StatusPriority::Normal, true, now);
            break;
        default: break;
    }
    Serial.printf("[awake] shown=%s\n",
                  want == Shown::Listening ? "listening" : want == Shown::NotConnected ? "not-connected" :
                  want == Shown::NotLinked ? "not-linked" : want == Shown::NoWifi ? "no-wifi" : "none");
    shown = want;
}

void updateMarkers() {
    StatusService& svc = StatusService::instance();
    const Marker m = marker(current, listenBoot && shown == Shown::Listening);
    svc.setAwakeMarker(m == Marker::Listening ? AwakeMarker::Listening :
                       m == Marker::Awake ? AwakeMarker::Awake : AwakeMarker::None);
    svc.setBluetoothNeedsRestart(bluetoothAppNeedsRestart(listenBoot && current.mode == Mode::Dev));
}

void updateStatus() {
    Shown want = Shown::None;
    if (CloudSync::devListening()) {
        const CloudSync::DevSnapshot snap = CloudSync::devSnapshot();
        want = snap.failures >= kShowNotConnectedAfter ? Shown::NotConnected : Shown::Listening;
    } else if (!CloudSync::busy() && workerStarted) {
        const CloudSync::Result& r = CloudSync::lastResult();
        if (r.reason == CloudSync::Reason::Dev) {
            if (strcmp(r.err, "not-linked") == 0 || strcmp(r.err, "link-mismatch") == 0)
                want = Shown::NotLinked;
            else if (strcmp(r.err, "no-wifi") == 0) want = Shown::NoWifi;
        }
    }
    if (want == shown) return;
    postShown(want);
    updateMarkers();
}

// Dev mode's indicator: the back LED breathes while the menu is in front.
// Any app owns the LEDs (its begin() clears them), and the menu gets the
// breathing back when the app ends.
void breathe(uint32_t now) {
    const bool show = AppManager::instance().activeApp() == APP_MENU && CloudSync::devListening();
    if (!show) {
        if (ledOn && AppManager::instance().activeApp() == APP_MENU) {
            HAL::setRgbLed(pixel_Back, 0, 0, 0, 0);
            HAL::showRgbLeds();
        }
        ledOn = false;
        return;
    }
    if (ledOn && now - ledAt < kBreathStepMs) return;
    ledAt = now;
    HAL::setRgbLed(pixel_Back, 0, 0, breathLevel(now), 0);
    HAL::showRgbLeds();
    ledOn = true;
}

// ---- dev mode listening --------------------------------------------------------------

bool pausesListening(AppIndex app) {
    // Listening continues only beside what was measured to leave the network
    // session its internal heap (AwakePolicy::listensDuring), and beside a
    // delivered (WASM) app while the heap allows (its interpreter stack is in
    // PSRAM). Everything else pauses it until it ends: the radio apps and the
    // Link screen need the radio or the worker, Voice Notes dips to 13 KB. A
    // send made meanwhile arrives at the menu and reopens the app it was for.
    if (app == APP_WASM_HOST) return !besideAppOk;
    return app < 0 || app >= APP_COUNT || !listensDuring(appIds[app]);
}

// While a delivered app runs beside listening, a check-in whose trough fell
// below the floor pauses listening for the rest of the app.
void watchBesideApp(AppIndex active) {
    if (active != APP_WASM_HOST || !besideAppOk || !CloudSync::devListening()) return;
    const CloudSync::DevSnapshot snap = CloudSync::devSnapshot();
    if ((int32_t)(snap.lastPollMs - besideAppSinceMs) <= 0 || !troughBelowFloor(snap.last.heapMin)) return;
    besideAppOk = false;
    Serial.printf("[awake] pause=heap app=%d trough=%u\n", (int)active, (unsigned)snap.last.heapMin);
}

void manageWorker(uint32_t now) {
    const AppIndex active = AppManager::instance().activeApp();
    watchBesideApp(active);
    if (pausesListening(active)) {
        if (CloudSync::devListening()) {
            CloudSync::requestCancel();
            pausedCancel = true;
        }
        return;
    }
    if (CloudSync::busy()) return;
    if (workerStarted && !pausedCancel && now - workerStartMs < kWorkerRetryMs) return;
    if (!CloudSync::runSession(CloudSync::Reason::Dev)) return;
    Serial.printf("[awake] listen=start free_int=%u largest_int=%u\n",
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    workerStarted = true;
    pausedCancel = false;
    workerStartMs = now;
    seenDeliveries = 0;
}

// A delivery that carries a new version of the app that is running, or was
// the last one run while the menu is in front, starts it again from the
// newly committed file.
void relaunchDelivered() {
    std::string id, path;
    if (!WasmFsApp::lastLaunch(id, path)) return;
    LoadoutManifest::Loadout lo;
    bool have = false;
    {
        LoadoutStore::Guard guard;
        have = loadLoadoutManifest(lo, nullptr);
    }
    if (!have) return;
    const LoadoutManifest::LoadoutEntry* hit = nullptr;
    for (const auto& e : lo.entries) {
        if (e.id == id && !e.blobPath.empty() && e.format != "builtin") { hit = &e; break; }
    }
    if (!hit || hit->blobPath == path) return;
    const AppIndex active = AppManager::instance().activeApp();
    const bool running = active == APP_WASM_HOST;
    const bool atMenu = active == APP_MENU && !ModalPrompt::instance().isOpen();
    if (!running && !atMenu) {
        Serial.printf("[dev] relaunch=skipped id=%s active=%d\n", id.c_str(), (int)active);
        return;
    }
    const int abi = LoadoutManifest::parseAbiVersion(hit->abi);
    WasmFsApp::setPending(hit->blobPath.c_str(), hit->name.empty() ? hit->id.c_str() : hit->name.c_str(),
                          abi, hit->id.c_str());
    Serial.printf("[dev] relaunch id=%s path=%s from=%s at_ms=%lu\n", id.c_str(),
                  hit->blobPath.c_str(), running ? "app" : "menu", (unsigned long)millis());
    if (running) AppManager::instance().relaunchActive();
    else AppManager::instance().switchToApp(APP_WASM_HOST);
}

void handleDeliveries(uint32_t now) {
    if (!workerStarted) return;
    const CloudSync::DevSnapshot snap = CloudSync::devSnapshot();
    if (snap.deliveries == seenDeliveries) return;
    seenDeliveries = snap.deliveries;
    lastUseMs = now;   // a sent app is use
    StatusService::instance().post(StatusKind::Info, "App changes applied",
                                   StatusPriority::Normal, false, now);
    relaunchDelivered();
}

bool batteryLowNow() {
#ifdef CF_TEST_CLI
    if (testBatteryLow) return true;
#endif
    const int32_t mv = batteryVoltage >= 2.0f && batteryVoltage <= 4.6f
        ? (int32_t)(batteryVoltage * 1000.0f + 0.5f) : -1;
    const int32_t soc = batteryVoltagePercentage >= 0.0f && batteryVoltagePercentage <= 110.0f
        ? (int32_t)batteryVoltagePercentage : -1;
    return batteryLow(mv, soc, batteryChangeRate > 0.0f);
}

void endMode(End e) {
    const Mode was = current.mode;
    Serial.printf("[awake] end=%s mode=%s since_use_ms=%lu since_press_ms=%lu\n", endName(e),
                  modeKey(was), (unsigned long)(millis() - lastUseMs),
                  (unsigned long)(millis() - lastPressMs));
    const Setting off;
    // Retried once: a restart with the mode still stored would come back
    // in the same mode.
    const bool saved = writeSetting(off) || writeSetting(off);
    current = off;
    if (!saved) {
        // Stay up, stop listening and keeping awake, and say so.
        Serial.println("[awake] end write=error restart=no");
        if (listenBoot) CloudSync::cancelPending();
        listenBoot = false;
        postShown(Shown::None);
        char text[StatusEntry::kMaxText];
        snprintf(text, sizeof(text), "Could not turn %s off. Restart to try again.",
                 was == Mode::Dev ? "Dev mode" : "Stay awake");
        StatusService::instance().post(StatusKind::Warning, text, StatusPriority::High, true, millis());
        updateMarkers();
        millis_APP_LASTINTERACTION = millis_NOW;
        return;
    }
    if (was == Mode::Dev && listenBoot) {
        // Bluetooth gets its memory back only across a restart.
        ending = true;
        ModalPrompt::instance().closeForTeardown();
        restartNow(endTitle(was), endReason(e), false, (uint8_t)e);
    }
    char text[StatusEntry::kMaxText];
    const char* why = endReason(e);
    snprintf(text, sizeof(text), "%s: %c%s", endTitle(was), tolower((unsigned char)why[0]), why + 1);
    StatusService::instance().post(StatusKind::Info, text, StatusPriority::Normal, true, millis());
    postShown(Shown::None);
    updateMarkers();
    lowSinceMs = 0;
    // Normal sleep resumes from now.
    millis_APP_LASTINTERACTION = millis_NOW;
}

// A crash or watchdog loop in an awake mode ends the mode: the count of
// abnormal resets in a row lives in memory that survives a reset (not a
// power cycle). Only this counter survives; the use / press / low-battery
// clocks restart with every start.
constexpr uint32_t kLoopMagic = 0x41574B4Cu;
RTC_NOINIT_ATTR uint32_t rtcLoopMagic;
RTC_NOINIT_ATTR uint8_t rtcLoopCount;

void endRestartLoop() {
    const esp_reset_reason_t why = esp_reset_reason();
    const bool abnormal = why == ESP_RST_PANIC || why == ESP_RST_INT_WDT ||
                          why == ESP_RST_TASK_WDT || why == ESP_RST_WDT ||
                          why == ESP_RST_BROWNOUT;
    const uint8_t previous = rtcLoopMagic == kLoopMagic ? rtcLoopCount : 0;
    const uint8_t count = nextLoopCount(previous, abnormal, keepsAwake(current));
    rtcLoopMagic = kLoopMagic;
    rtcLoopCount = count;
    if (count) Serial.printf("[awake] abnormal-reset count=%u reason=%d\n", (unsigned)count, (int)why);
    if (!loopEndsMode(count)) return;
    const Mode was = current.mode;
    const Setting off;
    const bool saved = writeSetting(off) || writeSetting(off);
    Serial.printf("[awake] end=%s mode=%s write=%s\n", endName(End::RestartLoop), modeKey(was),
                  saved ? "ok" : "error");
    // Off for this start whatever the write did: nothing below starts
    // listening or releases Bluetooth.
    current = off;
    rtcLoopCount = 0;
    char text[StatusEntry::kMaxText];
    const char* reason = endReason(End::RestartLoop);
    snprintf(text, sizeof(text), "%s: %c%s", endTitle(was), tolower((unsigned char)reason[0]), reason + 1);
    StatusService::instance().post(StatusKind::Info, text, StatusPriority::Normal, true, millis());
}

#ifdef CF_TEST_CLI
// Bench: panic this many times once dev mode starts listening, before its
// first check-in could count as a clean start (`awake crash`).
uint8_t testCrashes = 0;
void loadTestCrash() {
    Preferences test;
    if (!test.begin("cftest", true)) return;
    testCrashes = test.getUChar("awk_crash", 0);
    test.end();
}
void maybeTestCrash() {
    if (!testCrashes || !CloudSync::devListening()) return;
    Preferences test;
    if (test.begin("cftest", false)) {
        test.putUChar("awk_crash", testCrashes - 1);
        test.end();
    }
    Serial.printf("[awake] test-crash left=%u\n", (unsigned)(testCrashes - 1));
    Serial.flush();
    abort();
}
#endif

// ---- Bluetooth apps ------------------------------------------------------------------------

void onBluetoothAnswer(int result) {
    Serial.printf("[awake] bluetooth=%s\n", result == kBluetoothRestart ? "restart" : "cancel");
    if (result == kBluetoothRestart) restartNow("Restarting...", "", true, 0);
}

// ---- the Awake & dev mode screen ------------------------------------------------------------

constexpr int kScreenW = 128;
constexpr int kTitleH = 14;
int screenChoice = 0;
bool screenEnterArmed = false;
int labelsFor = -1;
ScrollLabel stopLabel;
ScrollLabel descLabel;

void applyChoice(const Setting& chosen, const char* from) {
    const Apply a = applyEffect(current, chosen);
    Serial.printf("[awake] set mode=%s stop=%s effect=%s from=%s\n", modeKey(chosen.mode),
                  stopName(chosen.stop),
                  a == Apply::Nothing ? "none" : a == Apply::Save ? "save" : "restart", from);
    if (a == Apply::Nothing) return;
    if (!writeSetting(chosen)) {
        Serial.println("[awake] set write=error");
        return;
    }
    if (a == Apply::SaveAndRestart) restartNow("Restarting...", "", false, 0);
    current = chosen;
    const uint32_t now = millis();
    lastUseMs = now;
    lastPressMs = now;
    lowSinceMs = 0;
    if (!keepsAwake(current)) {
        postShown(Shown::None);
        millis_APP_LASTINTERACTION = millis_NOW;
    }
    updateMarkers();
}

void onScreenLeft(const ButtonEvent& event) {
    if (event.eventType == ButtonEvent_Pressed) screenChoice = stepChoice(screenChoice, -1);
}
void onScreenRight(const ButtonEvent& event) {
    if (event.eventType == ButtonEvent_Pressed) screenChoice = stepChoice(screenChoice, 1);
}
void onScreenEnter(const ButtonEvent& event) {
    // Only a press that started on this screen applies (on its release).
    if (event.eventType == ButtonEvent_Pressed) screenEnterArmed = true;
    if (event.eventType != ButtonEvent_Released || !screenEnterArmed) return;
    screenEnterArmed = false;
    applyChoice(choice(screenChoice).setting, "screen");
}
void onScreenBack(const ButtonEvent& event) {
    if (event.eventType == ButtonEvent_Released) MenuManager::instance().returnToMenu();
}

} // namespace

// ---- lifecycle ------------------------------------------------------------------------------

void beginBoot(bool bluetoothAppBoot) {
    current = readSetting();
    endRestartLoop();
    listenBoot = listensThisBoot(current, bluetoothAppBoot);
    lastPressMs = lastUseMs = millis();
#ifdef CF_TEST_CLI
    loadTestCrash();
#endif
    const size_t freeBefore = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    bool released = false;
    if (releasesBluetoothAtBoot(current, bluetoothAppBoot)) {
        // As the setup portal does: this power cycle never starts
        // Bluetooth, so its memory goes to the network.
        WebPortalApp::releaseBluetoothForNetwork();
        released = true;
    }
    uint8_t ended = 0;
    Preferences boot;
    if (boot.begin("bootcfg", false)) {
        if (boot.isKey(kKeyEnded)) {
            ended = boot.getUChar(kKeyEnded, 0);
            boot.remove(kKeyEnded);
        }
        boot.end();
    }
    if (ended) {
        // Dev mode turned itself off before this restart: say why.
        char text[StatusEntry::kMaxText];
        const char* why = endReason((End)ended);
        snprintf(text, sizeof(text), "%s: %c%s", endTitle(Mode::Dev),
                 tolower((unsigned char)why[0]), why + 1);
        StatusService::instance().post(StatusKind::Info, text, StatusPriority::Normal, true, millis());
    }
    Serial.printf("[awake] boot mode=%s stop=%s listen=%d bt_released=%d free_int=%u "
                  "free_int_before=%u largest_int=%u ended=%s\n",
                  modeKey(current.mode), stopName(current.stop), listenBoot ? 1 : 0, released ? 1 : 0,
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)freeBefore,
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                  ended ? endName((End)ended) : "-");
    updateMarkers();
}

void loop() {
    // A clean stretch clears the restart-loop count (a crash much later is
    // not part of a loop).
    if (rtcLoopCount &&
        loopCountClears(millis(), listenBoot && CloudSync::devSnapshot().polls > 0 &&
                                  CloudSync::devSnapshot().failures == 0)) {
        Serial.println("[awake] abnormal-reset count=0 reason=clean");
        rtcLoopCount = 0;
    }
    if (ending || !keepsAwake(current)) return;
    const uint32_t now = millis();
    // Idle sleep is off in both awake modes.
    millis_APP_LASTINTERACTION = millis_NOW;
    if (!batteryLowNow()) lowSinceMs = 0;
    else if (!lowSinceMs) lowSinceMs = now ? now : 1;
    Activity a;
    a.sinceUseMs = now - lastUseMs;
    a.sincePressMs = now - lastPressMs;
    a.lowBatteryForMs = lowSinceMs ? now - lowSinceMs : 0;
    const End e = checkEnd(current, a, idleStopMs(), safetyNetMs());
    if (e != End::None) {
        endMode(e);
        return;
    }
    if (!listenBoot) return;
#ifdef CF_TEST_CLI
    maybeTestCrash();
#endif
    manageWorker(now);
    handleDeliveries(now);
    updateStatus();
    breathe(now);
}

void noteButton() {
    lastPressMs = lastUseMs = millis();
}

bool interceptSwitch(AppIndex newApp) {
    if (newApp == APP_WASM_HOST) {
        // Decided now, with listening (if running) at its steady level.
        const uint32_t freeInt = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        besideAppOk = listensBesideDeliveredApp(freeInt);
        besideAppSinceMs = millis();
        if (listenBoot)
            Serial.printf("[awake] beside-app=%s free_int=%u listening=%d\n", besideAppOk ? "listen" : "pause",
                          (unsigned)freeInt, CloudSync::devListening() ? 1 : 0);
    }
    if (newApp != APP_MUSIC_PLAYER || !bluetoothAppNeedsRestart(listenBoot)) {
        if (CloudSync::devListening() && pausesListening(newApp)) {
            // Listening stops (WiFi off) BEFORE the app begins, so the app
            // never starts beside a network session (bench: Voice Notes
            // started during a check-in dipped to 17 KB). It resumes when
            // the app ends. Stopping can take seconds: say so first.
            drawMessage("Pausing dev mode...", "");
            pausedCancel = true;
            if (!CloudSync::cancelPending()) {
                // Still inside a long network call: never open the app
                // beside it. A delivered app opens in a fresh start; a
                // built-in one waits, and the menu stays.
                Serial.printf("[awake] pause=stuck app=%d\n", (int)newApp);
                if (newApp == APP_WASM_HOST) AppManager::instance().restartIntoPendingApp();
                if (AppManager::instance().activeApp() == APP_MENU) MenuManager::instance().begin();
                StatusService::instance().post(StatusKind::Info, "Dev mode is busy. Try again in a moment.",
                                               StatusPriority::Normal, false, millis());
                return true;
            }
        }
        return false;
    }
    // From the menu, which has already let go of its buttons: take them
    // back first, so the prompt hands them to the menu when it closes.
    if (AppManager::instance().activeApp() == APP_MENU) MenuManager::instance().begin();
    static char title[128];
    bluetoothPromptTitle(title, sizeof(title), "Music");
    if (ModalPrompt::instance().open(title, kBluetoothOptions, 2, onBluetoothAnswer)) {
        Serial.println("[awake] bluetooth=ask app=music");
        return true;
    }
    // No prompt here (not the menu): the restart is what it would ask for.
    Serial.println("[awake] bluetooth=restart app=music prompt=no");
    restartNow("Restarting...", "", true, 0);
}

Setting setting() { return current; }

bool listening() { return listenBoot; }

// ---- Settings > Awake & dev mode ----------------------------------------------------------

void screenBegin() {
    auto& buttons = HAL::buttonManager();
    buttons.registerCallback(button_LeftIndex, onScreenLeft);
    buttons.registerCallback(button_RightIndex, onScreenRight);
    buttons.registerCallback(button_EnterIndex, onScreenEnter);
    buttons.registerCallback(button_SelectIndex, onScreenBack);
    setColorsOff();
    screenChoice = choiceIndex(current);
    screenEnterArmed = false;
    labelsFor = -1;
}

void screenEnd() {
    auto& buttons = HAL::buttonManager();
    buttons.unregisterCallback(button_LeftIndex);
    buttons.unregisterCallback(button_RightIndex);
    buttons.unregisterCallback(button_EnterIndex);
    buttons.unregisterCallback(button_SelectIndex);
    setColorsOff();
}

void screenUpdate() {
    const Choice& c = choice(screenChoice);
    const uint32_t now = millis();
    if (labelsFor != screenChoice) {
        labelsFor = screenChoice;
        stopLabel.restart(now);
        descLabel.restart(now);
    }
    display.clear();
    display.setFont(ArialMT_Plain_10);
    display.setColor(WHITE);
    display.fillRect(0, 0, kScreenW, kTitleH);
    display.setColor(BLACK);
    display.setTextAlignment(TEXT_ALIGN_CENTER);
    display.drawString(kScreenW / 2, 1, "Awake & dev mode");
    display.setColor(WHITE);
    // < Mode >
    display.setTextAlignment(TEXT_ALIGN_LEFT);
    display.drawString(2, 15, "<");
    display.setTextAlignment(TEXT_ALIGN_RIGHT);
    display.drawString(kScreenW - 2, 15, ">");
    display.setTextAlignment(TEXT_ALIGN_CENTER);
    display.drawString(kScreenW / 2, 15, c.name);
    if (c.stopLine[0]) stopLabel.draw(12, 27, kScreenW - 24, c.stopLine, true);
    descLabel.draw(0, 39, kScreenW, c.description, false);
    // The last row: whether this is the mode in use, or how to choose it.
    display.setTextAlignment(TEXT_ALIGN_CENTER);
    const bool inUse = sameSetting(c.setting, current) ||
                       (c.setting.mode == Mode::Off && current.mode == Mode::Off);
    display.drawString(kScreenW / 2, 51, inUse ? "In use" : "Enter: use this");
    display.setTextAlignment(TEXT_ALIGN_LEFT);
    display.display();
}

// ---- bench verbs -------------------------------------------------------------------------------

#ifdef CF_TEST_CLI
void cliCommand(const char* args) {
    const uint32_t now = millis();
    if (!args || !args[0]) {
        const CloudSync::DevSnapshot snap = CloudSync::devSnapshot();
        Serial.printf("[cmd] awake.mode=%s stop=%s listen=%d worker=%d polls=%u deliveries=%u "
                      "failures=%u connected=%d since_use_ms=%lu since_press_ms=%lu low_ms=%lu "
                      "idle_ms=%lu safety_ms=%lu heap_min=%u largest_min=%u free_int=%u "
                      "largest_int=%u bt_restart=%d\n",
                      modeKey(current.mode), stopName(current.stop), listenBoot ? 1 : 0,
                      CloudSync::devListening() ? 1 : 0, (unsigned)snap.polls,
                      (unsigned)snap.deliveries, (unsigned)snap.failures, snap.connected ? 1 : 0,
                      (unsigned long)(now - lastUseMs), (unsigned long)(now - lastPressMs),
                      (unsigned long)(lowSinceMs ? now - lowSinceMs : 0),
                      (unsigned long)idleStopMs(), (unsigned long)safetyNetMs(),
                      (unsigned)snap.heapMin, (unsigned)snap.largestMin,
                      (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                      StatusService::instance().bluetoothNeedsRestart() ? 1 : 0);
        return;
    }
    char verb[16] = {0};
    char a1[160] = {0};
    char a2[16] = {0};
    sscanf(args, "%15s %159s %15s", verb, a1, a2);
    if (strcmp(verb, "set") == 0) {
        Setting s;
        if (strcmp(a1, "stay") == 0) s.mode = Mode::StayAwake;
        else if (strcmp(a1, "dev") == 0) s.mode = Mode::Dev;
        else if (strcmp(a1, "off") != 0) { Serial.println("[cmd] awake.set=usage"); return; }
        s.stop = strcmp(a2, "until") == 0 ? Stop::UntilStopped : Stop::AfterIdle;
        Serial.printf("[cmd] awake.set=%s stop=%s\n", modeKey(s.mode), stopName(s.stop));
        applyChoice(s, "cli");
        return;
    }
    if (strcmp(verb, "legacy") == 0) {
        // Writes the earlier Settings > Updates key only (migration bench).
        Preferences upd;
        const bool ok = upd.begin(kNamespace, false) &&
                        upd.putUChar(kLegacyKeyDev, (uint8_t)atoi(a1)) != 0;
        if (ok) {
            upd.putUInt(kLegacyKeyDevIdle, 60);
            upd.remove(kKeyMode);
            upd.remove(kKeyStop);
        }
        upd.end();
        Serial.printf("[cmd] awake.legacy=%s\n", ok ? a1 : "error");
        return;
    }
    if (strcmp(verb, "idle") == 0) {
        testIdleMs = (uint32_t)strtoul(a1, nullptr, 10) * 1000u;
        Serial.printf("[cmd] awake.idle_ms=%lu\n", (unsigned long)idleStopMs());
        return;
    }
    if (strcmp(verb, "safety") == 0) {
        testSafetyMs = (uint32_t)strtoul(a1, nullptr, 10) * 1000u;
        Serial.printf("[cmd] awake.safety_ms=%lu\n", (unsigned long)safetyNetMs());
        return;
    }
    if (strcmp(verb, "battery") == 0) {
        testBatteryLow = strcmp(a1, "low") == 0;
        Serial.printf("[cmd] awake.battery=%s\n", testBatteryLow ? "low" : "real");
        return;
    }
    if (strcmp(verb, "tls") == 0) {
        const bool ok = CloudSync::setDevTlsProbe(strcmp(a1, "off") == 0 ? "" : a1);
        Serial.printf("[cmd] awake.tls=%s\n", ok ? (strcmp(a1, "off") == 0 ? "off" : "on") : "error");
        return;
    }
    if (strcmp(verb, "stall") == 0) {
        Serial.printf("[cmd] awake.stall=%s\n", CloudSync::setDevStallMs((uint32_t)atoi(a1)) ? a1 : "error");
        return;
    }
    if (strcmp(verb, "crash") == 0) {
        Preferences test;
        const uint8_t n = (uint8_t)atoi(a1);
        const bool ok = test.begin("cftest", false) && test.putUChar("awk_crash", n) != 0;
        test.end();
        Serial.printf("[cmd] awake.crash=%s\n", ok ? a1 : "error");
        return;
    }
    if (strcmp(verb, "poll") == 0) {
        CloudSync::devPollNow();
        Serial.println("[cmd] awake.poll=now");
        return;
    }
    Serial.println("[cmd] awake.usage=awake [set off|stay|dev [until|idle] | idle <s> | "
                   "safety <s> | battery low|real | tls <https-url>|off | poll | legacy <0|1|2> | crash <n> | stall <ms>]");
}
#endif

} // namespace AwakeMode
