// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#ifndef STARBURST_H
#define STARBURST_H
#include "DisplayProxy.h"
#include "ButtonManager.h"

class Starburst {
public:
    Starburst(ButtonManager& btnMgr);
    void begin();
    void update();
    void end();
private:
    ButtonManager& buttonManager;
    DisplayProxy& display;
    static Starburst* instance;

    // A burst is a single starburst effect on screen
    // style 0=Up (radial lines), 1=Down (concentric rings),
    // style 2=Left (diagonal cross), 3=Right (dot spray)
    // style 4=Enter (filled star polygon)
    struct Burst {
        bool active;
        int cx, cy;          // center
        int style;           // pattern type
        int variant;         // random within style
        unsigned long birth; // ms when created
        int lifetime;        // ms until gone
        int size;            // base size
        int seed;            // randomness seed
        int hue;             // 0-4095 color this burst lends to its LED
    };

    static const int MAX_BURSTS = 40;
    Burst bursts[MAX_BURSTS];
    int nextBurst;
    unsigned long lastFrame;

    // Each of the 3 front LEDs follows one burst and shows its color, fading as
    // that burst dies; -1 means the LED is dark. The back LED is never used.
    int ledBurst[3];
    int ledRR;            // round-robin cursor used when LED_SNAP_TO_NEWEST

    // LED behavior (code-only switches, no UI):
    //   LED_SNAP_TO_NEWEST true  -> each new burst grabs the next LED, so the 3
    //                               LEDs always show the 3 most-recent bursts.
    //                       false -> an LED latches onto a burst and holds it
    //                               until that burst dies (then takes a new one).
    //   LED_MAX_BRIGHT is the peak LED brightness (0-255) -- kept below full so
    //   the LEDs are a touch dimmer than the display.
    static const bool LED_SNAP_TO_NEWEST = true;
    static const int  LED_MAX_BRIGHT     = 110;

    // Run mode chosen on a small menu when the app starts.
    // MANUAL: bursts only happen when buttons are pressed (original behavior).
    // AUTO:   bursts also fire on their own at a varying cadence so the app
    //         runs itself unattended (demo booth). Buttons still work in AUTO.
    enum AppState { STATE_MODE_SELECT, STATE_RUNNING };
    enum RunMode  { MODE_MANUAL, MODE_AUTO };
    AppState appState;
    RunMode  runMode;
    int menuIndex;                // highlighted option on the mode menu (0=Manual, 1=Auto)
    unsigned long nextAutoSpawn;  // ms timestamp of the next automatic burst (AUTO mode)

    // Auto-mode cadence: each automatic burst is followed by a gap of
    // AUTO_MIN_MS plus a random 0..AUTO_VAR_MS, so the timing stays lively
    // and never feels metronomic.
    static const int AUTO_MIN_MS = 250;
    static const int AUTO_VAR_MS = 900;

    // Gravity (code-only switch, no UI). Each burst's sparks drift gently
    // downward as they age and twinkle out, like real firework embers. Set
    // GRAVITY_ENABLED to false for a pure expanding-and-twinkling cloud with no
    // fall. GRAVITY_STRENGTH is roughly the gentle drift (pixels) by end of life.
    static const bool GRAVITY_ENABLED  = true;
    static const int  GRAVITY_STRENGTH = 22;

    void spawnBurst(int style);
    void drawBurst(const Burst& b, unsigned long now);
    void updateLeds(unsigned long now);
    void drawModeSelect();
    void scheduleNextAutoSpawn(unsigned long now);

    void registerButtonCallbacks();
    void unregisterButtonCallbacks();
    static void onButtonUp(const ButtonEvent& event);
    static void onButtonDown(const ButtonEvent& event);
    static void onButtonLeft(const ButtonEvent& event);
    static void onButtonRight(const ButtonEvent& event);
    static void onButtonEnter(const ButtonEvent& event);
    static void onButtonBack(const ButtonEvent& event);
};

extern Starburst starburstApp;
#endif