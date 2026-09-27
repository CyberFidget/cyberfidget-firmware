// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#include "FactoryReset.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_bt.h>
#include <esp_bt_main.h>
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
    if (esp_bt_controller_get_status() != ESP_BT_CONTROLLER_STATUS_IDLE ||
        esp_bluedroid_get_status() != ESP_BLUEDROID_STATUS_UNINITIALIZED) {
        erasing = false;
        refuseOrFail("Bluetooth is busy");
        return false;
    }
    if (!LoadoutStore::formatForFactoryReset()) {
        erasing = false;
        refuseOrFail("Could not erase apps");
        return false;
    }
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
void confirmFromCli() {
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
    Serial.println("[cmd] reset.factory=start");
    Serial.flush();
    eraseAndRestart();
}
#endif

} // namespace FactoryReset
