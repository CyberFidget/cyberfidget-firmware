// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2026 Dismo Industries LLC

#ifndef OPEN_ADDRESS_HINT_H
#define OPEN_ADDRESS_HINT_H

// When the portal screen swaps its bottom line for the portal's address.
// Normally the joining device's "sign in to network" page opens the portal
// by itself. When a device has joined the Fidget's network but no portal
// page was asked for within kDelayMs (a laptop that also has a wired
// connection sends that page's request out the other way), the screen
// shows where to go instead. Pure, so it is unit-tested natively.

#include <stdint.h>

class OpenAddressHint {
public:
    static constexpr uint32_t kDelayMs = 10000;

    // Main loop: how many devices are on the Fidget's network now.
    void onStations(int count, uint32_t nowMs) {
        if (count > 0 && !joined_) {
            joined_ = true;
            joinedAtMs_ = nowMs;
            pageSeen_ = false;
        } else if (count <= 0) {
            joined_ = false;
        }
    }

    // Any task: the portal page was requested.
    void onPageRequest() { pageSeen_ = true; }

    bool show(uint32_t nowMs) const {
        return joined_ && !pageSeen_ && (uint32_t)(nowMs - joinedAtMs_) >= kDelayMs;
    }

    void reset() {
        joined_ = false;
        pageSeen_ = false;
        joinedAtMs_ = 0;
    }

private:
    bool joined_ = false;
    volatile bool pageSeen_ = false;
    uint32_t joinedAtMs_ = 0;
};

#endif
