// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#include "Starburst.h"
#include "globals.h"
#include "HAL.h"
#include "MenuManager.h"
#include "RGBController.h"
#include <math.h>

Starburst starburstApp(HAL::buttonManager());
Starburst* Starburst::instance = nullptr;

Starburst::Starburst(ButtonManager& btnMgr) :
    buttonManager(btnMgr), display(HAL::displayProxy()),
    nextBurst(0), lastFrame(0), ledRR(0),
    appState(STATE_MODE_SELECT), runMode(MODE_MANUAL),
    menuIndex(0), nextAutoSpawn(0) {
    instance = this;
    for (int i = 0; i < MAX_BURSTS; i++) bursts[i].active = false;
    for (int i = 0; i < 3; i++) ledBurst[i] = -1;
}

void Starburst::begin() {
    registerButtonCallbacks();
    for (int i = 0; i < MAX_BURSTS; i++) bursts[i].active = false;
    for (int i = 0; i < 3; i++) ledBurst[i] = -1;
    nextBurst = 0;
    ledRR = 0;
    lastFrame = millis();
    // Always start on the Manual/Auto chooser so the booth operator can pick.
    appState = STATE_MODE_SELECT;
    runMode = MODE_MANUAL;
    menuIndex = 0;
    nextAutoSpawn = 0;
}

void Starburst::scheduleNextAutoSpawn(unsigned long now) {
    nextAutoSpawn = now + AUTO_MIN_MS + random(0, AUTO_VAR_MS);
}

// Mode chooser shown when the app launches: Manual vs Auto.
void Starburst::drawModeSelect() {
    display.clear();
    display.setColor(WHITE);
    display.setTextAlignment(TEXT_ALIGN_CENTER);

    display.setFont(ArialMT_Plain_16);
    display.drawString(64, 0, "Starburst");

    display.setFont(ArialMT_Plain_10);
    const char* opts[2] = { "Manual", "Auto" };
    for (int i = 0; i < 2; i++) {
        int y = 22 + i * 15;
        if (i == menuIndex) {
            // highlighted row: white bar with black text
            display.fillRect(34, y - 1, 60, 13);
            display.setColor(BLACK);
            display.drawString(64, y, opts[i]);
            display.setColor(WHITE);
        } else {
            display.drawString(64, y, opts[i]);
        }
    }

    display.display();
}

void Starburst::spawnBurst(int style) {
    int bi = nextBurst;
    Burst& b = bursts[bi];
    b.active = true;
    // keep a margin so bursts mostly fit on screen but still touch edges sometimes
    b.cx = random(10, 118);
    b.cy = random(10, 54);
    b.style = style;
    b.variant = random(0, 1000);
    b.birth = millis();
    b.lifetime = 600 + random(0, 1200);  // 0.6 to 1.8 seconds
    b.size = 8 + random(0, 22);
    b.seed = random(0, 10000);
    b.hue = random(0, 4096);             // each burst gets its own LED color
    nextBurst = (nextBurst + 1) % MAX_BURSTS;

    // Tie a front LED to this burst so each spark cluster lends its color.
    if (LED_SNAP_TO_NEWEST) {
        // Always show the 3 most-recent bursts (round-robin the 3 LEDs).
        ledBurst[ledRR] = bi;
        ledRR = (ledRR + 1) % 3;
    } else {
        // Latch onto a free LED (dark, or its burst already finished) and hold.
        for (int i = 0; i < 3; i++) {
            int cur = ledBurst[i];
            bool freeSlot = (cur < 0) || !bursts[cur].active ||
                            (b.birth - bursts[cur].birth >= (unsigned long)bursts[cur].lifetime);
            if (freeSlot) { ledBurst[i] = bi; break; }
        }
    }
}

