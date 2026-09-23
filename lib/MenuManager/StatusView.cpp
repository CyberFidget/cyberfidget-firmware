// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#include "StatusView.h"

#include <Arduino.h>
#include <stdio.h>
#include <string.h>

#include "HAL.h"
#include "DisplayProxy.h"
#include "MenuManager.h"
#include "ModalPrompt.h"
#include "RGBController.h"
#include "ModalPromptModel.h"
#include "ScrollLabel.h"

namespace {

auto &display = HAL::displayProxy();

constexpr int kScreenW = 128;
// ArialMT_Plain_10 draws capitals on rows 3..9 of its 13-row box and
// descenders on rows 10..11, so text drawn at y=0 fits rows 0..11 exactly.
// (Text is never drawn at a negative y: the driver drops the top page.)
constexpr int kBarTextY = 0;
constexpr int kGlyphY   = 3;   // glyph rows match the capitals
constexpr int kGlyphW   = 7;
constexpr int kGap      = 3;

// WiFi fan, 7x7, drawn one arc per freshness level (outer arc = checked in
// within the hour). '#' = pixel.
const char *const kFanOuter[]  = { ".#####.", "#.....#" };
const char *const kFanMiddle[] = { "..###..", ".#...#." };
const char *const kFanDot      = "...#...";

void drawRows(int x, int y, const char *const *rows, int n) {
    for (int r = 0; r < n; r++) {
        for (int c = 0; rows[r][c]; c++) {
            if (rows[r][c] == '#') display.setPixel(x + c, y + r);
        }
    }
}

// Arcs shown per glyph state: Live and Hour draw all three, Today two,
// Days and Never only the dot (the age text beside it tells them apart).
void drawGlyph(StatusGlyph g) {
    const bool live = (g == StatusGlyph::Live);
    if (live) {
        // Dev mode listening: the only "on now" state, drawn inverted.
        display.setColor(WHITE);
        display.fillRect(0, kGlyphY - 1, kGlyphW + 2, 9);
        display.setColor(BLACK);
    }
    const int x = live ? 1 : 0;
    if (g == StatusGlyph::Live || g == StatusGlyph::Hour) drawRows(x, kGlyphY, kFanOuter, 2);
    if (g == StatusGlyph::Live || g == StatusGlyph::Hour || g == StatusGlyph::Today) {
        drawRows(x, kGlyphY + 2, kFanMiddle, 2);
    }
    drawRows(x, kGlyphY + 5, &kFanDot, 1);
    display.setColor(WHITE);
}

int batteryPercent() {
    float p = batteryVoltagePercentage;
    if (p < 0.0f) p = 0.0f;
    if (p > 100.0f) p = 100.0f;
    return (int)(p + 0.5f);
}

bool batteryPlausible() {
    return batteryVoltage >= 2.0f && batteryVoltage <= 4.6f;
}

int textWidth(const char *s) {
    return display.getStringWidth(s, (uint16_t)strlen(s));
}

// Bar marquee state: restarted whenever the shown line changes.
ScrollLabel barLabel;
uint32_t    barSeq = 0;
char        barText[StatusEntry::kMaxText] = {0};

// ---- popup ----
StatusKind            popupKind = StatusKind::Info;
char                  popupText[StatusEntry::kMaxText] = {0};
uint8_t               popupFlags = 0;
StatusView::AcceptCallback popupAccept = nullptr;
StatusView::ResultCallback popupResult = nullptr;

const char *acceptLabel(StatusKind kind) {
    switch (kind) {
        case StatusKind::UpdateReady:    return "Install now";
        case StatusKind::ChangesWaiting: return "Get them now";
        default:                         return "OK";
    }
}

void onPopupDone(int result) {
    const bool accepted = (result == 0);
    StatusService::instance().resolvePopup(popupKind, popupText, accepted,
                                           (uint32_t)millis(), popupFlags);
    StatusView::AcceptCallback accept = popupAccept;
    StatusView::ResultCallback report = popupResult;
    popupAccept = nullptr;
    popupResult = nullptr;
    if (accepted && accept) accept(popupKind);
    if (report) report(popupKind, accepted, true);
}

// ---- Status screen ----
constexpr int kTitleH    = 14;
constexpr int kListY     = 16;
constexpr int kRowH      = 12;
constexpr int kRows      = 4;
constexpr int kTextX     = 4;
constexpr int kMaxLines  = 3 + StatusService::kMaxEntries;
constexpr int kLineLen   = StatusEntry::kMaxText + 16;

char             lines[kMaxLines][kLineLen];
int              lineCount = 0;
ModalPromptModel listModel;          // selection + window, wraps like the menu
ScrollLabel      focusLabel;
int              focusFor = -1;

void buildLines(uint32_t nowSec) {
    StatusService &svc = StatusService::instance();
    lineCount = 0;

    // Last check-in: age, and whether the result was a cached one.
    if (!svc.hasCheckIn()) {
        snprintf(lines[lineCount++], kLineLen, "Checked in: never");
    } else {
        const uint32_t age = svc.checkInAgeSec(nowSec);
        char when[16];
        if (age < StatusService::kHourSec)      snprintf(when, sizeof(when), "%lum", (unsigned long)(age / 60));
        else if (age < StatusService::kDaySec)  snprintf(when, sizeof(when), "%luh", (unsigned long)(age / StatusService::kHourSec));
        else                                    snprintf(when, sizeof(when), "%lud", (unsigned long)(age / StatusService::kDaySec));
        snprintf(lines[lineCount++], kLineLen, "Checked in: %s ago%s",
                 when, svc.checkInCached() ? ", cached" : "");
    }

    // Battery detail.
    if (batteryPlausible()) {
        snprintf(lines[lineCount++], kLineLen, "Battery: %d%%, %.2f V",
                 batteryPercent(), batteryVoltage);
        snprintf(lines[lineCount++], kLineLen, "Battery trend: %+.1f%%/h",
                 batteryChangeRate);
    } else {
        snprintf(lines[lineCount++], kLineLen, "Battery: --");
    }

    // Pending notifications, most important first. '*' = needs attention.
    const StatusEntry *list[StatusService::kMaxEntries];
    const int n = svc.pending(list, StatusService::kMaxEntries);
    if (n == 0) {
        snprintf(lines[lineCount++], kLineLen, "No notifications");
    }
    for (int i = 0; i < n && lineCount < kMaxLines; i++) {
        const StatusEntry *e = list[i];
        const char *extra = e->late() && e->cached() ? " (late, cached)"
                          : e->late()                ? " (late)"
                          : e->cached()              ? " (cached)" : "";
        snprintf(lines[lineCount++], kLineLen, "%s%s%s",
                 e->attention ? "* " : "", e->text, extra);
    }
}

void onStatusUp(const ButtonEvent &event) {
    if (event.eventType == ButtonEvent_Pressed) listModel.moveUp((uint32_t)millis());
}
void onStatusDown(const ButtonEvent &event) {
    if (event.eventType == ButtonEvent_Pressed) listModel.moveDown((uint32_t)millis());
}
void onStatusBack(const ButtonEvent &event) {
    if (event.eventType == ButtonEvent_Released) {
        MenuManager::instance().returnToMenu();
    }
}

}  // namespace

