// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#include "SavedWifiScreen.h"

#include <Arduino.h>
#include <stdio.h>
#include <string.h>

#include "AppDefs.h"
#include "AppManager.h"
#include "DisplayProxy.h"
#include "HAL.h"
#include "MenuManager.h"
#include "ModalPrompt.h"
#include "ModalPromptModel.h"
#include "RGBController.h"
#include "SavedWifi.h"
#include "ScrollLabel.h"
#include "globals.h"

namespace SavedWifiScreen {
namespace {

auto& display = HAL::displayProxy();

constexpr int kScreenW = 128;
constexpr int kTitleH  = 14;
constexpr int kListY   = 16;
constexpr int kRowH    = 12;
constexpr int kRows    = 4;
constexpr int kTextX   = 4;
constexpr int kGutter  = 4;

char names[WifiList::kMax][WifiList::kNameMax + 1];
int count = 0;
ModalPromptModel listModel;
ScrollLabel focusLabel;
int focusFor = -1;
bool enterArmed = false;
char chosen[WifiList::kNameMax + 1] = {0};   // the network a prompt is about
bool chosenFirst = false;

// Rows: the saved networks, then "Setup WiFi" (or a note when none is saved).
int rowCount() { return (count > 0 ? count : 1) + 1; }
bool isSetupRow(int row) { return row == rowCount() - 1; }

void refresh() {
    count = SavedWifi::names(names, WifiList::kMax);
    listModel.open(rowCount(), kRows, (uint32_t)millis(), 0);
    focusFor = -1;
}

void rowLabel(int row, char* out, size_t len) {
    if (isSetupRow(row)) { snprintf(out, len, "Setup WiFi"); return; }
    if (count == 0) { snprintf(out, len, "Nothing saved yet"); return; }
    if (row == 0 && count > 1) snprintf(out, len, "%s (first)", names[row]);
    else snprintf(out, len, "%s", names[row]);
}

void onChoice(int result) {
    // The options are {"Use this first", "Forget", "Cancel"}, without the
    // first one for the network already tried first.
    const int useFirst = chosenFirst ? -2 : 0;
    const int forget = chosenFirst ? 0 : 1;
    if (result == useFirst) {
        const bool ok = SavedWifi::useFirst(chosen);
        Serial.printf("[wifi] screen=first ok=%d\n", ok ? 1 : 0);
    } else if (result == forget) {
        const bool ok = SavedWifi::forget(chosen);
        Serial.printf("[wifi] screen=forget ok=%d\n", ok ? 1 : 0);
    }
    memset(chosen, 0, sizeof(chosen));
    refresh();
}

void activate(int row) {
    if (isSetupRow(row)) {
        AppManager::instance().switchToApp(APP_SETUP_WIFI);
        return;
    }
    if (count == 0 || row >= count) return;
    memcpy(chosen, names[row], sizeof(chosen));
    chosenFirst = row == 0;
    static const char* const kOthers[] = {"Use this first", "Forget", "Cancel"};
    static const char* const kFirst[] = {"Forget", "Cancel"};
    if (chosenFirst) ModalPrompt::instance().open(chosen, kFirst, 2, onChoice);
    else ModalPrompt::instance().open(chosen, kOthers, 3, onChoice);
}

void onUp(const ButtonEvent& event) {
    if (event.eventType == ButtonEvent_Pressed) listModel.moveUp((uint32_t)millis());
}
void onDown(const ButtonEvent& event) {
    if (event.eventType == ButtonEvent_Pressed) listModel.moveDown((uint32_t)millis());
}
void onEnter(const ButtonEvent& event) {
    // Only a press that started on this screen chooses (on its release).
    if (event.eventType == ButtonEvent_Pressed) enterArmed = true;
    if (event.eventType != ButtonEvent_Released || !enterArmed) return;
    enterArmed = false;
    activate(listModel.selected());
}
void onBack(const ButtonEvent& event) {
    if (event.eventType == ButtonEvent_Released) MenuManager::instance().returnToMenu();
}

} // namespace

void begin() {
    auto& buttons = HAL::buttonManager();
    buttons.registerCallback(button_UpIndex, onUp);
    buttons.registerCallback(button_DownIndex, onDown);
    buttons.registerCallback(button_EnterIndex, onEnter);
    buttons.registerCallback(button_SelectIndex, onBack);
    setColorsOff();
    enterArmed = false;
    refresh();
}

void end() {
    auto& buttons = HAL::buttonManager();
    buttons.unregisterCallback(button_UpIndex);
    buttons.unregisterCallback(button_DownIndex);
    buttons.unregisterCallback(button_EnterIndex);
    buttons.unregisterCallback(button_SelectIndex);
    listModel.dismiss();
    setColorsOff();
}

void update() {
    char line[WifiList::kNameMax + 16];
    display.clear();
    display.setFont(ArialMT_Plain_10);
    display.setColor(WHITE);
    display.fillRect(0, 0, kScreenW, kTitleH);
    display.setColor(BLACK);
    display.setTextAlignment(TEXT_ALIGN_CENTER);
    display.drawString(kScreenW / 2, 1, "Saved WiFi");
    display.setColor(WHITE);

    if (listModel.selected() != focusFor) {
        focusFor = listModel.selected();
        focusLabel.restart((uint32_t)millis());
    }
    const int rows = rowCount();
    const int rowW = kScreenW - kGutter;
    const int start = listModel.windowStart();
    for (int r = 0; r < listModel.windowCount(); r++) {
        const int idx = start + r;
        const int y = kListY + r * kRowH;
        rowLabel(idx, line, sizeof(line));
        if (idx == listModel.selected()) {
            display.setColor(WHITE);
            display.fillRect(0, y, rowW, kRowH);
            display.setColor(BLACK);
            focusLabel.draw(kTextX, y - 1, rowW - 2 * kTextX, line, false);
            display.setColor(WHITE);
        } else {
            display.setTextAlignment(TEXT_ALIGN_LEFT);
            display.drawString(kTextX, y - 1, line);
        }
    }
    const int listH = kRows * kRowH;
    display.setColor(BLACK);
    display.fillRect(rowW, kListY, kGutter, listH);
    display.setColor(WHITE);
    if (rows > kRows) {
        const int thumbH = (listH * listModel.windowCount()) / rows;
        const int thumbY = kListY + (listH * start) / rows;
        display.drawRect(kScreenW - 2, kListY, 2, listH);
        display.fillRect(kScreenW - 2, thumbY, 2, thumbH);
    }
    display.setTextAlignment(TEXT_ALIGN_LEFT);
    display.display();
}

} // namespace SavedWifiScreen
