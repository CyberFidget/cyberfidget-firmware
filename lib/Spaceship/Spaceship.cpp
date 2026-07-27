// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#include "Spaceship.h"
#include "globals.h"
#include "HAL.h"
#include "MenuManager.h"
#include "RGBController.h"
#include "generated/ship.h"
#include <math.h>

Spaceship spaceshipApp(HAL::buttonManager());
Spaceship* Spaceship::instance = nullptr;

// ---- Scene / projection constants (shared by stars, asteroids, scenery) ----
static const int16_t VP_X = 64;     // vanishing point x (screen)
static const int16_t VP_Y = 30;     // vanishing point y (play area sits below)
static const float   X_SCALE = 48.0f;
static const float   Y_SCALE = 48.0f;
static const float   AST_WORLD_Y = 0.55f;  // fixed world y -> asteroids descend toward the ship

// ---- Ship geometry ----
static const int16_t SHIP_Y = 50;          // screen row the ship rides on
static const float   SHIP_HALF = 42.0f;    // px from centre at shipX = +/-1
// Q8.8 model scale; adjust this one value for pixel-size matching. At 112 the
// drawn wingspan matches the pre-model ship exactly (35 px at zero bank).
// The host test mirrors this value - change both together.
static const uint16_t SHIP_MODEL_SCALE_Q8 = 112;

// ---- Tunables (asteroid sizes, lasers, difficulty, scoring) ----
static const int16_t TIER_R[3]   = { 4, 6, 9 };   // base screen radius per tier
static const float   LASER_SPEED = 150.0f;        // px/s upward
static const uint16_t FIRE_CD[3] = { 300, 250, 150 };  // ms cooldown per laser level
static const uint32_t UP1_SCORE = 15;  // -> twin lasers
static const uint32_t UP2_SCORE = 40;  // -> double shot

// ---- Auto-mode accelerometer thresholds (LIKELY NEED ON-HARDWARE TUNING) ----
// `motion` is a smoothed deviation of the live accel vector from a slow-moving
// baseline of the rest pose. Sitting still -> baseline catches up -> ~0. Picking
// the device up rotates the gravity vector away from the baseline -> a large,
// sustained deviation. Thresholds assume the LIS2DH12 globals are ~milli-g
// (1g ~= 1000), matching the emulator's accel feed; verify/retune on hardware.
static const float        PICKUP_MOTION = 380.0f;   // deviation that means "picked up"
static const float        IDLE_MOTION   = 150.0f;   // below this counts as "at rest"
static const unsigned long IDLE_MS       = 6000;
static const unsigned long WARNING_MS    = 1700;
static const unsigned long AUTOPILOT_MS  = 1700;

Spaceship::Spaceship(ButtonManager& btnMgr) :
    buttonManager(btnMgr),
    display(HAL::displayProxy()),
    state(STATE_MODE_SELECT),
    autoMode(false),
    menuIndex(0),
    shipX(0.0f), shipVel(0.0f), bank(0.0f),
    turnLeftHeld(false), turnRightHeld(false),
    invuln(false), invulnUntil(0),
    speed(6.0f), speedTarget(6.0f),
    fireHeld(false), lastFireMs(0),
    score(0), deaths(0), laserLevel(0),
    runStartMs(0), lastSpawnMs(0), spawnGap(1400),
    stateEnterMs(0), lastSceneryMs(0),
    accelBaseX(0.0f), accelBaseY(0.0f), accelBaseZ(0.0f), motion(0.0f), lastActiveMs(0),
    lastMs(0), lastShootingMs(0) {
    instance = this;
}

