// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2023-2026 Dismo Industries LLC

#ifndef WASM_SSD1306WIRE_H
#define WASM_SSD1306WIRE_H

#include <cstdint>
#include <cstring>
#include <cmath>
#include <algorithm>

// ---- OLED enums (matching ThingPulse library) ----
enum OLEDDISPLAY_COLOR {
    BLACK = 0,
    WHITE = 1,
    INVERSE = 2
};

enum OLEDDISPLAY_TEXT_ALIGNMENT {
    TEXT_ALIGN_LEFT = 0,
    TEXT_ALIGN_RIGHT = 1,
    TEXT_ALIGN_CENTER = 2,
    TEXT_ALIGN_CENTER_BOTH = 3
};

// Font declarations — matching ThingPulse OLEDDisplayFonts.h
// Implemented in wasm_fonts.cpp with a 5x7 bitmap font scaled to each size
extern const uint8_t* ArialMT_Plain_10;
extern const uint8_t* ArialMT_Plain_16;
extern const uint8_t* ArialMT_Plain_24;

// Defined once in wasm_runtime.cpp (EM_JS cannot live in headers).
// EM_JS produces C linkage, so the declaration must match.
extern "C" void js_push_framebuffer(const uint8_t* buf, int len);

class SSD1306Wire {
public:
    static constexpr int WIDTH = 128;
    static constexpr int HEIGHT = 64;
    static constexpr int BUFFER_SIZE = WIDTH * HEIGHT;

    SSD1306Wire(uint8_t addr = 0x3C, int sda = 21, int scl = 22)
        : _color(WHITE), _textAlign(TEXT_ALIGN_LEFT), _fontData(nullptr) {
        (void)addr; (void)sda; (void)scl;
        memset(_buffer, 0, sizeof(_buffer));
    }

    void init() {
        _fontData = ArialMT_Plain_10;
    }
    void end() {}
    void resetDisplay() { memset(_buffer, 0, sizeof(_buffer)); }

    void displayOn() {}
    void displayOff() {}

    void clear() {
        memset(_buffer, 0, sizeof(_buffer));
    }

    void display() {
        js_push_framebuffer(_buffer, BUFFER_SIZE);
    }

    void invertDisplay() {}
    void normalDisplay() {}
    void setContrast(uint8_t, uint8_t = 241, uint8_t = 64) {}
    void setBrightness(uint8_t) {}
    void flipScreenVertically() {}
    void mirrorScreen() {}
    void cls() { clear(); }

    // ---- Color ----
    void setColor(OLEDDISPLAY_COLOR color) { _color = color; }

    // ---- Pixel ----
    void setPixel(int16_t x, int16_t y) {
        if (x < 0 || x >= WIDTH || y < 0 || y >= HEIGHT) return;
        int idx = y * WIDTH + x;
        switch (_color) {
            case WHITE:   _buffer[idx] = 1; break;
            case BLACK:   _buffer[idx] = 0; break;
            case INVERSE: _buffer[idx] ^= 1; break;
        }
    }

