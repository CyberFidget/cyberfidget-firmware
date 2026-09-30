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
//
// Page requests are counted, never cleared: a join compares the count with
// the value last seen while nobody was on the network, so a page fetched
// before the main loop's first poll after the join still counts.
//
// Threads: onPageRequest() runs on the web server's task; everything else
// on the main loop. Only the counter is shared, and it is atomic.

#include <atomic>
#include <stdint.h>

class OpenAddressHint {
public:
    static constexpr uint32_t kDelayMs = 10000;
    static constexpr uint32_t kAlternateMs = 3000;

    // Main loop: how many devices are on the Fidget's network now.
    void onStations(int count, uint32_t nowMs) {
        if (count <= 0) {
            joined_ = false;
            pagesWhenEmpty_ = pages_.load(std::memory_order_relaxed);
        } else if (!joined_) {
            joined_ = true;
            joinedAtMs_ = nowMs;
        }
    }

    // Web server task: the portal page was requested.
    void onPageRequest() { pages_.fetch_add(1, std::memory_order_relaxed); }

    // Main loop.
    bool show(uint32_t nowMs) const {
        return joined_ &&
               pages_.load(std::memory_order_relaxed) == pagesWhenEmpty_ &&
               (uint32_t)(nowMs - joinedAtMs_) >= kDelayMs;
    }

    // Main loop: while the hint is due, the address takes turns with the
    // screen's usual bottom line (e.g. "BACK to finish"), kAlternateMs each,
    // address first - so the way out never disappears.
    bool showAddressNow(uint32_t nowMs) const {
        if (!show(nowMs)) return false;
        const uint32_t since = (uint32_t)(nowMs - joinedAtMs_) - kDelayMs;
        return (since / kAlternateMs) % 2 == 0;
    }

    // Main loop, at portal start (before the web server runs).
    void reset() {
        joined_ = false;
        joinedAtMs_ = 0;
        pagesWhenEmpty_ = pages_.load(std::memory_order_relaxed);
    }

private:
    std::atomic<uint32_t> pages_{0};
    uint32_t pagesWhenEmpty_ = 0;
    bool joined_ = false;
    uint32_t joinedAtMs_ = 0;
};

#endif
