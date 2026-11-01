// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2026 Mike Garrison

#include "Counter.h"
#include <Preferences.h>
#include "ButtonManager.h"
#include "DisplayProxy.h"
#include "HAL.h"
#include "MenuManager.h"

namespace {

// Every button the app answers; HAL.h says which index is which.
constexpr int kButtons[] = {
    button_TopLeftIndex,  button_MiddleLeftIndex,
    button_TopRightIndex, button_MiddleRightIndex, button_BottomRightIndex,
    button_SelectIndex,
};

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

// The count a button adds to; -1 for Back.
int sideFor(int idx)
{
    switch (idx) {
    case button_TopLeftIndex:
    case button_MiddleLeftIndex:   return 0;
    case button_TopRightIndex:
    case button_MiddleRightIndex:
    case button_BottomRightIndex:  return 1;
    default:                       return -1; // button_SelectIndex (back)
    }
}

void setCount(int side, uint32_t value)
{
    counts[side] = value;
    dirty = true;
    lastChangeMs = millis();
}

void onButton(const ButtonEvent& event)
{
    const int side = sideFor(event.buttonIndex);
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
    for (int b : kButtons) HAL::buttonManager().registerCallback(b, onButton);
}

void counterEnd()
{
    if (dirty) save();
    for (int b : kButtons) HAL::buttonManager().unregisterCallback(b);
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
