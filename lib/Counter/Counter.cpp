// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2026 Mike Garrison

#include "Counter.h"
#include <Preferences.h>
#include "ButtonManager.h"
#include "DisplayProxy.h"
#include "HAL.h"
#include "MenuManager.h"

namespace {

// The count each button adds to, by button index; -1 is Back.
constexpr int8_t kSide[] = {0, 1, 0, 1, -1, 1};
static_assert(button_TopLeftIndex == 0 && button_TopRightIndex == 1 &&
              button_MiddleLeftIndex == 2 && button_MiddleRightIndex == 3 &&
              button_BottomLeftIndex == 4 && button_BottomRightIndex == 5,
              "kSide follows the HAL button order");
constexpr int kButtons = sizeof(kSide) / sizeof(kSide[0]);

// Presses are saved together once the buttons have been still this long,
// so a burst of taps is one flash write. Idle sleep comes much later.
constexpr unsigned long kSaveDelayMs = 2000;

const char* const kKeys[2] = {"a", "b"};
uint32_t counts[2];
bool dirty = false;
unsigned long lastChangeMs = 0;

void save()
{
    Preferences prefs;
    prefs.begin("counter", false);
    for (int i = 0; i < 2; i++) prefs.putUInt(kKeys[i], counts[i]);
    prefs.end();
    dirty = false;
}

void setCount(int side, uint32_t value)
{
    counts[side] = value;
    dirty = true;
    lastChangeMs = millis();
}

void onButton(const ButtonEvent& event)
{
    const int side = kSide[event.buttonIndex];
    if (side < 0) {
        // switchToApp ends this app, which saves.
        if (event.eventType == ButtonEvent_Released) MenuManager::instance().returnToMenu();
        return;
    }
    const bool top = event.buttonIndex == button_TopLeftIndex ||
                     event.buttonIndex == button_TopRightIndex;
    if (event.eventType == ButtonEvent_Pressed) setCount(side, counts[side] + 1);
    else if (event.eventType == ButtonEvent_Held && top) setCount(side, 0);
}

} // namespace

void counterBegin()
{
    Preferences prefs;
    prefs.begin("counter", true);
    for (int i = 0; i < 2; i++) counts[i] = prefs.getUInt(kKeys[i], 0);
    prefs.end();
    dirty = false;
    for (int i = 0; i < kButtons; i++) HAL::buttonManager().registerCallback(i, onButton);
}

void counterEnd()
{
    if (dirty) save();
    for (int i = 0; i < kButtons; i++) HAL::buttonManager().unregisterCallback(i);
}

void counterUpdate()
{
    if (dirty && millis() - lastChangeMs >= kSaveDelayMs) save();

    DisplayProxy& display = HAL::displayProxy();
    display.clear();
    display.setFont(ArialMT_Plain_24);
    display.setTextAlignment(TEXT_ALIGN_CENTER);
    display.drawString(32, 18, String(counts[0]));
    display.drawString(96, 18, String(counts[1]));
    display.drawLine(64, 4, 64, 60);
    display.display();
}
