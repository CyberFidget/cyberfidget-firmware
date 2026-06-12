#include "DancingGnomes.h"
#include "globals.h"
#include "HAL.h"
#include "MenuManager.h"
#include "RGBController.h"

DancingGnomes dancingGnomesApp(HAL::buttonManager(), HAL::audioManager());
DancingGnomes* DancingGnomes::instance = nullptr;

// Four beat tones that cycle each beat
const float DancingGnomes::beatTones[NUM_BEAT_TONES] = {
    261.63f, 329.63f, 392.00f, 329.63f  // C4 E4 G4 E4 — a happy little arpeggio
};

DancingGnomes::DancingGnomes(ButtonManager& btnMgr, AudioManager& audioMgr)
    : buttonManager(btnMgr),
      audioManager(audioMgr),
      display(HAL::displayProxy())
{
    instance = this;
}

void DancingGnomes::begin() {
    audioManager.setVolume(1.0f);
    audioManager.enableMic(true);

    micLevel      = 0.0f;
    isBeatActive  = false;
    lastBeatMs    = 0;
    lastUpdateMs  = 0;
    colorCycleMs  = 0;
    colorHue      = 0;
    toneOn        = false;
    beatToneIndex = 0;

    initGnomes();

    buttonManager.registerCallback(button_BottomLeftIndex, onButtonBack);
}

void DancingGnomes::end() {
    audioManager.stopTone();
    audioManager.enableMic(false);
    setColorsOff();
    buttonManager.unregisterCallback(button_BottomLeftIndex);
}

// Place 6 gnomes evenly across the screen with a small margin
// Gnome body fits in ~18px wide, 28px tall — so cols at ~21px intervals
void DancingGnomes::initGnomes() {
    // Two rows of 3: top row baseY=34, bottom row baseY=60
    // X positions: 14, 64, 114 (centered in three equal zones)
    int xPositions[6]    = { 14, 64, 114, 14, 64, 114 };
    int yPositions[6]    = { 34, 34,  34, 62, 62,  62 };
    float speeds[6]      = { 1.0f, 1.3f, 0.8f, 1.1f, 0.9f, 1.4f };

    for (int i = 0; i < NUM_GNOMES; i++) {
        gnomes[i].x      = xPositions[i];
        gnomes[i].baseY  = yPositions[i];
        gnomes[i].phase  = (i * 1) % 4;
        gnomes[i].style  = i % 4;
        gnomes[i].bobDir = (i % 2 == 0) ? 1 : -1;
        gnomes[i].speed  = speeds[i];
        gnomes[i].timer  = 0.0f;
    }
}

void DancingGnomes::update() {
    unsigned long now = millis();
    float dt = (float)(now - lastUpdateMs) / 1000.0f;
    if (dt > 0.1f) dt = 0.1f;
    lastUpdateMs = now;

    // --- Read mic ---
    float raw = audioManager.getMicVolumeLinear();
    // Smooth it: IIR low-pass with fast attack, slow decay
    if (raw > micLevel) {
        micLevel = micLevel * 0.3f + raw * 0.7f;
    } else {
        micLevel = micLevel * 0.85f + raw * 0.15f;
    }

    // Beat detection: threshold crossing
    bool beatNow = (micLevel > 0.05f);
    float danceSpeed = beatNow ? (1.0f + micLevel * 4.0f) : 0.3f;

    // Trigger a beat tone on rising edge, cycle through the arpeggio
    if (beatNow && !isBeatActive && (now - lastBeatMs > 150)) {
        audioManager.playTone(beatTones[beatToneIndex], 80);
        beatToneIndex = (beatToneIndex + 1) % NUM_BEAT_TONES;
        lastBeatMs = now;
        toneOn = true;
    }
    isBeatActive = beatNow;

    // Advance gnome animation phases
    advanceGnomes(danceSpeed * dt);

    // --- Color cycling on beat ---
    if (now - colorCycleMs > 300) {
        colorCycleMs = now;
        colorHue = (colorHue + 1) % 6;
    }
    // Map colorHue to R/G/B for LEDs — simple 6-step rainbow
    uint8_t lr = 0, lg = 0, lb = 0;
    switch (colorHue) {
        case 0: lr=80;  lg=0;   lb=0;   break;
        case 1: lr=80;  lg=40;  lb=0;   break;
        case 2: lr=0;   lg=80;  lb=0;   break;
        case 3: lr=0;   lg=80;  lb=80;  break;
        case 4: lr=0;   lg=0;   lb=80;  break;
        case 5: lr=60;  lg=0;   lb=80;  break;
    }
    if (beatNow) {
        setDeterminedColorsFront(lr, lg, lb, 0);
    } else {
        setColorsOff();
    }

    // --- Draw ---
    display.clear();

    // Title when idle (no mic signal)
    if (!beatNow && micLevel < 0.02f) {
        display.setFont(ArialMT_Plain_10);
        display.setTextAlignment(TEXT_ALIGN_CENTER);
        display.drawString(64, 0, "Make some noise!");
    }

    // Draw all gnomes
    for (int i = 0; i < NUM_GNOMES; i++) {
        drawGnome(gnomes[i], beatNow);
    }

    // Tiny mic bar at very bottom
    display.setColor(WHITE);
    int barW = (int)(micLevel * 60.0f);
    if (barW > 0) {
        display.drawHorizontalLine(34, 63, barW);
    }

    display.display();
}

