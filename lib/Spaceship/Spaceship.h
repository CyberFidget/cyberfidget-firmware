// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#ifndef SPACESHIP_H
#define SPACESHIP_H

#include "DisplayProxy.h"
#include "ButtonManager.h"

// Spaceship is a Star Fox-style rail shooter that also doubles as a passive
// desk screensaver. A small chooser on launch picks one of three modes:
//   Play Now    - jump straight into the asteroid shooter
//   Screensaver - the ship flies forever through space (no interaction)
//   Auto        - passive flight until the accelerometer feels you pick the
//                 device up, which drops you into the shooter; put it back
//                 down and autopilot returns it to the screensaver
class Spaceship {
public:
    Spaceship(ButtonManager& btnMgr);
    void begin();
    void update();
    void end();

private:
    ButtonManager& buttonManager;
    DisplayProxy& display;
    static Spaceship* instance;

    // ---- Fire behavior (compile-time switch; feel is best judged by play) ----
    //   FIRE_HOLD_REPEAT - hold the circle button to auto-repeat (default)
    //   FIRE_AUTO        - the ship fires on its own; you only steer to aim
    //   FIRE_SINGLE      - one volley per circle press, no auto-repeat
    // Two-thumb ergonomics may favour FIRE_AUTO on real hardware -- swap and
    // rebuild to compare (see preview.html for the local emulator loop).
    enum FireMode : uint8_t { FIRE_HOLD_REPEAT, FIRE_AUTO, FIRE_SINGLE };
    static const FireMode kFireMode = FIRE_HOLD_REPEAT;

    // ---- App state machine ----
    enum AppState : uint8_t {
        STATE_MODE_SELECT,
        STATE_SCREENSAVER,
        STATE_PLAYING,
        STATE_WARNING,     // Auto only: "OBJECT COLLISION" placard before play
        STATE_AUTOPILOT    // Auto only: "AUTOPILOT ENGAGED" placard before idle
    };

    // ---- Buttons ----
    void registerButtonCallbacks();
    void unregisterButtonCallbacks();
    static void onButtonBack(const ButtonEvent& event);
    static void onButtonLeft(const ButtonEvent& event);
    static void onButtonRight(const ButtonEvent& event);
    static void onButtonUp(const ButtonEvent& event);
    static void onButtonDown(const ButtonEvent& event);
    static void onButtonFire(const ButtonEvent& event);

    // ---- Background starfield (kept from the original screensaver) ----
    struct Star { float x, y, z, vx; uint8_t kind; float life; };
    static const int kStarCount = 80;
    Star stars[kStarCount];
    void resetStar(int i, bool forceShooting, bool randomDepth);
    void stepStars(float dt);
    void drawStars();

    // ---- Asteroids (world x + depth z; a fixed world-y makes them descend) ----
    struct Asteroid {
        bool active;
        float wx;        // world x (lateral)
        float z;         // depth -- shrinks toward the ship
        float drift;     // gentle lateral drift
        uint8_t tier;    // 0 small, 1 medium, 2 large
        uint8_t hp;      // shots remaining (tier + 1)
        uint16_t seed;   // jagged-outline seed
        int16_t sx, sy, r;  // cached screen projection for this frame
    };
    static const int kMaxAsteroids = 12;
    Asteroid asteroids[kMaxAsteroids];

    // ---- Lasers (screen-space bolts travelling up the display) ----
    struct Laser { bool active; float x, y, vy; };
    static const int kMaxLasers = 16;
    Laser lasers[kMaxLasers];

    // ---- Explosions ----
    struct Boom { bool active; int16_t x, y; unsigned long birth; uint8_t maxR; bool big; uint16_t seed; };
    static const int kMaxBooms = 8;
    Boom booms[kMaxBooms];

    // ---- Scenery (screensaver eye-candy: drifting wireframe worlds) ----
    struct Scenery { bool active; float x, y, z, vx; uint8_t kind; };
    static const int kMaxScenery = 2;
    Scenery scenery[kMaxScenery];

    // ---- State ----
    AppState state;
    bool autoMode;             // launched via the Auto chooser entry
    uint8_t menuIndex;         // 0 Play Now, 1 Screensaver, 2 Auto

    // ship
    float shipX;               // -1..1 lateral position
    float shipVel;
    float bank;                // visual roll
    bool turnLeftHeld, turnRightHeld;
    bool invuln;
    unsigned long invulnUntil;

    // throttle / tunnel speed
    float speed, speedTarget;

    // shooting
    bool fireHeld;
    unsigned long lastFireMs;

    // scoring / progression
    uint32_t score;
    uint16_t deaths;
    uint8_t laserLevel;        // 0 single, 1 twin, 2 double-shot
    unsigned long runStartMs;
    unsigned long lastSpawnMs;
    unsigned long spawnGap;

    // placard / accel timing
    unsigned long stateEnterMs;
    unsigned long lastSceneryMs;
    float accelBaseX, accelBaseY, accelBaseZ;  // slow baseline of the rest pose
    float motion;              // smoothed deviation of accel from its baseline
    unsigned long lastActiveMs;

    // frame timing
    unsigned long lastMs;
    unsigned long lastShootingMs;

    // ---- per-state update ----
    void enterState(AppState s);
    void updateModeSelect();
    void updateScreensaver(float dt);
    void updatePlaying(float dt);
    void updatePlacard(float dt);

    // ---- gameplay helpers ----
    void resetGame();
    void spawnAsteroid();
    void fireLasers();
    void moveAsteroids(float dt);
    void moveLasers(float dt);
    void collide();
    void updateUpgrades();
    void addBoom(int16_t x, int16_t y, uint8_t maxR, bool big);
    void updateAccelMotion(float dt);
    void spawnScenery();
    void moveScenery(float dt);
    void setEngineLeds(bool flash);

    // ---- drawing ----
    void drawScenery();
    void drawAsteroids();
    void drawLasers();
    void drawBooms(unsigned long now);
    void drawShip(int16_t cx, int16_t cy, float bankAmt, bool blink);
    void drawHud();
    void drawModeSelect();
    void drawWarningPlacard();
    void drawAutopilotPlacard();
    void drawSkull(int16_t x, int16_t y);
    void drawCrystal(int16_t x, int16_t y);

    static float clampf(float v, float lo, float hi);
    int16_t shipScreenX() const;
    int16_t shipScreenY() const;
};

extern Spaceship spaceshipApp;

#endif
