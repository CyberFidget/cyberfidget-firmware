// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#include "DisplayProxy.h"
//#include "BatteryManager.h"  // Uncomment if battery info is needed
//#include "icons.h"           // Uncomment if you want to use icon data

// Constructor: store reference to the actual display
DisplayProxy::DisplayProxy(SSD1306Wire& realDisplay)
: m_display(realDisplay)
, m_overlayMode(OverlayMode::OVERLAY_OFF) // Default to overlay enabled
{}

// --- Display Control ---

void DisplayProxy::init() {
    m_display.init();
}

void DisplayProxy::end() {
    m_display.end();
}

void DisplayProxy::resetDisplay() {
    m_display.resetDisplay();
}

// void DisplayProxy::reconnect() { // Not included in this SSD1306Wire.h library
//     m_display.reconnect();
// }

void DisplayProxy::displayOn() {
    m_display.displayOn();
}

void DisplayProxy::displayOff() {
    m_display.displayOff();
}

void DisplayProxy::clear() {
    m_display.clear();
}

void DisplayProxy::display() {
    // Final chance to add custom overlay elements
    if (m_overlayMode == OverlayMode::OVERLAY_ON) {
        drawOverlayElements();
    }
    m_display.display();
}

void DisplayProxy::invertDisplay() {
    m_display.invertDisplay();
}

void DisplayProxy::normalDisplay() {
    m_display.normalDisplay();
}

void DisplayProxy::setContrast(uint8_t contrast, uint8_t precharge, uint8_t comdetect) {
    m_display.setContrast(contrast, precharge, comdetect);
}

void DisplayProxy::setBrightness(uint8_t brightness) {
    m_display.setBrightness(brightness);
}

void DisplayProxy::flipScreenVertically() {
    m_display.flipScreenVertically();
}

void DisplayProxy::mirrorScreen() {
    m_display.mirrorScreen();
}

// --- Pixel Drawing ---

void DisplayProxy::setColor(OLEDDISPLAY_COLOR color) {
    m_display.setColor(color);
}

void DisplayProxy::setPixel(int16_t x, int16_t y) {
    // Example: intercept pixels drawn in the top 10 rows when overlay is enabled
    if (m_overlayMode == OverlayMode::OVERLAY_ON && y < 10) {
        // Optionally, ignore or customize the pixel drawing in this area
        return;
    }
    m_display.setPixel(x, y);
}

void DisplayProxy::drawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1) {
    m_display.drawLine(x0, y0, x1, y1);
}

void DisplayProxy::drawRect(int16_t x, int16_t y, int16_t width, int16_t height) {
    m_display.drawRect(x, y, width, height);
}

void DisplayProxy::fillRect(int16_t x, int16_t y, int16_t width, int16_t height) {
    m_display.fillRect(x, y, width, height);
}

void DisplayProxy::drawCircle(int16_t x, int16_t y, int16_t radius) {
    m_display.drawCircle(x, y, radius);
}

void DisplayProxy::fillCircle(int16_t x, int16_t y, int16_t radius) {
    m_display.fillCircle(x, y, radius);
}

void DisplayProxy::drawTriangle(int16_t x0, int16_t y0, int16_t x1, int16_t y1, int16_t x2, int16_t y2) {
    m_display.drawTriangle(x0, y0, x1, y1, x2, y2);
}

void DisplayProxy::fillTriangle(int16_t x0, int16_t y0, int16_t x1, int16_t y1, int16_t x2, int16_t y2) {
    m_display.fillTriangle(x0, y0, x1, y1, x2, y2);
}

void DisplayProxy::drawHorizontalLine(int16_t x, int16_t y, int16_t length) {
    m_display.drawHorizontalLine(x, y, length);
}

void DisplayProxy::drawVerticalLine(int16_t x, int16_t y, int16_t length) {
    m_display.drawVerticalLine(x, y, length);
}

void DisplayProxy::drawProgressBar(uint16_t x, uint16_t y, uint16_t width, uint16_t height, uint8_t progress) {
    m_display.drawProgressBar(x, y, width, height, progress);
}

void DisplayProxy::drawFastImage(int16_t x, int16_t y, int16_t width, int16_t height, const uint8_t *image) {
    m_display.drawFastImage(x, y, width, height, image);
}

void DisplayProxy::drawXbm(int16_t x, int16_t y, int16_t w, int16_t h, const unsigned char* data) {
    m_display.drawXbm(x, y, w, h, data);
}

// --- Text Operations ---

uint16_t DisplayProxy::drawString(int16_t x, int16_t y, const String &text) {
    return m_display.drawString(x, y, text);
}

uint16_t DisplayProxy::drawStringMaxWidth(int16_t x, int16_t y, uint16_t maxLineWidth, const String &text) {
    return m_display.drawStringMaxWidth(x, y, maxLineWidth, text);
}

uint16_t DisplayProxy::getStringWidth(const char* text, uint16_t length, bool utf8) {
    return m_display.getStringWidth(text, length, utf8);
}

uint16_t DisplayProxy::getStringWidth(const String &text) {
    return m_display.getStringWidth(text);
}

void DisplayProxy::setTextAlignment(OLEDDISPLAY_TEXT_ALIGNMENT alignment) {
    m_display.setTextAlignment(alignment);
}

void DisplayProxy::setFont(const uint8_t* fontData) {
    m_display.setFont(fontData);
}

// --- Arduino Print functionality ---

void DisplayProxy::cls() {
    m_display.cls();
}

// --- Special / Custom functions ---

void DisplayProxy::setPixelStatusBars(int16_t x, int16_t y) {
    // For instance, allow drawing in the top row despite overlay intercepts
    if (m_overlayMode == OverlayMode::OVERLAY_ON && y < 10) {
        m_display.setPixel(x, y);
    }
}

void DisplayProxy::setOverlayMode(OverlayMode mode) {
    m_overlayMode = mode;
}

OverlayMode DisplayProxy::getOverlayMode() const {
    return m_overlayMode;
}

// --- Overlay drawing (example) ---

void DisplayProxy::drawOverlayElements() {
    // Example: overlay battery or WiFi icons at the top of the display.
    // int batteryPercent = BatteryManager::getInstance().getBatteryPercent();
    // m_display.drawXbm(112, 0, battery_width, battery_height, battery_bits);
    // Add additional overlay drawing logic as desired.
}

// --- T-191 serial tunnel: framebuffer read access ---
const uint8_t* DisplayProxy::frameBuffer() const {
    // The exact 1bpp bitmap last pushed to the panel via display(), read-only.
    // The real ThingPulse OLED base class exposes it as a public `buffer`
    // member; the WASM shim (wasm/shims/SSD1306Wire.h) has a private _buffer
    // reached via getBuffer(). They must stay in step (REQ-066).
#ifdef __EMSCRIPTEN__
    return m_display.getBuffer();
#else
    return m_display.buffer;
#endif
}
