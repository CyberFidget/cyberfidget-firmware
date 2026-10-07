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
#include <utility>

/**
 * The MIT License (MIT)
 *
 * Copyright (c) 2018 by ThingPulse, Daniel Eichhorn
 * Copyright (c) 2018 by Fabrice Weinberg
 * Copyright (c) 2019 by Helmut Tschemernjak - www.radioshuttle.de
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * ThingPulse invests considerable time and money to develop these open source libraries.
 * Please support us by buying our products (and not the clones) from
 * https://thingpulse.com
 *
 */



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
    // ThingPulse scanline fill using the existing horizontal-line import.
    void fillTriangle(int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                                   int16_t x2, int16_t y2) {
      int16_t a, b, y, last;

      if (y0 > y1) {
        std::swap(y0, y1);
        std::swap(x0, x1);
      }
      if (y1 > y2) {
        std::swap(y2, y1);
        std::swap(x2, x1);
      }
      if (y0 > y1) {
        std::swap(y0, y1);
        std::swap(x0, x1);
      }

      if (y0 == y2) {
        a = b = x0;
        if (x1 < a) {
          a = x1;
        } else if (x1 > b) {
          b = x1;
        }
        if (x2 < a) {
          a = x2;
        } else if (x2 > b) {
          b = x2;
        }
        drawHorizontalLine(a, y0, b - a + 1);
        return;
      }

      int16_t
        dx01 = x1 - x0,
        dy01 = y1 - y0,
        dx02 = x2 - x0,
        dy02 = y2 - y0,
        dx12 = x2 - x1,
        dy12 = y2 - y1;
      int32_t
        sa   = 0,
        sb   = 0;

      if (y1 == y2) {
        last = y1; // Include y1 scanline
      } else {
        last = y1 - 1; // Skip it
      }

      for (y = y0; y <= last; y++) {
        a = x0 + sa / dy01;
        b = x0 + sb / dy02;
        sa += dx01;
        sb += dx02;

        if (a > b) {
          std::swap(a, b);
        }
        drawHorizontalLine(a, y, b - a + 1);
      }

      sa = dx12 * (y - y1);
      sb = dx02 * (y - y0);
      for (; y <= y2; y++) {
        a = x1 + sa / dy12;
        b = x0 + sb / dy02;
        sa += dx12;
        sb += dx02;

        if (a > b) {
          std::swap(a, b);
        }
        drawHorizontalLine(a, y, b - a + 1);
      }
    }
    void drawXbm(int16_t x, int16_t y, int16_t w, int16_t h, const unsigned char* data) {
        cf_display_draw_xbm(x, y, w, h, data);
    }
    void drawProgressBar(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint8_t progress) {
        cf_display_draw_rect((int16_t)x, (int16_t)y, (int16_t)w, (int16_t)h);
        uint16_t fill = (uint16_t)((w - 2) * progress / 100);
        cf_display_fill_rect((int16_t)(x + 1), (int16_t)(y + 1), (int16_t)fill, (int16_t)(h - 2));
    }

    // --- Text ---
    uint16_t drawString(int16_t x, int16_t y, const String& text) {
        cf_display_draw_string(x, y, text.c_str(), (int32_t)text.length());
        return 0;
    }
    // Emulator surface renders unwrapped (same behavior as wasm/shims); the
    // width argument is accepted for firmware API compatibility.
    uint16_t drawStringMaxWidth(int16_t x, int16_t y, uint16_t maxLineWidth, const String& text) {
        (void)maxLineWidth;
        return drawString(x, y, text);
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
