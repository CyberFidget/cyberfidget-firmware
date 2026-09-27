// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#include "FactoryReset.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_bt.h>
#include <esp_bt_main.h>
#include <esp_sleep.h>
#include <esp_system.h>
#include <nvs.h>
#include <nvs_flash.h>

#include "CloudSync.h"
#include "AppManager.h"
#include "AppDefs.h"
#include "DisplayProxy.h"
#include "FactoryResetPolicy.h"
#include "HAL.h"
#include "LoadoutStore.h"
#include "RGBController.h"
#include "MenuManager.h"
#include "SerialCli.h"
#include "UpdateSession.h"
#include "WebPortalApp.h"
#include "globals.h"

namespace FactoryReset {
namespace {

auto& display = HAL::displayProxy();
FactoryResetPolicy::Hold hold;
const char* message = nullptr;
uint32_t messageAt = 0;
uint32_t messageMs = 1400;
bool erasing = false;
#ifdef CF_TEST_CLI
bool holdAfterFormat = false;  // bench: time to cut power between the two erases
bool faultMark = false;        // bench: the mark write "fails"
// Bench: the apps erase "fails", here and on the start-up that follows a
// software restart (so the partial-reset path can be seen end to end).
RTC_NOINIT_ATTR uint32_t faultFsMagic;
constexpr uint32_t kFaultFsMagic = 0x46464653;  // "FFFS"
#endif

// "Reset in progress" mark. It lives in the settings store itself, so the
// settings erase that ends a reset also clears it; a power cut before that
// leaves it for the next start-up to finish the job.
// Start-ups that tried to finish a reset since power-on. A crash inside the
// finish would otherwise repeat on every start: the third try skips the
// apps erase, still erases the settings (which clears the mark) and records
// the reset as partial.
RTC_NOINIT_ATTR uint32_t finishTries;
RTC_NOINIT_ATTR uint32_t finishTriesMagic;
constexpr uint32_t kFinishTriesMagic = 0x46524553;  // "FRES"

constexpr const char* kMarkNs = "freset";
constexpr const char* kMarkKey = "busy";
// Written into the fresh settings store when a start-up finish could not
// erase the apps; the next start-up tells the owner once, then clears it.
constexpr const char* kPartialKey = "partial";

bool writeU8(const char* key, uint8_t value) {
    nvs_handle_t h;
    if (nvs_open(kMarkNs, NVS_READWRITE, &h) != ESP_OK) return false;
    const bool ok = nvs_set_u8(h, key, value) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

bool writeMark() {
#ifdef CF_TEST_CLI
    if (faultMark) {
        faultMark = false;
        Serial.println("[reset] factory=fault mark-write");
        return false;
    }
#endif
    return writeU8(kMarkKey, 1);
}

// "Not found" (namespace or key) means clear; any other failure is an error.
FactoryResetPolicy::MarkRead readU8(const char* key, esp_err_t* errOut) {
    using FactoryResetPolicy::MarkRead;
    nvs_handle_t h;
    esp_err_t err = nvs_open(kMarkNs, NVS_READONLY, &h);
    if (err == ESP_OK) {
        uint8_t value = 0;
        err = nvs_get_u8(h, key, &value);
        nvs_close(h);
        if (err == ESP_OK) {
            *errOut = ESP_OK;
            return value ? MarkRead::Set : MarkRead::Clear;
        }
    }
    *errOut = err;
    return err == ESP_ERR_NVS_NOT_FOUND ? MarkRead::Clear : MarkRead::Error;
}

// One retry on a read error; the result says what the second read saw.
FactoryResetPolicy::MarkRead readMark() {
    esp_err_t err = ESP_OK;
    auto mark = readU8(kMarkKey, &err);
    if (mark != FactoryResetPolicy::MarkRead::Error) return mark;
    Serial.printf("[reset] factory=mark-read-error err=0x%x retry=1\n", (unsigned)err);
    delay(20);
    mark = readU8(kMarkKey, &err);
    if (mark == FactoryResetPolicy::MarkRead::Error)
        Serial.printf("[reset] factory=mark-unreadable err=0x%x\n", (unsigned)err);
    return mark;
}

FactoryResetPolicy::Refusal refusal() {
    return FactoryResetPolicy::refusal(UpdateSession::imagePending(),
                                       UpdateSession::sessionRequestArmed());
}

const char* refusalText(FactoryResetPolicy::Refusal reason) {
    switch (reason) {
        case FactoryResetPolicy::Refusal::PendingImage: return "Finish update first";
        case FactoryResetPolicy::Refusal::ArmedSession: return "Update is starting";
        default: return nullptr;
    }
}

// Centred; '\n' starts a new line (up to four).
void showMessage(const char* text) {
    char lines[4][32];
    int count = 0;
    size_t len = 0;
    for (const char* p = text;; ++p) {
        if (*p == '\n' || *p == '\0') {
            lines[count++][len] = '\0';
            len = 0;
            if (*p == '\0' || count == 4) break;
        } else if (len < sizeof(lines[0]) - 1) {
            lines[count][len++] = *p;
        }
    }
    display.clear();
    display.setFont(ArialMT_Plain_10);
    display.setColor(WHITE);
    display.setTextAlignment(TEXT_ALIGN_CENTER);
    const int top = 26 - (count - 1) * 6;
    for (int i = 0; i < count; ++i) display.drawString(64, top + i * 12, lines[i]);
    display.display();
    display.setTextAlignment(TEXT_ALIGN_LEFT);
}

// logReason: a short token for the log when the screen text is long.
void refuseOrFail(const char* text, const char* logReason = nullptr,
                  uint32_t showMs = 1400) {
    Serial.printf("[reset] factory=refused reason=%s\n", logReason ? logReason : text);
    message = text;
    messageAt = millis();
    messageMs = showMs;
    hold.back();
    showMessage(text);
}

// All handles and workers must be gone before the filesystem is formatted.
// The erase uses IDF's default NVS-partition API after deinitialization;
// neither call addresses otadata, an app slot or the SD card.
bool eraseAndRestart() {
    erasing = true;
    showMessage("Erasing...");
    SerialCli::instance().closeStorageForFactoryReset();
    if (CloudSync::busy() && (!CloudSync::cancelPending() || CloudSync::busy())) {
        erasing = false;
        refuseOrFail("Check is still busy");
        return false;
    }
    WiFi.disconnect(false);
    if (!WiFi.mode(WIFI_OFF) || WiFi.getMode() != WIFI_OFF) {
        erasing = false;
        refuseOrFail("WiFi did not stop");
        return false;
    }
    WebPortalApp::releaseBluetoothForNetwork();
    // Only a RUNNING stack blocks the erase; after the release (or the boot-time
    // memory release) a layer may sit initialized-but-off, which is safe.
    if (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_ENABLED ||
        esp_bluedroid_get_status() == ESP_BLUEDROID_STATUS_ENABLED) {
        Serial.printf("[reset] factory=refused bt_ctrl=%d bluedroid=%d\n",
                      (int)esp_bt_controller_get_status(), (int)esp_bluedroid_get_status());
        erasing = false;
        refuseOrFail("Bluetooth is busy");
        return false;
    }
    // From here on the reset must finish, if need be on the next start-up,
    // so it only goes ahead once the mark is written and reads back.
    const bool written = writeMark();
    esp_err_t readErr = ESP_OK;
    const auto readBack = written ? readU8(kMarkKey, &readErr)
                                  : FactoryResetPolicy::MarkRead::Error;
    if (!FactoryResetPolicy::markConfirmed(written, readBack)) {
        // A write that failed at the commit may still have left the mark;
        // take it back so a later start-up does not erase anything.
        nvs_handle_t h;
        if (nvs_open(kMarkNs, NVS_READWRITE, &h) == ESP_OK) {
            nvs_erase_key(h, kMarkKey);
            nvs_commit(h);
            nvs_close(h);
        }
        erasing = false;
        refuseOrFail("Could not start\nthe reset.\nNothing was erased.",
                     written ? "mark-readback" : "mark-write", 3000);
        return false;
    }
    Serial.println("[reset] factory=marked");
    finishTriesMagic = kFinishTriesMagic;
    finishTries = 0;
    bool formatted = false;
#ifdef CF_TEST_CLI
    if (faultFsMagic == kFaultFsMagic) Serial.println("[reset] factory=fault fs");
    else
#endif
    formatted = LoadoutStore::formatForFactoryReset();
    if (!formatted) {
        // The mark stays: the apps may be half erased, so the next start-up
        // finishes the reset rather than leave that state.
        erasing = false;
        refuseOrFail("Could not erase apps");
        return false;
    }
#ifdef CF_TEST_CLI
    if (holdAfterFormat) {
        Serial.println("[reset] factory=formatted hold_ms=10000");
        Serial.flush();
        delay(10000);
    }
#endif
    const esp_err_t deinit = nvs_flash_deinit();
    if (deinit != ESP_OK && deinit != ESP_ERR_NVS_NOT_INITIALIZED) {
        erasing = false;
        refuseOrFail("Could not stop settings");
        return false;
    }
    if (nvs_flash_erase() != ESP_OK) {
        Serial.println("[reset] factory=error nvs");
        Serial.flush();
        esp_restart(); // A partial NVS erase must recover on a fresh boot.
        for (;;) {}
    }
    Serial.println("[reset] factory=done");
    Serial.flush();
    esp_restart();
    for (;;) {}
}

void onEnter(const ButtonEvent& event) {
    if (message || erasing) return;
    if (event.eventType == ButtonEvent_Pressed) hold.press(millis());
    else if (event.eventType == ButtonEvent_Released) hold.release();
}

void onBack(const ButtonEvent& event) {
    if (erasing) return;
    hold.back();
    if (event.eventType == ButtonEvent_Released) MenuManager::instance().returnToMenu();
}

} // namespace

// A previous start-up finished a reset without erasing the apps: say so
// once. A timer wake has no screen; the notice waits for a normal start.
void showPartialNotice() {
    esp_err_t err = ESP_OK;
    if (readU8(kPartialKey, &err) != FactoryResetPolicy::MarkRead::Set) return;
    if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER) return;
    Serial.println("[reset] factory=partial");
    showMessage("The reset could not\nerase apps. Run Reset\nto factory again.");
    nvs_handle_t h;
    if (nvs_open(kMarkNs, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, kPartialKey);
        nvs_commit(h);
        nvs_close(h);
    }
    delay(6000);
}

void finishIfInterrupted() {
    using FactoryResetPolicy::BootStep;
#ifdef CF_TEST_CLI
    if (esp_reset_reason() == ESP_RST_POWERON) faultFsMagic = 0;
#endif
    const auto step = FactoryResetPolicy::bootStep(readMark(), UpdateSession::imagePending());
    if (step == BootStep::Normal) {
        showPartialNotice();
        return;
    }
    if (step == BootStep::Unreadable) {
        // Logged by readMark; start normally, the mark (if any) stays.
        Serial.println("[reset] factory=boot-normal mark=unreadable");
        return;
    }
    if (step == BootStep::Wait) {
        Serial.println("[reset] factory=interrupted wait=pending-image");
        return;
    }
    Serial.println("[reset] factory=finishing");
    // A timer wake has not started the screen; the restart below brings it up.
    if (esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_TIMER) showMessage("Finishing reset...");
    if (esp_reset_reason() == ESP_RST_POWERON || finishTriesMagic != kFinishTriesMagic) {
        finishTriesMagic = kFinishTriesMagic;
        finishTries = 0;
    }
    ++finishTries;
    // Nothing has mounted the app storage or started a radio yet.
    bool formatted = false;
    if (!FactoryResetPolicy::finishFormats(finishTries)) {
        Serial.println("[reset] factory=skip-fs retries");
    }
#ifdef CF_TEST_CLI
    else if (faultFsMagic == kFaultFsMagic) {
        faultFsMagic = 0;  // one start-up only
        Serial.println("[reset] factory=fault fs");
    }
#endif
    else {
        formatted = LoadoutStore::formatForFactoryReset();
        if (!formatted) Serial.println("[reset] factory=error fs");
    }
    nvs_flash_deinit();
    if (nvs_flash_erase() != ESP_OK) {
        // Start normally and try again next time rather than restart in a loop.
        Serial.println("[reset] factory=error nvs");
        nvs_flash_init();
        return;
    }
    if (FactoryResetPolicy::finishResult(formatted) == FactoryResetPolicy::FinishResult::Partial) {
        // The settings are gone (the mark with them, so no loop), but the
        // apps may not be: never report that as a finished reset.
        const bool saved = nvs_flash_init() == ESP_OK && writeU8(kPartialKey, 1);
        Serial.printf("[reset] factory=partial saved=%d\n", saved ? 1 : 0);
    } else {
        Serial.println("[reset] factory=done");
    }
    Serial.flush();
    esp_restart();
    for (;;) {}
}

void begin() {
    hold.back();
    erasing = false;
    message = nullptr;
    const auto reason = refusal();
    if (reason != FactoryResetPolicy::Refusal::None)
        refuseOrFail(refusalText(reason));
    auto& buttons = HAL::buttonManager();
    buttons.registerCallback(button_EnterIndex, onEnter);
    buttons.registerCallback(button_SelectIndex, onBack);
    setColorsOff();
}

void end() {
    auto& buttons = HAL::buttonManager();
    buttons.unregisterCallback(button_EnterIndex);
    buttons.unregisterCallback(button_SelectIndex);
    hold.back();
    setColorsOff();
}

void update() {
    if (message) {
        showMessage(message);
        if (static_cast<uint32_t>(millis() - messageAt) >= messageMs)
            MenuManager::instance().returnToMenu();
        return;
    }
    const uint32_t now = millis();
    if (hold.tick(now)) {
        const auto reason = refusal();
        if (reason != FactoryResetPolicy::Refusal::None) refuseOrFail(refusalText(reason));
        else eraseAndRestart();
        return;
    }
    display.clear();
    display.setFont(ArialMT_Plain_10);
    display.setColor(WHITE);
    display.setTextAlignment(TEXT_ALIGN_CENTER);
    display.drawString(64, 0, "Erase everything?");
    display.drawString(64, 10, "Apps, WiFi, settings");
    display.drawString(64, 20, "and account link go.");
    display.drawString(64, 30, "Memory card stays.");
    display.drawString(64, 42, "Hold ENTER 3s BACK=no");
    display.drawRect(4, 55, 120, 8);
    const int width = static_cast<int>(116U * hold.progress(now) /
                                       FactoryResetPolicy::Hold::kDurationMs);
    if (width) display.fillRect(6, 57, width, 4);
    display.display();
    display.setTextAlignment(TEXT_ALIGN_LEFT);
}

#ifdef CF_TEST_CLI
void confirmFromCli(bool holdBetweenErases, Fault fault) {
    const auto reason = refusal();
    if (reason != FactoryResetPolicy::Refusal::None) {
        Serial.printf("[cmd] reset.factory=refused reason=%s\n", refusalText(reason));
        return;
    }
    if (SerialCli::instance().radioBusy()) {
        Serial.println("[cmd] reset.factory=refused reason=radio-busy");
        return;
    }
    // A bench command can arrive while a radio or delivered app is active.
    // AppManager's normal switch runs that app's end path before storage goes.
    if (AppManager::instance().activeApp() != APP_FACTORY_RESET)
        AppManager::instance().switchToApp(APP_FACTORY_RESET);
    if (AppManager::instance().activeApp() != APP_FACTORY_RESET) {
        Serial.println("[cmd] reset.factory=refused reason=app-busy");
        return;
    }
    holdAfterFormat = holdBetweenErases;
    faultMark = fault == Fault::MarkWrite;
    faultFsMagic = fault == Fault::Format ? kFaultFsMagic : 0;
    Serial.println("[cmd] reset.factory=start");
    Serial.flush();
    eraseAndRestart();
}
#endif

} // namespace FactoryReset