    // ---- Lines ----
    void drawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1) {
        int16_t steep = abs(y1 - y0) > abs(x1 - x0);
        if (steep) {
            std::swap(x0, y0);
            std::swap(x1, y1);
        }

        if (x0 > x1) {
            std::swap(x0, x1);
            std::swap(y0, y1);
        }

        int16_t dx, dy;
        dx = x1 - x0;
        dy = abs(y1 - y0);

        int16_t err = dx / 2;
        int16_t ystep;

        if (y0 < y1) {
            ystep = 1;
        } else {
            ystep = -1;
        }

        for (; x0 <= x1; x0++) {
            if (steep) {
                setPixel(y0, x0);
            } else {
                setPixel(x0, y0);
            }
            err -= dy;
            if (err < 0) {
                y0 += ystep;
                err += dx;
            }
        }
    }

    void drawHorizontalLine(int16_t x, int16_t y, int16_t length) {
        if (y < 0 || y >= HEIGHT) return;
        for (int16_t i = 0; i < length; i++) setPixel(x + i, y);
    }

    void drawVerticalLine(int16_t x, int16_t y, int16_t length) {
        if (x < 0 || x >= WIDTH) return;
        for (int16_t i = 0; i < length; i++) setPixel(x, y + i);
    }

    // ---- Rectangles ----
    void drawRect(int16_t x, int16_t y, int16_t w, int16_t h) {
        drawHorizontalLine(x, y, w);
        drawHorizontalLine(x, y + h - 1, w);
        drawVerticalLine(x, y, h);
        drawVerticalLine(x + w - 1, y, h);
    }

    void fillRect(int16_t x, int16_t y, int16_t w, int16_t h) {
        for (int16_t j = 0; j < h; j++) {
            drawHorizontalLine(x, y + j, w);
        }
    }

    // ---- Circles (midpoint algorithm) ----
    void drawCircle(int16_t cx, int16_t cy, int16_t r) {
        int16_t x = 0, y = r, d = 3 - 2 * r;
        while (x <= y) {
            setPixel(cx + x, cy + y); setPixel(cx - x, cy + y);
            setPixel(cx + x, cy - y); setPixel(cx - x, cy - y);
            setPixel(cx + y, cy + x); setPixel(cx - y, cy + x);
            setPixel(cx + y, cy - x); setPixel(cx - y, cy - x);
            if (d < 0) { d += 4 * x + 6; }
            else { d += 4 * (x - y) + 10; y--; }
            x++;
        }
    }

    void fillCircle(int16_t cx, int16_t cy, int16_t r) {
        for (int16_t y = -r; y <= r; y++) {
            int16_t dx = (int16_t)sqrtf((float)(r * r - y * y));
            drawHorizontalLine(cx - dx, cy + y, 2 * dx + 1);
        }
    }

    // ---- Triangles ----
    void drawTriangle(int16_t x0, int16_t y0, int16_t x1, int16_t y1, int16_t x2, int16_t y2) {
        drawLine(x0, y0, x1, y1);
        drawLine(x1, y1, x2, y2);
        drawLine(x2, y2, x0, y0);
    }

    void fillTriangle(int16_t x0, int16_t y0, int16_t x1, int16_t y1, int16_t x2, int16_t y2) {
        // Scanline fill
        int16_t minY = std::min({y0, y1, y2});
        int16_t maxY = std::max({y0, y1, y2});
        for (int16_t y = minY; y <= maxY; y++) {
            int16_t xmin = WIDTH, xmax = -1;
            auto edgeScan = [&](int16_t ax, int16_t ay, int16_t bx, int16_t by) {
                if ((ay <= y && by >= y) || (by <= y && ay >= y)) {
                    if (ay == by) { xmin = std::min(xmin, std::min(ax, bx)); xmax = std::max(xmax, std::max(ax, bx)); }
                    else {
                        int16_t xi = ax + (int16_t)((long)(y - ay) * (bx - ax) / (by - ay));
                        xmin = std::min(xmin, xi);
                        xmax = std::max(xmax, xi);
                    }
                }
            };
            edgeScan(x0, y0, x1, y1);
            edgeScan(x1, y1, x2, y2);
            edgeScan(x2, y2, x0, y0);
            if (xmin <= xmax) drawHorizontalLine(xmin, y, xmax - xmin + 1);
        }
    }

    // ---- Progress bar ----
    void drawProgressBar(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint8_t progress) {
        drawRect(x, y, w, h);
        uint16_t fill = (uint16_t)((w - 2) * progress / 100);
        fillRect(x + 1, y + 1, fill, h - 2);
    }

    // ---- Bitmap drawing ----
    void drawFastImage(int16_t xMove, int16_t yMove, int16_t width, int16_t height, const uint8_t* image) {
        drawXbm(xMove, yMove, width, height, image);
    }

    void drawXbm(int16_t xMove, int16_t yMove, int16_t w, int16_t h, const unsigned char* xbm) {
        int16_t bytesInRow = (w + 7) / 8;
        for (int16_t y = 0; y < h; y++) {
            for (int16_t x = 0; x < w; x++) {
                int byteIdx = y * bytesInRow + x / 8;
                if (pgm_read_byte(xbm + byteIdx) & (1 << (x & 7))) {
                    setPixel(xMove + x, yMove + y);
                }
            }
        }
    }

    // ---- Text ----
    void setFont(const uint8_t* fontData) { _fontData = fontData; }

    void setTextAlignment(OLEDDISPLAY_TEXT_ALIGNMENT align) { _textAlign = align; }

    uint16_t drawString(int16_t xMove, int16_t yMove, const String& text) {
        if (!_fontData) return 0;
        const char* str = text.c_str();
        uint16_t textWidth = getStringWidth(str, strlen(str));

        switch (_textAlign) {
            case TEXT_ALIGN_RIGHT:  xMove -= textWidth; break;
            case TEXT_ALIGN_CENTER: xMove -= textWidth / 2; break;
            case TEXT_ALIGN_CENTER_BOTH:
                xMove -= textWidth / 2;
                yMove -= _fontData[1] / 2;
                break;
            default: break;
        }

        uint16_t xCursor = xMove;
        for (int i = 0; str[i]; i++) {
            xCursor += drawChar(xCursor, yMove, str[i]);
        }
        return xCursor - xMove;
    }

    uint16_t drawStringMaxWidth(int16_t x, int16_t y, uint16_t maxWidth, const String& text) {
        (void)maxWidth;
        return drawString(x, y, text);
    }

    uint16_t getStringWidth(const char* text, uint16_t length, bool utf8 = false) const {
        (void)utf8;
        if (!_fontData) return 0;
        uint16_t w = 0;
        uint8_t firstChar = pgm_read_byte(_fontData + 2);
        uint8_t charCount = pgm_read_byte(_fontData + 3);
        for (uint16_t i = 0; i < length && text[i]; i++) {
            uint8_t c = (uint8_t)text[i];
            if (c < firstChar || c >= firstChar + charCount) { w += pgm_read_byte(_fontData) / 2; continue; }
            uint8_t ci = c - firstChar;
            uint8_t charWidth = pgm_read_byte(_fontData + 4 + ci * 4 + 3);
            w += charWidth;
        }
        return w;
    }

    uint16_t getStringWidth(const String& text) const {
        return getStringWidth(text.c_str(), text.length());
    }

    // Direct buffer access for the WASM bridge
    const uint8_t* getBuffer() const { return _buffer; }
    int getBufferSize() const { return BUFFER_SIZE; }