// Draws a single burst as a cloud of firework particles. They blow out fast,
// coast to their furthest diameter as drag bleeds off the radial speed
// (inertia), then twinkle and gently fall under a soft gravity. Each style only
// changes how the particles are laid out (palm, ring, cross, spray, star); the
// physics below is shared.
void Starburst::drawBurst(const Burst& b, unsigned long now) {
    unsigned long age = now - b.birth;
    if (age >= (unsigned long)b.lifetime) return;
    float tn = (float)age / (float)b.lifetime;  // 0..1 over the burst's life

    // Inertia / drag: explode outward fast, then ease toward the furthest
    // diameter and coast (asymptotic, so the sparks keep momentum behind them).
    float expand = 1.0f - expf(-3.5f * tn);
    // Gentle gravity: negligible during the flash, easing the sparks downward as
    // they age (t^2 accelerates the drift softly toward the end).
    float fall = (float)GRAVITY_STRENGTH * tn * tn;
    float maxR = (float)b.size * 1.7f;

    // Twinkle / fade: a solid flash up front, then particles flicker and thin
    // out as the burst dies. keepChance falls 255 -> 0 across the back ~70%.
    int keepChance = 255;
    if (tn > 0.30f) {
        keepChance = (int)(255.0f * (1.0f - (tn - 0.30f) / 0.70f));
        if (keepChance < 0) keepChance = 0;
    }
    unsigned int timeBucket = (unsigned int)(now >> 6);  // ~15 Hz flicker

    // Particle count and the spokes/arms/points the chosen pattern uses.
    int count, spokes = 0;
    switch (b.style) {
        case 0:  spokes = 8 + (b.variant % 7); count = spokes * 3;  break; // palm
        case 1:  count = 30 + (b.variant % 14);                     break; // ring / peony
        case 2:  spokes = 4 + (b.variant % 3); count = spokes * 7;  break; // diagonal cross
        case 3:  count = 36 + (b.variant % 20);                     break; // brocade spray
        default: spokes = 5 + (b.variant % 4); count = spokes * 4;  break; // star (style 4)
    }
    if (count > 64) count = 64;

    for (int i = 0; i < count; i++) {
        // Stable per-particle hash -> jitter, speed and fall variation.
        unsigned int ph = (unsigned int)b.seed + (unsigned int)i * 2654435761u;
        ph ^= ph >> 15; ph *= 2246822519u; ph ^= ph >> 13;
        float pr  = (float)(ph & 0xFFFF) / 65535.0f;          // 0..1
        float pr2 = (float)((ph >> 16) & 0xFFFF) / 65535.0f;  // 0..1

        float theta, rf;
        switch (b.style) {
            case 0: { // palm: a few particles strung along each spoke
                int sp = i / 3, j = i % 3;
                theta = (float)sp * 6.28318f / (float)spokes + (pr - 0.5f) * 0.15f;
                rf = (0.45f + 0.275f * j) + (pr2 - 0.5f) * 0.10f;
                break;
            }
            case 1: { // ring / peony: evenly spaced shell, thin radius spread
                theta = (float)i * 6.28318f / (float)count + (pr - 0.5f) * 0.08f;
                rf = 0.86f + pr2 * 0.14f;
                break;
            }
            case 2: { // cross: a few long diagonal arms (rooted at 45 degrees)
                int arm = i / 7, j = i % 7;
                theta = 0.7853f + (float)arm * (6.28318f / (float)spokes);
                rf = (0.30f + 0.115f * j) + (pr2 - 0.5f) * 0.08f;
                break;
            }
            case 3: { // brocade spray: filled random cloud
                theta = pr * 6.28318f;
                rf = sqrtf(pr2);                                   // uniform disk fill
                break;
            }
            default: { // star (style 4): spikes of particles toward each point
                int pt = i / 4, j = i % 4;
                theta = (b.variant * 0.05f) + (float)pt * 6.28318f / (float)spokes
                        + (pr - 0.5f) * 0.06f;
                rf = (1.0f - 0.18f * j) + (pr2 - 0.5f) * 0.05f;
                break;
            }
        }

        float dist = rf * maxR * expand;
        // Per-particle fall variation breaks up the formation, so the cloud sags
        // and rains instead of sliding down as one rigid shape.
        float pfall = GRAVITY_ENABLED ? fall * (0.75f + pr2 * 0.6f) : 0.0f;
        int px = b.cx + (int)(cosf(theta) * dist);
        int py = (int)((float)b.cy + sinf(theta) * dist + pfall);

        // Twinkle: drop a time-varying fraction of particles as the burst fades.
        unsigned int th = ph ^ (timeBucket * 40503u);
        th ^= th >> 13; th *= 2654435761u; th ^= th >> 16;
        if ((int)(th & 0xFF) > keepChance) continue;

        if (px >= 0 && px < 128 && py >= 0 && py < 64)
            display.setPixel(px, py);
    }
}

// Drives the 3 front LEDs from the bursts they're attached to: each LED takes
// its burst's color and fades out as the burst dies. With no burst attached the
// LED goes dark, so the LEDs are off whenever nothing is bursting. The back LED
// is never used.
void Starburst::updateLeds(unsigned long now) {
    const uint16_t frontPixels[3] = { pixel_Front_Top, pixel_Front_Middle, pixel_Front_Bottom };
    for (int i = 0; i < 3; i++) {
        int bi = ledBurst[i];
        bool lit = false;
        if (bi >= 0 && bursts[bi].active) {
            unsigned long age = now - bursts[bi].birth;
            if (age < (unsigned long)bursts[bi].lifetime) {
                float tn = (float)age / (float)bursts[bi].lifetime;
                float bright = 1.0f - tn;          // bright at the flash, fading out
                uint8_t dim = (uint8_t)(LED_MAX_BRIGHT * bright);
                uint8_t r, g, bl;
                mapToRainbow(bursts[bi].hue, dim, r, g, bl);
                HAL::setRgbLed(frontPixels[i], r, g, bl, 0);
                lit = true;
            }
        }
        if (!lit) {
            ledBurst[i] = -1;
            HAL::setRgbLed(frontPixels[i], 0, 0, 0, 0);
        }
    }
    HAL::setRgbLed(pixel_Back, 0, 0, 0, 0);   // back LED unused
    updateStrip();
}