void DancingGnomes::advanceGnomes(float delta) {
    for (int i = 0; i < NUM_GNOMES; i++) {
        gnomes[i].timer += delta * gnomes[i].speed * 4.0f;
        if (gnomes[i].timer >= 1.0f) {
            gnomes[i].timer -= 1.0f;
            gnomes[i].phase = (gnomes[i].phase + 1) % 4;
            gnomes[i].bobDir = -gnomes[i].bobDir;
        }
    }
}

void DancingGnomes::drawGnome(const Gnome& g, bool lit) {
    display.setColor(WHITE);
    switch (g.style) {
        case 0: drawStyleBob(g.x, g.baseY, g.phase);   break;
        case 1: drawStyleArms(g.x, g.baseY, g.phase);  break;
        case 2: drawStyleSpin(g.x, g.baseY, g.phase);  break;
        case 3: drawStyleShake(g.x, g.baseY, g.phase); break;
    }
}

// --- Style 0: Classic bob up/down ---
// Hat, head, body, legs shift up/down
void DancingGnomes::drawStyleBob(int cx, int by, int phase) {
    int bob = (phase < 2) ? -2 : 0;   // up on phases 0,1; normal on 2,3

    // Hat (tall pointy triangle)
    display.fillTriangle(cx, by - 18 + bob, cx - 5, by - 10 + bob, cx + 5, by - 10 + bob);
    // Head (small circle)
    display.drawCircle(cx, by - 7 + bob, 3);
    // Beard (filled rect below head)
    display.fillRect(cx - 3, by - 5 + bob, 6, 3);
    // Body
    display.drawRect(cx - 4, by - 3 + bob, 8, 7);
    // Legs
    int legSplay = (phase % 2 == 0) ? 2 : 0;
    display.drawLine(cx - 2, by + 4 + bob, cx - 2 - legSplay, by + 9);
    display.drawLine(cx + 2, by + 4 + bob, cx + 2 + legSplay, by + 9);
}

// --- Style 1: Arms raised / lowered alternating ---
void DancingGnomes::drawStyleArms(int cx, int by, int phase) {
    int bob = (phase == 1 || phase == 3) ? -1 : 0;
    bool armsUp = (phase < 2);

    display.fillTriangle(cx, by - 18 + bob, cx - 5, by - 10 + bob, cx + 5, by - 10 + bob);
    display.drawCircle(cx, by - 7 + bob, 3);
    display.fillRect(cx - 3, by - 5 + bob, 6, 3);
    display.drawRect(cx - 4, by - 3 + bob, 8, 7);

    // Arms
    if (armsUp) {
        display.drawLine(cx - 4, by - 2 + bob, cx - 8, by - 6 + bob); // left arm up
        display.drawLine(cx + 4, by - 2 + bob, cx + 8, by - 6 + bob); // right arm up
    } else {
        display.drawLine(cx - 4, by - 2 + bob, cx - 8, by + 1 + bob); // left arm down
        display.drawLine(cx + 4, by - 2 + bob, cx + 8, by + 1 + bob); // right arm down
    }
    // Legs (stationary)
    display.drawLine(cx - 2, by + 4 + bob, cx - 2, by + 9);
    display.drawLine(cx + 2, by + 4 + bob, cx + 2, by + 9);
}

// --- Style 2: Spin effect — leans left/right each phase ---
void DancingGnomes::drawStyleSpin(int cx, int by, int phase) {
    // Lean offset: -3, -1, +1, +3 across 4 phases
    int lean = (phase == 0) ? -3 : (phase == 1) ? -1 : (phase == 2) ? 1 : 3;

    display.fillTriangle(cx + lean, by - 18, cx + lean - 5, by - 10, cx + lean + 5, by - 10);
    display.drawCircle(cx + lean, by - 7, 3);
    display.fillRect(cx + lean - 3, by - 5, 6, 3);
    display.drawRect(cx + lean - 4, by - 3, 8, 7);
    // Legs kick outward when leaned
    display.drawLine(cx + lean - 2, by + 4, cx - 3, by + 9);
    display.drawLine(cx + lean + 2, by + 4, cx + 5, by + 9);
}

// --- Style 3: Shake (rapid side-to-side) ---
void DancingGnomes::drawStyleShake(int cx, int by, int phase) {
    int shake = (phase % 2 == 0) ? -2 : 2;
    int bob   = (phase < 2) ? -1 : 0;

    display.fillTriangle(cx + shake, by - 18 + bob,
                         cx + shake - 5, by - 10 + bob,
                         cx + shake + 5, by - 10 + bob);
    display.drawCircle(cx + shake, by - 7 + bob, 3);
    display.fillRect(cx + shake - 3, by - 5 + bob, 6, 3);
    display.drawRect(cx + shake - 4, by - 3 + bob, 8, 7);
    // Legs planted, body wiggles above
    display.drawLine(cx - 2, by + 4, cx - 2, by + 9);
    display.drawLine(cx + 2, by + 4, cx + 2, by + 9);
}

void DancingGnomes::onButtonBack(const ButtonEvent& event) {
    if (event.eventType == ButtonEvent_Released) {
        instance->end();
        MenuManager::instance().returnToMenu();
    }
}