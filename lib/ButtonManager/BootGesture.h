// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#ifndef BOOT_GESTURE_H
#define BOOT_GESTURE_H

// A physical press present at start-up belongs to the wake gesture. Wait
// for a stable release before allowing the next gesture through.
class BootGesture {
public:
    void init(bool pressed) { blocked = pressed; releasing = false; }
    bool swallow(bool pressed, unsigned long now, unsigned long debounceMs) {
        if (!blocked) return false;
        if (pressed) releasing = false;
        else if (!releasing) { releasing = true; releaseAt = now; }
        else if (now - releaseAt > debounceMs) blocked = false;
        return true; // The release that ends the wake gesture is swallowed too.
    }
private:
    bool blocked = false;
    bool releasing = false;
    unsigned long releaseAt = 0;
};
#endif
