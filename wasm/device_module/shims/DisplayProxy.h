// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC
//
// Guest-side DisplayProxy shim: same class name/API subset as
// lib/DisplayProxy/DisplayProxy.h, forwarding every call to "cf" imports.
// The host renders with the real SSD1306 fonts; guests reference fonts via
// 1-byte ID arrays (ArialMT_Plain_10/16/24 below) so no font data ships in
// the module.

#ifndef DISPLAY_PROXY_H  // same guard as the real header — must shadow it
#define DISPLAY_PROXY_H

#include <stdint.h>

#include "Arduino.h"
#include "cf_hal_imports.h"

// Values match the ThingPulse OLED library enums the firmware uses.
enum OLEDDISPLAY_COLOR {
    BLACK   = 0,
    WHITE   = 1,
    INVERSE = 2,
};

enum OLEDDISPLAY_TEXT_ALIGNMENT {
    TEXT_ALIGN_LEFT        = 0,
    TEXT_ALIGN_RIGHT       = 1,
    TEXT_ALIGN_CENTER      = 2,
    TEXT_ALIGN_CENTER_BOTH = 3,
};

// Font handles: first byte is the font ID sent to the host.
extern const uint8_t ArialMT_Plain_10[];
extern const uint8_t ArialMT_Plain_16[];
extern const uint8_t ArialMT_Plain_24[];

enum class OverlayMode { OVERLAY_OFF, OVERLAY_ON };

class DisplayProxy {
public:
    // --- Display control ---
    void clear() { cf_display_clear(); }
    void display() { cf_display_show(); }

    // --- Pixel drawing ---
    void setColor(OLEDDISPLAY_COLOR color) { cf_display_set_color((int32_t)color); }
    void setPixel(int16_t x, int16_t y) { cf_display_set_pixel(x, y); }
    void drawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1) {
        cf_display_draw_line(x0, y0, x1, y1);
    }
    void drawRect(int16_t x, int16_t y, int16_t w, int16_t h) {
        cf_display_draw_rect(x, y, w, h);
    }
    void fillRect(int16_t x, int16_t y, int16_t w, int16_t h) {
        cf_display_fill_rect(x, y, w, h);
    }
    void drawCircle(int16_t x, int16_t y, int16_t r) { cf_display_draw_circle(x, y, r); }
    void fillCircle(int16_t x, int16_t y, int16_t r) { cf_display_fill_circle(x, y, r); }
    void drawHorizontalLine(int16_t x, int16_t y, int16_t len) {
        cf_display_draw_hline(x, y, len);
    }
    void drawVerticalLine(int16_t x, int16_t y, int16_t len) {
        cf_display_draw_vline(x, y, len);
    }
    void drawTriangle(int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                      int16_t x2, int16_t y2) {
        cf_display_draw_triangle(x0, y0, x1, y1, x2, y2);
    }
    void drawXbm(int16_t x, int16_t y, int16_t w, int16_t h, const unsigned char* data) {
        cf_display_draw_xbm(x, y, w, h, data);
    }

    // --- Text ---
    uint16_t drawString(int16_t x, int16_t y, const String& text) {
        cf_display_draw_string(x, y, text.c_str(), (int32_t)text.length());
        return 0;
    }
    uint16_t getStringWidth(const String& text) {
        return (uint16_t)cf_display_string_width(text.c_str(), (int32_t)text.length());
    }
    uint16_t getStringWidth(const char* text, uint16_t length, bool = false) {
        return (uint16_t)cf_display_string_width(text, (int32_t)length);
    }
    void setTextAlignment(OLEDDISPLAY_TEXT_ALIGNMENT alignment) {
        cf_display_set_align((int32_t)alignment);
    }
    void setFont(const uint8_t* fontData) {
        cf_display_set_font(fontData ? (int32_t)fontData[0] : 10);
    }

    // --- Overlay (no-op in guest; host overlay stays host-controlled) ---
    void setOverlayMode(OverlayMode) {}
    OverlayMode getOverlayMode() const { return OverlayMode::OVERLAY_OFF; }
};

#endif  // DISPLAY_PROXY_H
