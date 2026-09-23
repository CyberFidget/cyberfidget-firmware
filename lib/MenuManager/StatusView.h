// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#ifndef STATUS_VIEW_H
#define STATUS_VIEW_H

#include <stdint.h>

#include "StatusService.h"

/**
 * @brief Screen side of StatusService: the main-menu status bar, the
 * "Status" screen, and major-event popups.
 *
 *  - drawBar(): the thin strip MenuManager draws across the top of every
 *    menu screen. Apps and screensavers never draw it; they keep 128x64.
 *    Left: WiFi glyph plus the age of the last check-in ("--", "<1h",
 *    "5h", "3d"), or the glyph alone while Dev mode is listening. Middle:
 *    the service's current line (marquee when long). Right: battery %.
 *  - The Status app (APP_STATUS): last check-in, battery detail, and the
 *    pending notification list. Up/Down move, Back returns to the menu.
 *    Leaving it marks everything seen, which clears the badge.
 *  - popup(): a ModalPrompt with an accept option and "Later". Later, a
 *    timeout, or a teardown routes the event to the bar and badge; so does
 *    a popup that cannot open (not on the menu). Accept clears the entry
 *    and calls onAccept, if given - what accepting means belongs to the
 *    caller.
 */
namespace StatusView {

// Geometry the menu lays out around.
constexpr int kBarHeight = 12;   // strip rows 0..11; menu rows start below

typedef void (*AcceptCallback)(StatusKind kind);
// Called with accepted=true/false once the popup is answered (optional).
typedef void (*ResultCallback)(StatusKind kind, bool accepted, bool shown);

/** Draw the strip over rows 0..kBarHeight-1 (clears that area first). */
void drawBar();

/**
 * Seconds on the clock the bar uses for check-in age. Until the check-in
 * code supplies a real clock this is uptime plus kClockBaseSec, so a
 * check-in can be recorded days in the past right after boot (bench).
 * Posters of setCheckIn() must use this same clock.
 */
constexpr uint32_t kClockBaseSec = 100u * 86400u;   // 100 days
uint32_t nowSec();

/**
 * @brief Ask about a major event. Returns true when the popup opened; false
 * when it could not (it was then routed to the bar + badge).
 */
bool popup(StatusKind kind, const char *text, uint8_t flags = 0,
           AcceptCallback onAccept = nullptr, ResultCallback onResult = nullptr);

// Status app lifecycle (AppManifest).
void appBegin();
void appEnd();
void appUpdate();

}  // namespace StatusView

#endif
