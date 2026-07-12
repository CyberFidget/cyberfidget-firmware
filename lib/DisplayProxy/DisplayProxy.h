// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#ifndef DISPLAY_PROXY_H
#define DISPLAY_PROXY_H

#include <Arduino.h>
#include <SSD1306Wire.h>

// Enum to define overlay modes
enum class OverlayMode {
    OVERLAY_OFF, // No overlay at all
    OVERLAY_ON   // Show overlay (battery icons, etc.)
};

class DisplayProxy {
public:
    // Pass the real display in constructor
    DisplayProxy(SSD1306Wire& realDisplay);

    // --- Display Control ---
    void init();
    void end();
    void resetDisplay();
    //void reconnect(); // Not included in this the SSD1306Wire.h library
    void displayOn();
    void displayOff();
    void clear();
    void display();
    void invertDisplay();
    void normalDisplay();
    void setContrast(uint8_t contrast, uint8_t precharge = 241, uint8_t comdetect = 64);
    void setBrightness(uint8_t brightness);
    void flipScreenVertically();
    void mirrorScreen();

    // --- Pixel Drawing ---
    void setColor(OLEDDISPLAY_COLOR color);
    void setPixel(int16_t x, int16_t y);
    void drawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1);
    void drawRect(int16_t x, int16_t y, int16_t width, int16_t height);
    void fillRect(int16_t x, int16_t y, int16_t width, int16_t height);
    void drawCircle(int16_t x, int16_t y, int16_t radius);
    void fillCircle(int16_t x, int16_t y, int16_t radius);
    void drawTriangle(int16_t x0, int16_t y0, int16_t x1, int16_t y1, int16_t x2, int16_t y2);
    void fillTriangle(int16_t x0, int16_t y0, int16_t x1, int16_t y1, int16_t x2, int16_t y2);
    void drawHorizontalLine(int16_t x, int16_t y, int16_t length);
    void drawVerticalLine(int16_t x, int16_t y, int16_t length);
    void drawProgressBar(uint16_t x, uint16_t y, uint16_t width, uint16_t height, uint8_t progress);
    void drawFastImage(int16_t x, int16_t y, int16_t width, int16_t height, const uint8_t *image);
    void drawXbm(int16_t x, int16_t y, int16_t w, int16_t h, const unsigned char* data);

    // --- Text Operations ---
    uint16_t drawString(int16_t x, int16_t y, const String &text);
    uint16_t drawStringMaxWidth(int16_t x, int16_t y, uint16_t maxLineWidth, const String &text);
    uint16_t getStringWidth(const char* text, uint16_t length, bool utf8 = false);
    uint16_t getStringWidth(const String &text);
    void setTextAlignment(OLEDDISPLAY_TEXT_ALIGNMENT alignment);
    void setFont(const uint8_t* fontData);

    // --- Arduino Print functionality ---
    void cls();

    // --- Special / Custom functions ---
    void setPixelStatusBars(int16_t x, int16_t y);
    void setOverlayMode(OverlayMode mode);
    OverlayMode getOverlayMode() const;

    // --- T-191 serial tunnel: read-only access to the 1bpp framebuffer
    // (128x64 = 1024 bytes, column-major pages) so `screencap` can ship
    // exactly what the panel shows. The pointer is the library's live
    // buffer; the serial verb copies it out immediately under the loop
    // task, never handing the live pointer across contexts. ---
    const uint8_t* frameBuffer() const;
    static constexpr int kFrameBufferBytes = 1024;  // 128x64 / 8

private:
    SSD1306Wire& m_display;    // Reference to the real display
    OverlayMode m_overlayMode; // Overlay mode setting

    // Example overlay drawing function (e.g., battery, WiFi icons, etc.)
    void drawOverlayElements();
};

#endif // DISPLAY_PROXY_H