private:
    uint8_t _buffer[WIDTH * HEIGHT]; // flat pixel buffer, 1 byte per pixel (0 or 1)
    OLEDDISPLAY_COLOR _color;
    OLEDDISPLAY_TEXT_ALIGNMENT _textAlign;
    const uint8_t* _fontData;

    // ThingPulse font format renderer
    uint16_t drawChar(int16_t xMove, int16_t yMove, uint8_t c) {
        if (!_fontData) return 0;
        uint8_t fontWidth = pgm_read_byte(_fontData);
        uint8_t fontHeight = pgm_read_byte(_fontData + 1);
        uint8_t firstChar = pgm_read_byte(_fontData + 2);
        uint8_t charCount = pgm_read_byte(_fontData + 3);

        (void)fontWidth;

        if (c < firstChar || c >= firstChar + charCount) return fontWidth / 2;

        uint8_t ci = c - firstChar;
        const uint8_t* jumpEntry = _fontData + 4 + ci * 4;

        uint16_t offset = (pgm_read_byte(jumpEntry) << 8) | pgm_read_byte(jumpEntry + 1);
        uint8_t  size   = pgm_read_byte(jumpEntry + 2);
        uint8_t  charWidth = pgm_read_byte(jumpEntry + 3);

        if (offset == 0xFFFF || size == 0) return charWidth;

        const uint8_t* data = _fontData + 4 + charCount * 4 + offset;
        uint8_t colBytes = (fontHeight + 7) / 8;

        for (uint8_t col = 0; col < charWidth && col * colBytes < size; col++) {
            for (uint8_t row = 0; row < fontHeight; row++) {
                uint8_t byteIdx = col * colBytes + row / 8;
                if (byteIdx >= size) break;
                if (pgm_read_byte(data + byteIdx) & (1 << (row & 7))) {
                    setPixel(xMove + col, yMove + row);
                }
            }
        }

        return charWidth;
    }
};

#endif // WASM_SSD1306WIRE_H
