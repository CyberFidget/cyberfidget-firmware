// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#include "SpeakerEqScreen.h"

#include <Arduino.h>

#include "AudioManager.h"
#include "DisplayProxy.h"
#include "HAL.h"
#include "MenuManager.h"
#include "RGBController.h"
#include "SpeakerEqPresets.h"
#include "globals.h"

namespace SpeakerEqScreen {
namespace {

auto& display = HAL::displayProxy();

constexpr int kScreenW = 128;
constexpr int kTitleH  = 14;

// A rising run that covers the low, middle and top of the speaker's range,
// so each preset's character is heard.
const AudioManager::ToneStep kPreview[] = {
    {500.0f,  110, 20},
    {1000.0f, 110, 20},
    {2000.0f, 110, 20},
    {3500.0f, 110, 0},
};

int selected = 0;

void step(int dir) {
    selected = (selected + dir + kSpeakerEqPresetCount) % kSpeakerEqPresetCount;
    AudioManager& audio = HAL::audioManager();
    audio.setSpeakerEqPreset(selected);
    audio.playSequence(kPreview, (int)(sizeof(kPreview) / sizeof(kPreview[0])));
}

void onLeft(const ButtonEvent& event) {
    if (event.eventType == ButtonEvent_Pressed) step(-1);
}
void onRight(const ButtonEvent& event) {
    if (event.eventType == ButtonEvent_Pressed) step(1);
}
void onBack(const ButtonEvent& event) {
    if (event.eventType == ButtonEvent_Released) MenuManager::instance().returnToMenu();
}

} // namespace

void begin() {
    auto& buttons = HAL::buttonManager();
    buttons.registerCallback(button_LeftIndex, onLeft);
    buttons.registerCallback(button_RightIndex, onRight);
    buttons.registerCallback(button_SelectIndex, onBack);
    setColorsOff();
    selected = HAL::audioManager().speakerEqPreset();
}

void end() {
    auto& buttons = HAL::buttonManager();
    buttons.unregisterCallback(button_LeftIndex);
    buttons.unregisterCallback(button_RightIndex);
    buttons.unregisterCallback(button_SelectIndex);
    HAL::audioManager().stopSequence();
    setColorsOff();
}

void update() {
    const SpeakerEqPreset& p = kSpeakerEqPresets[selected];
    display.clear();
    display.setFont(ArialMT_Plain_10);
    display.setColor(WHITE);
    display.fillRect(0, 0, kScreenW, kTitleH);
    display.setColor(BLACK);
    display.setTextAlignment(TEXT_ALIGN_CENTER);
    display.drawString(kScreenW / 2, 1, "Speaker EQ");
    display.setColor(WHITE);
    // < Preset >
    display.setTextAlignment(TEXT_ALIGN_LEFT);
    display.drawString(2, 20, "<");
    display.setTextAlignment(TEXT_ALIGN_RIGHT);
    display.drawString(kScreenW - 2, 20, ">");
    display.setTextAlignment(TEXT_ALIGN_CENTER);
    display.drawString(kScreenW / 2, 20, p.name);
    display.drawString(kScreenW / 2, 34, p.hint);
    display.drawString(kScreenW / 2, 51, "Select: back");
    display.setTextAlignment(TEXT_ALIGN_LEFT);
    display.display();
}

} // namespace SpeakerEqScreen
