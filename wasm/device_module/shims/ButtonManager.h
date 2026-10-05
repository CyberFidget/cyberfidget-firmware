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
// Pressed/Released/Held.
//
// Like the native ButtonManager (scan all buttons, then dispatch), events are
// recorded as they arrive and callbacks run afterwards from flushCallbacks()
// at the top of app_update(), so a callback sees every state change from the
// same frame (e.g. chords via isPressed()). The host delivers a frame's
// events just before app_update(), so durations are best-effort: accurate to
// the time between frames, not to the host's debounced scan.

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

    // Called by the exported app_handle_button() glue: updates state now (even
    // with no callback registered, so isPressed() works for polling apps) and
    // queues the event for flushCallbacks().
    void dispatch(int buttonIndex, int eventType) {
        if (buttonIndex < 0 || buttonIndex >= kMaxButtons) return;
        const uint32_t now = cf_millis();
        uint32_t duration = 0;
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
            if (!pressed[buttonIndex]) {
                // Press happened before the module started: it has been down
                // for at least the hold threshold.
                pressed[buttonIndex] = true;
                pressedAt[buttonIndex] = now - (uint32_t)kHoldThresholdMs;
            }
            duration = now - pressedAt[buttonIndex];
            if (duration < kHoldThresholdMs) duration = kHoldThresholdMs;
            break;
        default:
            return;
        }
        if (pendingCount == kMaxPending) return;  // full: drop (matches a full native queue)
        ButtonEvent& ev = pending[(pendingHead + pendingCount) % kMaxPending];
        ev.buttonIndex = buttonIndex;
        ev.eventType   = (ButtonEventType)eventType;
        ev.duration    = duration;
        ++pendingCount;
    }

    // Called by the glue at the top of app_update(): runs callbacks for the
    // events recorded since the last call, in arrival order.
    void flushCallbacks() {
        while (pendingCount > 0) {
            const ButtonEvent ev = pending[pendingHead];
            pendingHead = (pendingHead + 1) % kMaxPending;
            --pendingCount;
            ButtonCallback cb = callbacks[ev.buttonIndex];
            if (cb) cb(ev);
        }
    }

private:
    static constexpr int kMaxPending = 32;
    ButtonCallback callbacks[kMaxButtons] = {};
    bool pressed[kMaxButtons] = {};
    uint32_t pressedAt[kMaxButtons] = {};
    ButtonEvent pending[kMaxPending] = {};
    int pendingHead = 0;
    int pendingCount = 0;
};

#endif  // BUTTON_MANAGER_H