namespace StatusView {

uint32_t nowSec()
{
    // Uptime seconds (offset, see the header) until the check-in code
    // supplies a real clock; the service only needs posters and the bar to
    // agree on one.
    return kClockBaseSec + (uint32_t)(millis() / 1000UL);
}

void drawBar()
{
    StatusService &svc = StatusService::instance();
    const uint32_t nowMs = (uint32_t)millis();
    const uint32_t sec = nowSec();
    svc.expire(nowMs);

    display.setFont(ArialMT_Plain_10);
    display.setColor(BLACK);
    display.fillRect(0, 0, kScreenW, kBarHeight);
    display.setColor(WHITE);

    // Left: glyph + check-in age. Right: battery %.
    const StatusGlyph g = svc.glyph(sec);
    char age[8];
    svc.ageLabel(sec, age, sizeof(age));
    int leftEnd = kGlyphW + (g == StatusGlyph::Live ? 2 : 0) + kGap;
    if (age[0]) leftEnd += textWidth(age) + kGap;

    char batt[8];
    if (batteryPlausible()) snprintf(batt, sizeof(batt), "%d%%", batteryPercent());
    else                    snprintf(batt, sizeof(batt), "--%%");
    const int rightStart = kScreenW - textWidth(batt) - kGap;

    // Middle: the current line; restart the marquee when it changes.
    const StatusEntry *cur = svc.current();
    if (cur) {
        if (cur->seq != barSeq || strcmp(cur->text, barText) != 0) {
            barSeq = cur->seq;
            strncpy(barText, cur->text, sizeof(barText) - 1);
            barText[sizeof(barText) - 1] = '\0';
            barLabel.restart(nowMs);
        }
        barLabel.draw(leftEnd, kBarTextY, rightStart - leftEnd, cur->text, false);
    } else {
        barSeq = 0;
        barText[0] = '\0';
    }

    // Long text scrolls under the side regions: blank them, then draw them.
    display.setColor(BLACK);
    display.fillRect(0, 0, leftEnd, kBarHeight);
    display.fillRect(rightStart, 0, kScreenW - rightStart, kBarHeight);
    display.setColor(WHITE);

    drawGlyph(g);
    display.setTextAlignment(TEXT_ALIGN_LEFT);
    if (age[0]) {
        display.drawString(kGlyphW + (g == StatusGlyph::Live ? 2 : 0) + kGap, kBarTextY, age);
    }
    display.setTextAlignment(TEXT_ALIGN_RIGHT);
    display.drawString(kScreenW - 1, kBarTextY, batt);
    display.setTextAlignment(TEXT_ALIGN_LEFT);
}

bool popup(StatusKind kind, const char *text, uint8_t flags,
           AcceptCallback onAccept, ResultCallback onResult)
{
    const char *body = (text && text[0]) ? text : StatusService::defaultText(kind);
    ModalPrompt &prompt = ModalPrompt::instance();
    if (!body[0] || prompt.isOpen() || !prompt.canOpen()) {
        // Never dropped: no popup right now means the bar and the badge.
        if (body[0]) {
            StatusService::instance().resolvePopup(kind, body, false,
                                                   (uint32_t)millis(), flags);
        }
        if (onResult) onResult(kind, false, false);
        return false;
    }

    popupKind  = kind;
    strncpy(popupText, body, sizeof(popupText) - 1);
    popupText[sizeof(popupText) - 1] = '\0';
    popupFlags  = flags;
    popupAccept = onAccept;
    popupResult = onResult;

    const char *const options[] = { acceptLabel(kind), "Later" };
    if (!prompt.open(popupText, options, 2, onPopupDone)) {
        popupAccept = nullptr;
        popupResult = nullptr;
        StatusService::instance().resolvePopup(kind, popupText, false,
                                               (uint32_t)millis(), flags);
        if (onResult) onResult(kind, false, false);
        return false;
    }
    return true;
}

void appBegin()
{
    auto &buttons = HAL::buttonManager();
    buttons.registerCallback(button_UpIndex, onStatusUp);
    buttons.registerCallback(button_DownIndex, onStatusDown);
    buttons.registerCallback(button_SelectIndex, onStatusBack);
    setColorsOff();
    buildLines(nowSec());
    listModel.open(lineCount, kRows, (uint32_t)millis(), 0);
    focusFor = -1;
}

void appEnd()
{
    auto &buttons = HAL::buttonManager();
    buttons.unregisterCallback(button_UpIndex);
    buttons.unregisterCallback(button_DownIndex);
    buttons.unregisterCallback(button_SelectIndex);
    listModel.dismiss();
    setColorsOff();
    // Leaving the screen means everything on it was seen: badge off.
    StatusService::instance().markSeen();
}

void appUpdate()
{
    StatusService::instance().expire((uint32_t)millis());
    buildLines(nowSec());
    if (lineCount != listModel.optionCount()) {
        listModel.open(lineCount, kRows, (uint32_t)millis(), 0);
        focusFor = -1;
    }

    display.clear();
    display.setFont(ArialMT_Plain_10);

    // Title bar (inverted), like the prompt's.
    display.setColor(WHITE);
    display.fillRect(0, 0, kScreenW, kTitleH);
    display.setColor(BLACK);
    display.setTextAlignment(TEXT_ALIGN_CENTER);
    display.drawString(kScreenW / 2, 1, "Status");
    display.setColor(WHITE);

    if (listModel.selected() != focusFor) {
        focusFor = listModel.selected();
        focusLabel.restart((uint32_t)millis());
    }

    const int start = listModel.windowStart();
    const int shown = listModel.windowCount();
    for (int r = 0; r < shown; r++) {
        const int idx = start + r;
        const int y = kListY + r * kRowH;
        if (idx == listModel.selected()) {
            display.setColor(WHITE);
            display.fillRect(0, y, kScreenW, kRowH);
            display.setColor(BLACK);
            focusLabel.draw(kTextX, y, kScreenW - 2 * kTextX, lines[idx], false);
            display.setColor(WHITE);
        } else {
            display.setTextAlignment(TEXT_ALIGN_LEFT);
            display.drawString(kTextX, y, lines[idx]);
        }
    }
    display.setTextAlignment(TEXT_ALIGN_LEFT);
    display.display();
}

}  // namespace StatusView
