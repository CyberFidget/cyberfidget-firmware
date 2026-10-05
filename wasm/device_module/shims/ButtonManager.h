// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC
//
// Guest-side ButtonManager shim. Callback registration is guest-local: the
// device host registers for ALL buttons on the real ButtonManager and
// forwards each event into the module's exported app_handle_button(), which
// dispatches to whatever the app registered here. Enum values must match
// lib/ButtonManager/ButtonManager.h.
//
// The host forwards only (index, eventType), so press duration and pressed
// state are reconstructed here from event delivery times: Released carries
// how long the button was down, Held how long it has been down (never below
// the firmware's 1500 ms hold threshold), and isPressed() follows the last
// Pressed/Released. Events are delivered at the start of the next frame, so
// durations are accurate to about one frame.

#ifndef BUTTON_MANAGER_H  // same guard as the real header — must shadow it
#define BUTTON_MANAGER_H

#include "cf_hal_imports.h"  // cf_millis, declared as a device import

enum ButtonEventType {
    ButtonEvent_None,
    ButtonEvent_Pressed,
    ButtonEvent_Released,
    ButtonEvent_Held,
};

struct ButtonEvent {
    int buttonIndex;
    ButtonEventType eventType;
    unsigned long duration;
};

typedef void (*ButtonCallback)(const ButtonEvent&);

class ButtonManager {
public:
    static constexpr int kMaxButtons = 6;
    static constexpr unsigned long kHoldThresholdMs = 1500;  // lib/HAL/HAL.cpp hold threshold

    void registerCallback(int buttonIndex, ButtonCallback callback) {
        if (buttonIndex < 0 || buttonIndex >= kMaxButtons) return;
        callbacks[buttonIndex] = callback;
    }
    void unregisterCallback(int buttonIndex) {
        if (buttonIndex < 0 || buttonIndex >= kMaxButtons) return;
        callbacks[buttonIndex] = nullptr;
    }
    bool hasCallback(int buttonIndex) const {
        return buttonIndex >= 0 && buttonIndex < kMaxButtons && callbacks[buttonIndex];
    }
    ButtonCallback getCallback(int buttonIndex) const {
        if (buttonIndex < 0 || buttonIndex >= kMaxButtons) return nullptr;
        return callbacks[buttonIndex];
    }
    bool isPressed(int buttonIndex) const {
        return buttonIndex >= 0 && buttonIndex < kMaxButtons && pressed[buttonIndex];
    }

    // Called by the exported app_handle_button() glue. Tracks state even when
    // no callback is registered, so isPressed() works for polling apps.
    void dispatch(int buttonIndex, int eventType) {
        if (buttonIndex < 0 || buttonIndex >= kMaxButtons) return;
        const unsigned long now = cf_millis();
        unsigned long duration = 0;
        switch (eventType) {
        case ButtonEvent_Pressed:
            pressed[buttonIndex] = true;
            pressedAt[buttonIndex] = now;
            break;
        case ButtonEvent_Released:
            if (pressed[buttonIndex]) duration = now - pressedAt[buttonIndex];
            pressed[buttonIndex] = false;
            break;
        case ButtonEvent_Held:
            duration = pressed[buttonIndex] ? now - pressedAt[buttonIndex] : 0;
            if (duration < kHoldThresholdMs) duration = kHoldThresholdMs;
            break;
        default:
            break;
        }
        ButtonCallback cb = callbacks[buttonIndex];
        if (!cb) return;
        ButtonEvent ev;
        ev.buttonIndex = buttonIndex;
        ev.eventType   = (ButtonEventType)eventType;
        ev.duration    = duration;
        cb(ev);
    }

private:
    ButtonCallback callbacks[kMaxButtons] = {};
    bool pressed[kMaxButtons] = {};
    unsigned long pressedAt[kMaxButtons] = {};
};

#endif  // BUTTON_MANAGER_H
