#include "Cronchybird.h"
#include "globals.h"
#include "HAL.h"
#include "MenuManager.h"
#include "RGBController.h"

Cronchybird cronchybirdApp(HAL::buttonManager());
Cronchybird* Cronchybird::instance = nullptr;

Cronchybird::Cronchybird(ButtonManager& btnMgr)
    : buttonManager(btnMgr), display(HAL::displayProxy()) {
    instance = this;
}

void Cronchybird::begin() {
    state     = CB_STATE_TITLE;
    highScore = 0;
    flapPending = false;
    titleTick   = millis();

    resetGame();

    // Any of the four directional/action buttons + Enter flap; Back exits
    buttonManager.registerCallback(button_TopLeftIndex,     onFlapButton);
    buttonManager.registerCallback(button_TopRightIndex,    onFlapButton);
    buttonManager.registerCallback(button_MiddleLeftIndex,  onFlapButton);
    buttonManager.registerCallback(button_MiddleRightIndex, onFlapButton);
    buttonManager.registerCallback(button_EnterIndex,       onFlapButton);
    buttonManager.registerCallback(button_BottomLeftIndex,  onBackButton);

    setColorsOff();
}

void Cronchybird::end() {
    buttonManager.unregisterCallback(button_TopLeftIndex);
    buttonManager.unregisterCallback(button_TopRightIndex);
    buttonManager.unregisterCallback(button_MiddleLeftIndex);
    buttonManager.unregisterCallback(button_MiddleRightIndex);
    buttonManager.unregisterCallback(button_EnterIndex);
    buttonManager.unregisterCallback(button_BottomLeftIndex);
    setColorsOff();
}

// ── Reset / spawn ─────────────────────────────────────────────────────────────

void Cronchybird::resetGame() {
    birdY  = CB_SCREEN_H / 2.0f;
    birdVY = 0.0f;
    score  = 0;

    // Space pipes evenly ahead of the bird
    for (int i = 0; i < CB_PIPE_COUNT; i++) {
        spawnPipe(i, CB_SCREEN_W + 20.0f + i * CB_PIPE_SPACING);
    }
}

void Cronchybird::spawnPipe(int index, float startX) {
    pipes[index].x      = startX;
    // Keep gap fully on screen
    pipes[index].gapY   = 8 + random(0, CB_SCREEN_H - CB_PIPE_GAP - 16);
    pipes[index].scored = false;
}

// ── Collision ─────────────────────────────────────────────────────────────────

bool Cronchybird::checkCollision() {
    // Floor / ceiling
    if (birdY - CB_BIRD_RADIUS < 0 || birdY + CB_BIRD_RADIUS >= CB_SCREEN_H - 1)
        return true;

    for (int i = 0; i < CB_PIPE_COUNT; i++) {
        float px = pipes[i].x;
        int   gap = pipes[i].gapY;

        // Broad X overlap (bird is a circle at x=CB_BIRD_X)
        if (CB_BIRD_X + CB_BIRD_RADIUS > px &&
            CB_BIRD_X - CB_BIRD_RADIUS < px + CB_PIPE_WIDTH) {

            // Inside the gap? y must be within [gap, gap+CB_PIPE_GAP]
            float top = (float)gap;
            float bot = (float)(gap + CB_PIPE_GAP);

            if (birdY - CB_BIRD_RADIUS < top || birdY + CB_BIRD_RADIUS > bot)
                return true;
        }
    }
    return false;
}

// ── Drawing helpers ──────────────────────────────────────────────────────────

void Cronchybird::drawBird(int x, int y) {
    // Body circle
    display.drawCircle(x, y, CB_BIRD_RADIUS);
    // Eye
    display.setPixel(x + 2, y - 1);
    // Wing — small filled rect offset down
    display.fillRect(x - 4, y + 1, 4, 2);
    // Beak
    display.drawLine(x + CB_BIRD_RADIUS, y, x + CB_BIRD_RADIUS + 2, y + 1);
}

void Cronchybird::drawPipe(int x, int topH, int botY) {
    // Top pipe body
    display.fillRect(x, 0, CB_PIPE_WIDTH, topH);
    // Cap on top pipe
    display.fillRect(x - 1, topH - 3, CB_PIPE_WIDTH + 2, 3);

    // Bottom pipe body
    display.fillRect(x, botY, CB_PIPE_WIDTH, CB_SCREEN_H - botY);
    // Cap on bottom pipe
    display.fillRect(x - 1, botY, CB_PIPE_WIDTH + 2, 3);
}

// ── LED feedback ─────────────────────────────────────────────────────────────

void Cronchybird::setLedForScore(int s) {
    // Light more LEDs as score climbs (every 3 points)
    int lit = s / 3;
    if (lit > 4) lit = 4;
    for (int i = 0; i < 4; i++) {
        if (i < lit) {
            // Cycle hue: green -> yellow -> orange -> red
            uint8_t r = (uint8_t)(i * 60);
            uint8_t g = (uint8_t)(120 - i * 30);
            HAL::setRgbLed(i, r, g, 0, 0);
        } else {
            HAL::setRgbLed(i, 0, 0, 0, 0);
        }
    }
}

