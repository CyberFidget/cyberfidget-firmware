// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#ifndef MATRIX_SCREENSAVER_H
#define MATRIX_SCREENSAVER_H

#include <Arduino.h>
#include "DisplayProxy.h"
#include "ButtonManager.h" // For returning back to menu
#include "MenuManager.h" // For returning back to menu

class MatrixScreensaver {
public:
    // Adjust to match your display
    static const int SCREEN_HEIGHT = 64;
    static const int FONT_HEIGHT   = 8;  // 8 pixels tall
    static const int FONT_WIDTH    = 8;  // or 8, depending on your font
    static const int NUM_ROWS      = SCREEN_HEIGHT / FONT_HEIGHT; // e.g. 8
    static const int NUM_COLUMNS   = 16; // how many columns horizontally

    static const char ALIEN_CHARS[16];

    MatrixScreensaver(ButtonManager& btnMgr);

    void begin();  // Initialize state
    void update(); // Step state machine
    void draw();   // Render to the display
    void end(); // AppManager integration

private:
    // AppManager Integration
    ButtonManager& buttonManager;
    void registerButtonCallbacks();
    void unregisterButtonCallbacks();
    static void buttonPressedCallback(const ButtonEvent& event);
    static void onButtonBackPressed(const ButtonEvent& event);
    static void onButtonSelectPressed(const ButtonEvent& event);
    static MatrixScreensaver* instance; // To allow static callbacks

    DisplayProxy& display;

    // We do small stepping every frameInterval ms
    unsigned long lastUpdateTime;
    unsigned long frameInterval;

    // The time to increase/decrease each row by 1 pixel line
    // e.g. if rowPixelDelay=30, we take 30ms to move from litPixels=0 to litPixels=1
    // Then after 8 * 30=240ms, that row is fully on.
    unsigned long rowPixelDelay;

    // Column-level states
    enum ColumnState {
        OFF,
        TURNING_ON,
        ON,
        TURNING_OFF
    };

    // Info about each row
    struct RowInfo {
        char ch;
        uint8_t litPixels;      // 0..FONT_HEIGHT (8)
        uint32_t nextChange;    // next time to flicker
    };

    struct Column {
        ColumnState state;
        int currentRow;        // which row are we currently turning on/off?
        unsigned long nextStep; // when we increment/decrement row's litPixels

        unsigned long onDuration;   // how long we stay fully on
        unsigned long offDuration;  // how long we stay fully off
        unsigned long nextStateChangeTime; // when we transition from ON->OFF or OFF->ON

        RowInfo rows[NUM_ROWS];
    };

    Column columns[NUM_COLUMNS];

    // -------------- Internal Helpers ------------------
    void startTurningOn(Column &col, unsigned long now);
    void startTurningOff(Column &col, unsigned long now);

    // randomize entire column's character set
    void randomizeColumnSymbols(Column &col);

    // pick random char
    char randomAlienChar();

    // partial draw
    void drawCharPartial(int x, int y, char c, int litPixels, bool fill);

    // The main raw font data / look-up
    // If you have it in a separate .cpp, you can extern it here
    // or embed it directly. Example signature:
    // static const uint8_t MY_FONT[];  // stored in PROGMEM

    // findGlyphOffset(c) -> returns the index in MY_FONT for char c
    uint16_t findGlyphOffset(char c);
};

extern MatrixScreensaver matrixScreensaver;

#endif