float Spaceship::clampf(float v, float lo, float hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

int16_t Spaceship::shipScreenX() const {
    return (int16_t)(VP_X + clampf(shipX, -1.0f, 1.0f) * SHIP_HALF);
}
int16_t Spaceship::shipScreenY() const { return SHIP_Y; }

void Spaceship::begin() {
    registerButtonCallbacks();
    lastMs = millis();
    lastShootingMs = millis();

    for (int i = 0; i < kMaxAsteroids; ++i) asteroids[i].active = false;
    for (int i = 0; i < kMaxLasers; ++i) lasers[i].active = false;
    for (int i = 0; i < kMaxBooms; ++i) booms[i].active = false;
    for (int i = 0; i < kMaxScenery; ++i) scenery[i].active = false;
    for (int i = 0; i < kStarCount; ++i) resetStar(i, false, true);

    accelBaseX = accelX; accelBaseY = accelY; accelBaseZ = accelZ;
    motion = 0.0f;

    // Always open on the chooser so the operator picks a mode.
    state = STATE_MODE_SELECT;
    autoMode = false;
    menuIndex = 0;
    setColorsOff();
}

void Spaceship::end() {
    setColorsOff();
    unregisterButtonCallbacks();
}

// ===========================================================================
//  Buttons
// ===========================================================================
void Spaceship::registerButtonCallbacks() {
    buttonManager.registerCallback(button_BottomLeftIndex, onButtonBack);
    buttonManager.registerCallback(button_LeftIndex, onButtonLeft);
    buttonManager.registerCallback(button_RightIndex, onButtonRight);
    buttonManager.registerCallback(button_UpIndex, onButtonUp);
    buttonManager.registerCallback(button_DownIndex, onButtonDown);
    buttonManager.registerCallback(button_BottomRightIndex, onButtonFire);
}

void Spaceship::unregisterButtonCallbacks() {
    buttonManager.unregisterCallback(button_BottomLeftIndex);
    buttonManager.unregisterCallback(button_LeftIndex);
    buttonManager.unregisterCallback(button_RightIndex);
    buttonManager.unregisterCallback(button_UpIndex);
    buttonManager.unregisterCallback(button_DownIndex);
    buttonManager.unregisterCallback(button_BottomRightIndex);
}

void Spaceship::onButtonBack(const ButtonEvent& event) {
    if (event.eventType == ButtonEvent_Released) {
        instance->end();
        MenuManager::instance().returnToMenu();
    }
}

void Spaceship::onButtonLeft(const ButtonEvent& event) {
    if (event.eventType == ButtonEvent_Pressed)  instance->turnLeftHeld = true;
    else if (event.eventType == ButtonEvent_Released) instance->turnLeftHeld = false;
}

void Spaceship::onButtonRight(const ButtonEvent& event) {
    if (event.eventType == ButtonEvent_Pressed)  instance->turnRightHeld = true;
    else if (event.eventType == ButtonEvent_Released) instance->turnRightHeld = false;
}

// Up/Down navigate the chooser; unused in play.
void Spaceship::onButtonUp(const ButtonEvent& event) {
    if (event.eventType != ButtonEvent_Pressed) return;
    if (instance->state == STATE_MODE_SELECT && instance->menuIndex > 0)
        instance->menuIndex--;
}
void Spaceship::onButtonDown(const ButtonEvent& event) {
    if (event.eventType != ButtonEvent_Pressed) return;
    if (instance->state == STATE_MODE_SELECT && instance->menuIndex < 2)
        instance->menuIndex++;
}

// Circle button: confirm on the chooser, fire in play, return-to-chooser from
// the screensaver. Also tracks held state for FIRE_HOLD_REPEAT.
void Spaceship::onButtonFire(const ButtonEvent& event) {
    if (event.eventType == ButtonEvent_Pressed) {
        instance->fireHeld = true;
        switch (instance->state) {
            case STATE_MODE_SELECT:
                if (instance->menuIndex == 0)      { instance->autoMode = false; instance->enterState(STATE_PLAYING); }
                else if (instance->menuIndex == 1) { instance->autoMode = false; instance->enterState(STATE_SCREENSAVER); }
                else                                { instance->autoMode = true;  instance->enterState(STATE_SCREENSAVER); }
                break;
            case STATE_SCREENSAVER:
                instance->enterState(STATE_MODE_SELECT);
                break;
            case STATE_PLAYING:
                if (kFireMode == FIRE_SINGLE) instance->fireLasers();
                break;
            default: break;
        }
    } else if (event.eventType == ButtonEvent_Released) {
        instance->fireHeld = false;
    }
}

// ===========================================================================
//  State transitions
// ===========================================================================
void Spaceship::enterState(AppState s) {
    state = s;
    stateEnterMs = millis();
    if (s == STATE_PLAYING) {
        resetGame();
    } else if (s == STATE_SCREENSAVER) {
        lastSceneryMs = millis();
        // arm the baseline to the current pose so a fresh pickup is detectable
        accelBaseX = accelX; accelBaseY = accelY; accelBaseZ = accelZ;
        motion = 0.0f;
        lastActiveMs = millis();
    }
}

void Spaceship::resetGame() {
    for (int i = 0; i < kMaxAsteroids; ++i) asteroids[i].active = false;
    for (int i = 0; i < kMaxLasers; ++i) lasers[i].active = false;
    for (int i = 0; i < kMaxBooms; ++i) booms[i].active = false;
    score = 0;
    deaths = 0;
    laserLevel = 0;
    shipX = 0.0f; shipVel = 0.0f; bank = 0.0f;
    invuln = true; invulnUntil = millis() + 1200;  // brief grace on spawn
    runStartMs = millis();
    lastSpawnMs = millis();
    spawnGap = 1400;
    lastActiveMs = millis();
}

// ===========================================================================
//  Main update -- dispatches on state
// ===========================================================================
void Spaceship::update() {
    unsigned long now = millis();
    float dt = (float)(now - lastMs) / 1000.0f;
    lastMs = now;
    if (dt < 0.0f) dt = 0.0f;
    if (dt > 0.05f) dt = 0.05f;

    switch (state) {
        case STATE_MODE_SELECT: updateModeSelect(); break;
        case STATE_SCREENSAVER: updateScreensaver(dt); break;
        case STATE_PLAYING:     updatePlaying(dt); break;
        case STATE_WARNING:
        case STATE_AUTOPILOT:   updatePlacard(dt); break;
    }
}

// ===========================================================================
//  Mode chooser
// ===========================================================================
void Spaceship::updateModeSelect() {
    // gentle drifting starfield behind the menu
    for (int i = 0; i < kStarCount; ++i) {
        stars[i].z -= 0.8f * 0.016f;
        if (stars[i].z < 0.15f) resetStar(i, false, false);
    }
    display.clear();
    drawStars();
    drawModeSelect();
    display.display();
    setColorsOff();
}

void Spaceship::drawModeSelect() {
    display.setColor(WHITE);
    display.setTextAlignment(TEXT_ALIGN_CENTER);
    display.setFont(ArialMT_Plain_16);
    display.drawString(64, 0, "Spaceship");

    display.setFont(ArialMT_Plain_10);
    const char* opts[3] = { "Play Now", "Screensaver", "Auto" };
    for (int i = 0; i < 3; ++i) {
        int y = 22 + i * 14;
        if (i == menuIndex) {
            display.fillRect(20, y - 1, 88, 13);
            display.setColor(BLACK);
            display.drawString(64, y, opts[i]);
            display.setColor(WHITE);
        } else {
            display.drawString(64, y, opts[i]);
        }
    }
}

// ===========================================================================
//  Screensaver (passive flight; Auto mode watches the accelerometer here)
// ===========================================================================
void Spaceship::updateScreensaver(float dt) {
    unsigned long now = millis();

    // throttle still sets the cruise speed so it feels alive
    float sliderNorm = (float)sliderPosition_Percentage_Inverted_Filtered / 100.0f;
    speedTarget = 2.0f + sliderNorm * 7.0f;
    speed += (speedTarget - speed) * (1.0f - expf(-3.0f * dt));

    // ship gently weaves on its own
    bank = 0.45f * sinf((float)now * 0.0011f);
    shipX = 0.6f * sinf((float)now * 0.0007f);

    stepStars(dt);
    moveScenery(dt);
    if (now - lastSceneryMs > (unsigned long)(4000 + (int)random(0, 4000))) {
        lastSceneryMs = now;
        spawnScenery();
    }

    if (autoMode) {
        updateAccelMotion(dt);
        if (motion > PICKUP_MOTION) enterState(STATE_WARNING);
    }

    display.clear();
    drawStars();
    drawScenery();
    drawShip(shipScreenX(), shipScreenY(), bank, false);
    display.display();
    setEngineLeds(false);
}

// ===========================================================================
//  Placards (Auto-mode transitions)
// ===========================================================================
void Spaceship::updatePlacard(float dt) {
    unsigned long now = millis();
    stepStars(dt);

    display.clear();
    drawStars();
    if (state == STATE_WARNING) drawWarningPlacard();
    else                        drawAutopilotPlacard();
    display.display();

    // pulse the LEDs red during the warning, calm blue on autopilot
    if (state == STATE_WARNING) {
        bool on = (((now - stateEnterMs) / 220) % 2) == 0;
        uint8_t v = on ? 90 : 0;
        HAL::setRgbLed(pixel_Front_Top, v, 0, 0, 0);
        HAL::setRgbLed(pixel_Front_Middle, v, 0, 0, 0);
        HAL::setRgbLed(pixel_Front_Bottom, v, 0, 0, 0);
        HAL::setRgbLed(pixel_Back, 0, 0, 0, 0);
        if (now - stateEnterMs > WARNING_MS) enterState(STATE_PLAYING);
    } else {
        HAL::setRgbLed(pixel_Front_Top, 0, 0, 40, 0);
        HAL::setRgbLed(pixel_Front_Middle, 0, 0, 40, 0);
        HAL::setRgbLed(pixel_Front_Bottom, 0, 0, 40, 0);
        HAL::setRgbLed(pixel_Back, 0, 0, 0, 0);
        if (now - stateEnterMs > AUTOPILOT_MS) enterState(STATE_SCREENSAVER);
    }
}

void Spaceship::drawWarningPlacard() {
    unsigned long now = millis();
    bool on = (((now - stateEnterMs) / 220) % 2) == 0;  // blink ~2.3 Hz
    if (!on) return;

    display.setColor(WHITE);
    // angled caution triangle, skewed for a "placard" look
    int16_t cx = 64, cy = 30;
    int16_t skew = 4;
    display.drawTriangle(cx + skew, cy - 13, cx - 13 + skew/2, cy + 9, cx + 13 + skew/2, cy + 9);
    display.drawTriangle(cx + skew, cy - 11, cx - 11 + skew/2, cy + 8, cx + 11 + skew/2, cy + 8);
    // exclamation mark
    display.fillRect(cx + skew - 1, cy - 7, 2, 9);
    display.fillRect(cx + skew - 1, cy + 4, 2, 2);

    display.setFont(ArialMT_Plain_10);
    display.setTextAlignment(TEXT_ALIGN_CENTER);
    display.drawString(64, 44, "WARNING");
    display.drawString(64, 54, "OBJECT COLLISION");
}

void Spaceship::drawAutopilotPlacard() {
    display.setColor(WHITE);
    display.drawRect(24, 22, 80, 22);
    display.setFont(ArialMT_Plain_10);
    display.setTextAlignment(TEXT_ALIGN_CENTER);
    display.drawString(64, 24, "AUTOPILOT");
    display.drawString(64, 50, "engaged");
}

// ===========================================================================
//  Gameplay
// ===========================================================================
void Spaceship::updatePlaying(float dt) {
    unsigned long now = millis();

    // --- steering: left/right buttons slide the ship, with a banking lean ---
    float accel = 0.0f;
    if (turnLeftHeld && !turnRightHeld)  accel = -3.2f;
    else if (turnRightHeld && !turnLeftHeld) accel = 3.2f;
    shipVel += accel * dt;
    shipVel *= expf(-6.0f * dt);                 // drag
    if (accel == 0.0f) shipVel *= expf(-4.0f * dt);
    shipX += shipVel * dt;
    if (shipX < -1.0f) { shipX = -1.0f; shipVel = 0.0f; }
    if (shipX >  1.0f) { shipX =  1.0f; shipVel = 0.0f; }
    bank += (clampf(shipVel * 0.7f, -0.55f, 0.55f) - bank) * (1.0f - expf(-8.0f * dt));

    // --- throttle sets tunnel speed (visual pace) ---
    float sliderNorm = (float)sliderPosition_Percentage_Inverted_Filtered / 100.0f;
    speedTarget = 2.5f + sliderNorm * 7.5f;
    speed += (speedTarget - speed) * (1.0f - expf(-3.0f * dt));

    // --- invulnerability window after a hit ---
    if (invuln && now >= invulnUntil) invuln = false;

    // --- firing ---
    bool wantFire = (kFireMode == FIRE_AUTO) ||
                    (kFireMode == FIRE_HOLD_REPEAT && fireHeld);
    if (wantFire && (now - lastFireMs >= FIRE_CD[laserLevel])) fireLasers();

    // --- difficulty: spawn faster as the run goes on ---
    float diff = clampf((float)(now - runStartMs) / 50000.0f, 0.0f, 1.0f)
               + clampf((float)score / 120.0f, 0.0f, 0.6f);
    if (diff > 1.4f) diff = 1.4f;
    unsigned long gap = (unsigned long)(1400.0f - diff * 950.0f);
    if (now - lastSpawnMs >= gap) {
        lastSpawnMs = now;
        spawnAsteroid();
        if (diff > 0.8f && random(0, 3) == 0) spawnAsteroid();  // occasional pair
    }

    moveLasers(dt);
    moveAsteroids(dt);
    stepStars(dt);
    collide();
    updateUpgrades();

    // --- render ---
    display.clear();
    drawStars();
    drawAsteroids();
    drawLasers();
    drawShip(shipScreenX(), shipScreenY(), bank, invuln);
    drawBooms(now);
    drawHud();
    display.display();

    bool recentFire = (now - lastFireMs) < 70;
    setEngineLeds(recentFire);

    if (autoMode) {
        updateAccelMotion(dt);
        if (motion > IDLE_MOTION) lastActiveMs = now;
        if (now - lastActiveMs > IDLE_MS) enterState(STATE_AUTOPILOT);
    }
}

void Spaceship::spawnAsteroid() {
    int idx = -1;
    for (int i = 0; i < kMaxAsteroids; ++i) if (!asteroids[i].active) { idx = i; break; }
    if (idx < 0) return;

    Asteroid& a = asteroids[idx];
    a.active = true;
    a.wx = (float)random(-70, 71) / 100.0f;   // -0.70 .. 0.70
    a.z = 4.8f;
    a.drift = (float)random(-12, 13) / 1000.0f;
    // tier weighting shifts toward bigger rocks as the run heats up
    long roll = random(0, 100);
    long bigChance = 18 + (long)(score / 4);
    if (bigChance > 45) bigChance = 45;
    if (roll < bigChance)        a.tier = 2;
    else if (roll < bigChance + 35) a.tier = 1;
    else                          a.tier = 0;
    a.hp = a.tier + 1;
    a.seed = (uint16_t)random(0, 10000);
    a.sx = VP_X; a.sy = VP_Y; a.r = 1;
}

void Spaceship::fireLasers() {
    lastFireMs = millis();
    int16_t sx = shipScreenX();
    int16_t sy = SHIP_Y - 6;

    // laser level controls how many barrels fire
    int offsets[2]; int count;
    if (laserLevel == 0)      { offsets[0] = 0; count = 1; }
    else                      { offsets[0] = -5; offsets[1] = 5; count = 2; }

    for (int k = 0; k < count; ++k) {
        for (int i = 0; i < kMaxLasers; ++i) {
            if (!lasers[i].active) {
                lasers[i].active = true;
                lasers[i].x = (float)(sx + offsets[k]);
                lasers[i].y = (float)sy;
                lasers[i].vy = -LASER_SPEED;
                break;
            }
        }
    }
}

void Spaceship::moveLasers(float dt) {
    for (int i = 0; i < kMaxLasers; ++i) {
        if (!lasers[i].active) continue;
        lasers[i].y += lasers[i].vy * dt;
        if (lasers[i].y < 6.0f) lasers[i].active = false;
    }
}

void Spaceship::moveAsteroids(float dt) {
    unsigned long now = millis();
    float diff = clampf((float)(now - runStartMs) / 50000.0f, 0.0f, 1.0f);
    float zSpeed = (0.75f + diff * 1.0f) * (0.7f + (speed / 10.0f) * 0.6f);

    for (int i = 0; i < kMaxAsteroids; ++i) {
        Asteroid& a = asteroids[i];
        if (!a.active) continue;
        a.z -= zSpeed * dt;
        a.wx += a.drift;
        // project to screen + cache
        float z = clampf(a.z, 0.45f, 6.0f);
        float inv = 1.0f / z;
        a.sx = (int16_t)(VP_X + a.wx * X_SCALE * inv);
        a.sy = (int16_t)(VP_Y + AST_WORLD_Y * Y_SCALE * inv);
        int16_t r = (int16_t)(TIER_R[a.tier] * inv);
        a.r = (int16_t)clampf((float)r, 1.0f, 14.0f);
        if (a.sy > 72 || a.z < 0.45f) a.active = false;  // flew past the camera
    }
}

void Spaceship::collide() {
    unsigned long now = millis();
    // lasers vs asteroids
    for (int li = 0; li < kMaxLasers; ++li) {
        if (!lasers[li].active) continue;
        for (int ai = 0; ai < kMaxAsteroids; ++ai) {
            Asteroid& a = asteroids[ai];
            if (!a.active) continue;
            int16_t dx = (int16_t)lasers[li].x - a.sx;
            int16_t dy = (int16_t)lasers[li].y - a.sy;
            if (dx < 0) dx = -dx;
            if (dy < 0) dy = -dy;
            if (dx <= a.r + 2 && dy <= a.r + 3) {
                lasers[li].active = false;
                if (a.hp > 0) a.hp--;
                if (a.hp == 0) {
                    score += (a.tier + 1);
                    addBoom(a.sx, a.sy, a.r + 5, false);
                    a.active = false;
                } else {
                    addBoom(a.sx, a.sy, 3, false);  // chip spark
                }
                break;
            }
        }
    }
    // asteroids vs ship
    if (!invuln) {
        int16_t scx = shipScreenX();
        for (int ai = 0; ai < kMaxAsteroids; ++ai) {
            Asteroid& a = asteroids[ai];
            if (!a.active) continue;
            if (a.sy >= SHIP_Y - 7 && a.sy <= SHIP_Y + 9) {
                int16_t dx = a.sx - scx;
                if (dx < 0) dx = -dx;
                if (dx <= a.r + 9) {
                    deaths++;
                    addBoom(scx, SHIP_Y, 13, true);
                    a.active = false;
                    invuln = true;
                    invulnUntil = now + 1300;
                    shipVel = 0.0f;
                    break;
                }
            }
        }
    }
}

void Spaceship::updateUpgrades() {
    uint8_t lvl = 0;
    if (score >= UP2_SCORE) lvl = 2;
    else if (score >= UP1_SCORE) lvl = 1;
    laserLevel = lvl;
}

void Spaceship::addBoom(int16_t x, int16_t y, uint8_t maxR, bool big) {
    for (int i = 0; i < kMaxBooms; ++i) {
        if (!booms[i].active) {
            booms[i].active = true;
            booms[i].x = x; booms[i].y = y;
            booms[i].birth = millis();
            booms[i].maxR = maxR;
            booms[i].big = big;
            booms[i].seed = (uint16_t)random(0, 10000);
            return;
        }
    }
}

// ===========================================================================
//  Accelerometer motion estimate (Auto mode pickup / put-down detection)
// ===========================================================================
void Spaceship::updateAccelMotion(float dt) {
    // Slow baseline tracks the rest pose; deviation from it is the "handling"
    // signal. A slow alpha means a held/moving device keeps deviating (baseline
    // lags), while a device left still lets the baseline catch up to ~0 deviation.
    (void)dt;
    accelBaseX += (accelX - accelBaseX) * 0.02f;
    accelBaseY += (accelY - accelBaseY) * 0.02f;
    accelBaseZ += (accelZ - accelBaseZ) * 0.02f;
    float dx = accelX - accelBaseX, dy = accelY - accelBaseY, dz = accelZ - accelBaseZ;
    float dev = sqrtf(dx*dx + dy*dy + dz*dz);
    motion += (dev - motion) * 0.25f;
}

// ===========================================================================
//  Starfield (ported from the original screensaver)
// ===========================================================================
void Spaceship::resetStar(int i, bool forceShooting, bool randomDepth) {
    float z;
    if (randomDepth) z = (float)random(200L, 5000L) / 1000.0f;
    else             z = (float)random(3500L, 5000L) / 1000.0f;

    long rsx = random(-20L, 148L);
    long rsy = random(-10L, 74L);
    stars[i].x = ((float)rsx - 64.0f) * z / X_SCALE;
    stars[i].y = ((float)rsy - 32.0f) * z / Y_SCALE;
    stars[i].z = z;
    stars[i].vx = (float)random(-20L, 21L) / 1000.0f;
    stars[i].kind = 0;
    stars[i].life = 0.0f;

    if (forceShooting) {
        stars[i].kind = 1;
        stars[i].life = 1.0f;
        stars[i].vx = (float)random(-180L, 181L) / 1000.0f;
        stars[i].z = (float)random(1200L, 2800L) / 1000.0f;
        stars[i].x = ((float)rsx - 64.0f) * stars[i].z / X_SCALE;
        stars[i].y = ((float)rsy - 32.0f) * stars[i].z / Y_SCALE;
    }
}

void Spaceship::stepStars(float dt) {
    unsigned long now = millis();
    if (now - lastShootingMs > 1200UL) {
        lastShootingMs = now;
        if (random(0L, 3L) == 0) resetStar((int)random(0L, (long)kStarCount), true, false);
    }
    float bankPush = bank * 0.5f;
    for (int i = 0; i < kStarCount; ++i) {
        float dz = speed * dt * 0.55f;
        if (stars[i].kind == 1) dz *= 1.8f;
        stars[i].z -= dz;
        float par = 1.0f / clampf(stars[i].z, 0.25f, 6.0f);
        stars[i].x += (stars[i].vx) * dt * (0.9f + 3.0f * par);
        stars[i].x += bankPush * dt * 0.3f;
        if (stars[i].kind == 1) {
            stars[i].life -= dt * 0.8f;
            if (stars[i].life < 0.0f) stars[i].life = 0.0f;
        }
        if (stars[i].z < 0.15f) resetStar(i, false, false);
    }
}

void Spaceship::drawStars() {
    display.setColor(WHITE);
    for (int i = 0; i < kStarCount; ++i) {
        float z = clampf(stars[i].z, 0.20f, 6.0f);
        float inv = 1.0f / z;
        int16_t sx = (int16_t)(VP_X + stars[i].x * X_SCALE * inv);
        int16_t sy = (int16_t)(32 + stars[i].y * Y_SCALE * inv);
        if (sx < -30 || sx > 157 || sy < -30 || sy > 93) continue;

        if (stars[i].kind == 0) {
            if (inv > 1.0f) {
                int16_t dx = sx - VP_X, dy = sy - 32;
                int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
                int denom = adx + ady; if (denom < 1) denom = 1;
                float lenF = 2.0f + 12.0f * (inv - 1.0f);
                int16_t len = (int16_t)clampf(lenF, 2.0f, 16.0f);
                int16_t ex = sx + (int16_t)((dx * len) / denom);
                int16_t ey = sy + (int16_t)((dy * len) / denom);
                display.drawLine(sx, sy, ex, ey);
            } else if (inv > 0.6f) {
                display.fillRect(sx, sy, 2, 2);
            } else if (sx >= 0 && sx <= 127 && sy >= 0 && sy <= 63) {
                display.setPixel(sx, sy);
            }
        } else {
            float lenF = (6.0f + 16.0f * inv) * clampf(stars[i].life, 0.2f, 1.0f);
            int16_t len = (int16_t)clampf(lenF, 4.0f, 22.0f);
            int16_t dx = sx - VP_X, dy = sy - 32;
            int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
            int denom = adx + ady; if (denom < 1) denom = 1;
            int16_t ex = sx + (int16_t)((dx * len) / denom);
            int16_t ey = sy + (int16_t)((dy * len) / denom);
            display.drawLine(sx, sy, ex, ey);
            display.setPixel(sx, sy);
        }
    }
}

// ===========================================================================
//  Scenery (screensaver: a wireframe world drifts past in the distance)
// ===========================================================================
void Spaceship::spawnScenery() {
    for (int i = 0; i < kMaxScenery; ++i) {
        if (!scenery[i].active) {
            scenery[i].active = true;
            scenery[i].z = 4.5f;
            scenery[i].x = (float)random(-90, 91) / 100.0f;
            scenery[i].y = (float)random(-40, 0) / 100.0f;   // sit high, above the ship
            scenery[i].vx = (float)random(-15, 16) / 1000.0f;
            scenery[i].kind = (uint8_t)random(0, 2);         // 0 planet, 1 ringed
            return;
        }
    }
}

void Spaceship::moveScenery(float dt) {
    for (int i = 0; i < kMaxScenery; ++i) {
        if (!scenery[i].active) continue;
        scenery[i].z -= speed * dt * 0.18f;
        scenery[i].x += scenery[i].vx;
        if (scenery[i].z < 0.5f) scenery[i].active = false;
    }
}

void Spaceship::drawScenery() {
    display.setColor(WHITE);
    for (int i = 0; i < kMaxScenery; ++i) {
        if (!scenery[i].active) continue;
        float z = clampf(scenery[i].z, 0.5f, 6.0f);
        float inv = 1.0f / z;
        int16_t sx = (int16_t)(VP_X + scenery[i].x * X_SCALE * inv);
        int16_t sy = (int16_t)(VP_Y + scenery[i].y * Y_SCALE * inv);
        int16_t r = (int16_t)clampf(10.0f * inv, 3.0f, 26.0f);
        if (sx < -30 || sx > 158 || sy < -30 || sy > 94) continue;
        display.drawCircle(sx, sy, r);
        if (scenery[i].kind == 1) {
            // a flat ring: an ellipse faked with two arcs (drawn as a wide line pair)
            display.drawLine(sx - (int16_t)(r * 1.6f), sy + r / 3, sx, sy + r / 2);
            display.drawLine(sx, sy + r / 2, sx + (int16_t)(r * 1.6f), sy + r / 3);
            display.drawLine(sx - (int16_t)(r * 1.6f), sy + r / 3, sx, sy + r / 6);
            display.drawLine(sx, sy + r / 6, sx + (int16_t)(r * 1.6f), sy + r / 3);
        }
    }
}

// ===========================================================================
//  Asteroids / lasers / explosions drawing
// ===========================================================================
void Spaceship::drawAsteroids() {
    display.setColor(WHITE);
    for (int i = 0; i < kMaxAsteroids; ++i) {
        Asteroid& a = asteroids[i];
        if (!a.active) continue;
        int16_t r = a.r;
        if (r <= 1) { display.setPixel(a.sx, a.sy); continue; }
        // jagged outline: 8 vertices wobbled by a per-asteroid seed
        int16_t px = 0, py = 0, fx = 0, fy = 0;
        for (int k = 0; k <= 8; ++k) {
            float ang = (float)k * 6.2831853f / 8.0f;
            unsigned int h = a.seed + (unsigned int)k * 2654435761u;
            h ^= h >> 13; h *= 2246822519u; h ^= h >> 16;
            float jag = 0.70f + (float)(h & 0xFF) / 255.0f * 0.5f;  // 0.70..1.20
            int16_t vx = a.sx + (int16_t)(cosf(ang) * r * jag);
            int16_t vy = a.sy + (int16_t)(sinf(ang) * r * jag);
            if (k == 0) { fx = vx; fy = vy; }
            else        { display.drawLine(px, py, vx, vy); }
            px = vx; py = vy;
        }
        display.drawLine(px, py, fx, fy);
        // a couple of internal facet ticks for the bigger rocks
        if (a.tier >= 1) {
            display.drawLine(a.sx - r/2, a.sy, a.sx + r/3, a.sy - r/3);
        }
    }
}

void Spaceship::drawLasers() {
    display.setColor(WHITE);
    for (int i = 0; i < kMaxLasers; ++i) {
        if (!lasers[i].active) continue;
        int16_t x = (int16_t)lasers[i].x;
        int16_t y = (int16_t)lasers[i].y;
        display.drawLine(x, y, x, y + 5);   // short bright bolt
        display.setPixel(x, y);
    }
}

void Spaceship::drawBooms(unsigned long now) {
    display.setColor(WHITE);
    for (int i = 0; i < kMaxBooms; ++i) {
        Boom& b = booms[i];
        if (!b.active) continue;
        unsigned long age = now - b.birth;
        unsigned long life = b.big ? 520 : 300;
        if (age >= life) { b.active = false; continue; }
        float tn = (float)age / (float)life;
        float r = b.maxR * (0.3f + 0.7f * tn);
        int count = b.big ? 14 : 9;
        for (int k = 0; k < count; ++k) {
            float ang = (float)k * 6.2831853f / (float)count;
            unsigned int h = b.seed + (unsigned int)k * 40503u;
            h ^= h >> 13; h *= 2654435761u; h ^= h >> 16;
            float rr = r * (0.6f + (float)(h & 0xFF) / 255.0f * 0.6f);
            int16_t px = b.x + (int16_t)(cosf(ang) * rr);
            int16_t py = b.y + (int16_t)(sinf(ang) * rr);
            // thin out the particles as the burst ages (twinkle)
            if (tn > 0.4f && ((int)(h & 0x3) == 0)) continue;
            if (px >= 0 && px < 128 && py >= 0 && py < 64) display.setPixel(px, py);
        }
    }
}

// ===========================================================================
//  Ship -- Arwing-style wireframe (back view, banks with steering)
// ===========================================================================
void Spaceship::drawShip(int16_t cx, int16_t cy, float bankAmt, bool blink) {
    if (blink && (((millis() / 110) % 2) == 0)) return;  // flicker while invulnerable

    display.setColor(WHITE);
    const float b = clampf(bankAmt, -0.6f, 0.6f);
    const uint8_t yaw =
        (uint8_t)lroundf(b * 256.0f / (2.0f * PI));

    cf::gfx::MeshActor actor;
    actor.setMesh(&cf::gfx::ship::ship);
    actor.setPos(cx, cy);
    actor.setScaleQ8(SHIP_MODEL_SCALE_Q8);
    actor.setRotation(yaw, 0, 0);
    actor.draw(display);
}

// ===========================================================================
//  HUD -- skull (deaths) top-left, crystal (score) top-right
// ===========================================================================
void Spaceship::drawSkull(int16_t x, int16_t y) {
    display.setColor(WHITE);
    display.fillCircle(x + 4, y + 4, 4);          // cranium
    display.fillRect(x + 2, y + 7, 5, 3);         // jaw
    display.setColor(BLACK);
    display.setPixel(x + 2, y + 3); display.setPixel(x + 3, y + 3);  // eyes
    display.setPixel(x + 5, y + 3); display.setPixel(x + 6, y + 3);
    display.setPixel(x + 4, y + 9);               // jaw gap
    display.setColor(WHITE);
}

void Spaceship::drawCrystal(int16_t x, int16_t y) {
    display.setColor(WHITE);
    int16_t cx = x + 4;
    display.drawLine(cx, y,     x + 8, y + 4);    // top-right edge
    display.drawLine(x + 8, y + 4, cx, y + 9);    // bottom-right edge
    display.drawLine(cx, y + 9, x,     y + 4);    // bottom-left edge
    display.drawLine(x,     y + 4, cx, y);        // top-left edge
    display.drawLine(x,     y + 4, x + 8, y + 4); // girdle
    display.drawLine(cx, y,     cx,    y + 4);    // facet
}

void Spaceship::drawHud() {
    display.setFont(ArialMT_Plain_10);
    // deaths (skull) top-left
    drawSkull(2, 1);
    display.setColor(WHITE);
    display.setTextAlignment(TEXT_ALIGN_LEFT);
    display.drawString(13, 0, String(deaths));
    // score (crystal) top-right
    drawCrystal(108, 1);
    display.setTextAlignment(TEXT_ALIGN_RIGHT);
    display.drawString(105, 0, String(score));
}

// ===========================================================================
//  LEDs -- engine glow by throttle, white blip on fire
// ===========================================================================
void Spaceship::setEngineLeds(bool flash) {
    if (flash) {
        HAL::setRgbLed(pixel_Front_Top, 0, 0, 0, 60);
        HAL::setRgbLed(pixel_Front_Middle, 0, 0, 0, 60);
        HAL::setRgbLed(pixel_Front_Bottom, 0, 0, 0, 60);
        HAL::setRgbLed(pixel_Back, 0, 0, 0, 0);
        return;
    }
    uint8_t glow = (uint8_t)clampf((speed - 2.5f) / 7.5f * 14.0f, 0.0f, 14.0f);
    HAL::setRgbLed(pixel_Front_Top, 0, glow, glow / 2, 0);
    HAL::setRgbLed(pixel_Front_Middle, 0, glow / 2, glow, 0);
    HAL::setRgbLed(pixel_Front_Bottom, 0, 0, glow, 0);
    HAL::setRgbLed(pixel_Back, glow / 3, 0, 0, 0);
}