void Starburst::update() {
    unsigned long now = millis();
    lastFrame = now;

    // Mode chooser: pause the effect until the operator picks Manual or Auto.
    if (appState == STATE_MODE_SELECT) {
        drawModeSelect();
        updateLeds(now);   // no bursts attached yet -> LEDs stay dark
        return;
    }

    display.clear();

    // Auto mode: fire bursts on their own at a varying cadence so the app
    // runs unattended. Buttons still spawn bursts on top of this.
    if (runMode == MODE_AUTO && now >= nextAutoSpawn) {
        spawnBurst(random(0, 5));                  // random style 0..4
        if (random(0, 5) == 0) spawnBurst(random(0, 5));  // occasional flurry
        scheduleNextAutoSpawn(now);
    }

    // Draw all active bursts
    for (int i = 0; i < MAX_BURSTS; i++) {
        if (!bursts[i].active) continue;
        if (now - bursts[i].birth >= (unsigned long)bursts[i].lifetime) {
            bursts[i].active = false;
            continue;
        }
        drawBurst(bursts[i], now);
    }

    display.display();
    updateLeds(now);
}

// Up/Left move the menu highlight toward Manual; Down/Right toward Auto.
// Once running, the four direction buttons spawn their burst styles as before.
void Starburst::onButtonUp(const ButtonEvent& event) {
    if (event.eventType != ButtonEvent_Pressed) return;
    if (instance->appState == STATE_MODE_SELECT) {
        if (instance->menuIndex > 0) instance->menuIndex--;
    } else {
        instance->spawnBurst(0);
    }
}
void Starburst::onButtonDown(const ButtonEvent& event) {
    if (event.eventType != ButtonEvent_Pressed) return;
    if (instance->appState == STATE_MODE_SELECT) {
        if (instance->menuIndex < 1) instance->menuIndex++;
    } else {
        instance->spawnBurst(1);
    }
}
void Starburst::onButtonLeft(const ButtonEvent& event) {
    if (event.eventType != ButtonEvent_Pressed) return;
    if (instance->appState == STATE_MODE_SELECT) {
        if (instance->menuIndex > 0) instance->menuIndex--;
    } else {
        instance->spawnBurst(2);
    }
}
void Starburst::onButtonRight(const ButtonEvent& event) {
    if (event.eventType != ButtonEvent_Pressed) return;
    if (instance->appState == STATE_MODE_SELECT) {
        if (instance->menuIndex < 1) instance->menuIndex++;
    } else {
        instance->spawnBurst(3);
    }
}
// Enter confirms the highlighted mode on the chooser; spawns style 4 while running.
void Starburst::onButtonEnter(const ButtonEvent& event) {
    if (event.eventType != ButtonEvent_Pressed) return;
    if (instance->appState == STATE_MODE_SELECT) {
        instance->runMode = (instance->menuIndex == 1) ? MODE_AUTO : MODE_MANUAL;
        instance->appState = STATE_RUNNING;
        if (instance->runMode == MODE_AUTO) {
            instance->spawnBurst(random(0, 5));   // instant feedback on entering auto
            instance->scheduleNextAutoSpawn(millis());
        }
    } else {
        instance->spawnBurst(4);
    }
}
void Starburst::onButtonBack(const ButtonEvent& event) {
    if (event.eventType == ButtonEvent_Released) {
        instance->end();
        MenuManager::instance().returnToMenu();
    }
}

void Starburst::end() {
    setColorsOff();
    unregisterButtonCallbacks();
}

void Starburst::registerButtonCallbacks() {
    buttonManager.registerCallback(button_TopLeftIndex, onButtonUp);
    buttonManager.registerCallback(button_TopRightIndex, onButtonDown);
    buttonManager.registerCallback(button_MiddleLeftIndex, onButtonLeft);
    buttonManager.registerCallback(button_MiddleRightIndex, onButtonRight);
    buttonManager.registerCallback(button_BottomRightIndex, onButtonEnter);
    buttonManager.registerCallback(button_BottomLeftIndex, onButtonBack);
}

void Starburst::unregisterButtonCallbacks() {
    buttonManager.unregisterCallback(button_TopLeftIndex);
    buttonManager.unregisterCallback(button_TopRightIndex);
    buttonManager.unregisterCallback(button_MiddleLeftIndex);
    buttonManager.unregisterCallback(button_MiddleRightIndex);
    buttonManager.unregisterCallback(button_BottomRightIndex);
    buttonManager.unregisterCallback(button_BottomLeftIndex);
}