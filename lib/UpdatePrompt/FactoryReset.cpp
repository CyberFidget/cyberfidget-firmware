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
bool erasing = false;
#ifdef CF_TEST_CLI
bool holdAfterFormat = false;  // bench: time to cut power between the two erases
#endif

// "Reset in progress" mark. It lives in the settings store itself, so the
// settings erase that ends a reset also clears it; a power cut before that
// leaves it for the next start-up to finish the job.
// Start-ups that tried to finish a reset since power-on. A crash inside the
// finish would otherwise repeat on every start: the third try skips the
// apps erase and still erases the settings (which clears the mark).
RTC_NOINIT_ATTR uint32_t finishTries;
RTC_NOINIT_ATTR uint32_t finishTriesMagic;
constexpr uint32_t kFinishTriesMagic = 0x46524553;  // "FRES"

constexpr const char* kMarkNs = "freset";
constexpr const char* kMarkKey = "busy";

bool writeMark() {
    nvs_handle_t h;
    if (nvs_open(kMarkNs, NVS_READWRITE, &h) != ESP_OK) return false;
    const bool ok = nvs_set_u8(h, kMarkKey, 1) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

bool markSet() {
    nvs_handle_t h;
    if (nvs_open(kMarkNs, NVS_READONLY, &h) != ESP_OK) return false;
    uint8_t value = 0;
    const bool set = nvs_get_u8(h, kMarkKey, &value) == ESP_OK && value != 0;
    nvs_close(h);
    return set;
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

void showMessage(const char* text) {
    display.clear();
    display.setFont(ArialMT_Plain_10);
    display.setColor(WHITE);
    display.setTextAlignment(TEXT_ALIGN_CENTER);
    display.drawString(64, 26, text);
    display.display();
    display.setTextAlignment(TEXT_ALIGN_LEFT);
}

void refuseOrFail(const char* text) {
    Serial.printf("[reset] factory=refused reason=%s\n", text);
    message = text;
    messageAt = millis();
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
    // From here on the reset must finish, if need be on the next start-up.
    // Without the mark the reset still runs (as before the mark existed).
    finishTriesMagic = kFinishTriesMagic;
    finishTries = 0;
    Serial.println(writeMark() ? "[reset] factory=marked" : "[reset] factory=mark-failed");
    if (!LoadoutStore::formatForFactoryReset()) {
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

void finishIfInterrupted() {
    const auto step = FactoryResetPolicy::bootStep(markSet(), UpdateSession::imagePending());
    if (step == FactoryResetPolicy::BootStep::Normal) return;
    if (step == FactoryResetPolicy::BootStep::Wait) {
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
    if (finishTries > 2) Serial.println("[reset] factory=skip-fs retries");
    else if (!LoadoutStore::formatForFactoryReset()) Serial.println("[reset] factory=error fs");
    nvs_flash_deinit();
    if (nvs_flash_erase() != ESP_OK) {
        // Start normally and try again next time rather than restart in a loop.
        Serial.println("[reset] factory=error nvs");
        nvs_flash_init();
        return;
    }
    Serial.println("[reset] factory=done");
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
        if (static_cast<uint32_t>(millis() - messageAt) >= 1400)
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
void confirmFromCli(bool holdBetweenErases) {
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
    Serial.println("[cmd] reset.factory=start");
    Serial.flush();
    eraseAndRestart();
}
#endif

} // namespace FactoryReset