// ── Callbacks ────────────────────────────────────────────────────────────────

void Cronchybird::onFlapButton(const ButtonEvent& event) {
    if (event.eventType == ButtonEvent_Pressed) {
        instance->flapPending = true;
    }
}

void Cronchybird::onBackButton(const ButtonEvent& event) {
    if (event.eventType == ButtonEvent_Released) {
        instance->end();
        MenuManager::instance().returnToMenu();
    }
}

// ── Main update ──────────────────────────────────────────────────────────────

void Cronchybird::update() {
    unsigned long now = millis();

    display.clear();
    display.setColor(WHITE);

    // ── TITLE SCREEN ──────────────────────────────────────────────────────
    if (state == CB_STATE_TITLE) {
        // Animated bird bobbing in centre
        int bobY = 28 + (int)(3.0f * sinf((now - titleTick) / 300.0f));
        drawBird(CB_BIRD_X + 40, bobY);

        display.setFont(ArialMT_Plain_16);
        display.setTextAlignment(TEXT_ALIGN_CENTER);
        display.drawString(64, 2, "CRONCHYBIRD");

        display.setFont(ArialMT_Plain_10);
        display.drawString(64, 46, "Press any btn to fly!");

        // Flap to start
        if (flapPending) {
            flapPending = false;
            state = CB_STATE_PLAYING;
            resetGame();
            birdVY = CB_FLAP_FORCE;
            lastUpdate = now;
        }

        display.display();
        return;
    }

    // ── DEAD SCREEN ───────────────────────────────────────────────────────
    if (state == CB_STATE_DEAD) {
        display.setFont(ArialMT_Plain_16);
        display.setTextAlignment(TEXT_ALIGN_CENTER);
        display.drawString(64, 4, "CRONCHED!");

        display.setFont(ArialMT_Plain_10);
        display.drawString(64, 26, "Score: " + String(score));
        display.drawString(64, 38, "Best:  " + String(highScore));
        display.drawString(64, 52, "Press to retry");

        // Flash all LEDs red on death (for 600ms)
        if (now - deathTime < 600) {
            for (int i = 0; i < 4; i++)
                HAL::setRgbLed(i, 0, 120, 0, 0); // NeoPixel GRBW: g=120 is red
        } else {
            setColorsOff();
        }

        if (flapPending) {
            flapPending = false;
            state = CB_STATE_PLAYING;
            resetGame();
            birdVY = CB_FLAP_FORCE;
            lastUpdate = now;
            setColorsOff();
        }

        display.display();
        return;
    }

    // ── PLAYING ───────────────────────────────────────────────────────────

    // Fixed timestep: advance physics at ~50 Hz
    float dt = (now - lastUpdate) / 20.0f; // scale so 20ms = 1.0
    if (dt > 3.0f) dt = 3.0f;             // cap on first frame
    lastUpdate = now;

    // Flap
    if (flapPending) {
        flapPending = false;
        birdVY = CB_FLAP_FORCE;
    }

    // Physics
    birdVY += CB_GRAVITY * dt;
    birdY  += birdVY  * dt;

    // Move pipes & score
    for (int i = 0; i < CB_PIPE_COUNT; i++) {
        pipes[i].x -= CB_PIPE_SPEED * dt;

        // Score when bird clears the pipe
        if (!pipes[i].scored && pipes[i].x + CB_PIPE_WIDTH < CB_BIRD_X) {
            pipes[i].scored = true;
            score++;
            if (score > highScore) highScore = score;
            setLedForScore(score);
        }

        // Recycle pipe when off screen
        if (pipes[i].x + CB_PIPE_WIDTH < 0) {
            // Find the rightmost pipe and space beyond it
            float maxX = 0;
            for (int j = 0; j < CB_PIPE_COUNT; j++) maxX = max(maxX, pipes[j].x);
            spawnPipe(i, maxX + CB_PIPE_SPACING);
        }
    }

    // Collision check
    if (checkCollision()) {
        state     = CB_STATE_DEAD;
        deathTime = now;
        if (score > highScore) highScore = score;
        display.display();
        return;
    }

    // ── Draw ground line
    display.drawHorizontalLine(0, CB_SCREEN_H - 1, CB_SCREEN_W);

    // ── Draw pipes
    for (int i = 0; i < CB_PIPE_COUNT; i++) {
        int px   = (int)pipes[i].x;
        int topH = pipes[i].gapY;
        int botY = pipes[i].gapY + CB_PIPE_GAP;
        drawPipe(px, topH, botY);
    }

    // ── Draw bird
    drawBird(CB_BIRD_X, (int)birdY);

    // ── Score HUD (top-right)
    display.setFont(ArialMT_Plain_10);
    display.setTextAlignment(TEXT_ALIGN_RIGHT);
    display.drawString(126, 1, String(score));

    display.display();
}