// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC
//
// Guest-side ButtonManager shim. Callback registration is guest-local: the
// device host registers for ALL buttons on the real ButtonManager and
// forwards each event into the module's exported app_handle_button(), which
// dispatches to whatever the app registered here. Enum values must match
// lib/ButtonManager/ButtonManager.h.

#ifndef BUTTON_MANAGER_H  // same guard as the real header — must shadow it
#define BUTTON_MANAGER_H

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

    void registerCallback(int buttonIndex, ButtonCallback callback);
    void unregisterCallback(int buttonIndex);
    bool hasCallback(int buttonIndex) const;
    ButtonCallback getCallback(int buttonIndex) const;

    // Called by the exported app_handle_button() glue.
    void dispatch(int buttonIndex, int eventType);

private:
    ButtonCallback callbacks[kMaxButtons] = {};
};

#endif  // BUTTON_MANAGER_H
