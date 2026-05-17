// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

// lib/BreakoutGame/BreakoutGame.cpp

#include "BreakoutGame.h"
#include "BreakoutMath.h"
#include "globals.h" // For button indices, accelX
#include "HAL.h"     // For sliderPosition_Percentage_Filtered (0..100)
#include "MenuManager.h"
#include "RGBController.h"
#include <math.h>

BreakoutGame breakoutGame(HAL::buttonManager(), HAL::audioManager()); // AppManager Integration
// Initialize the static instance pointer
BreakoutGame* BreakoutGame::instance = nullptr;

// LEVEL_SPEEDS is initialized inline in BreakoutGame.h (C++17 inline static).
// LEVEL_LAYOUTS is plain (non-constexpr) and defined here.

// Layout char legend (must match BreakoutMath::isLayoutRowValid):
//   B = brick (1 hit)              U = unbreakable
//   C = crumble (3 hits)           S = spike, one-shot (1 hit, +50% speed)
//   P = spike, persistent (+30% per hit)   . = empty
// Each row string must be BRICK_COLS (8) characters.
const char* const BreakoutGame::LEVEL_LAYOUTS[BreakoutGame::NUM_LEVELS][BreakoutGame::BRICK_ROWS] = {
    // Level 1 — pure intro
    { "BBBBBBBB",
      "BBBBBBBB",
      "BBBBBBBB" },
    // Level 2 — same layout, faster
    { "BBBBBBBB",
      "BBBBBBBB",
      "BBBBBBBB" },
    // Level 3 — first taste of unbreakables in the middle row
    { "BBBBBBBB",
      "BB.UU.BB",
      "BBBBBBBB" },
    // Level 4 — introduce crumble bricks alongside unbreakable pillars
    { "BBCBBCBB",
      "BBUBBUBB",
      "BBCBBCBB" },
    // Level 5 — denser mix; crumble + unbreakable checkerboard band
    { "BUBUBUBU",
      "CBCBCBCB",
      "BUBUBUBU" },
    // Level 6 — TEXTURED paddle starts; mostly crumble bricks (per BreakoutMath::TEXTURE_FROM_LEVEL)
    { "CUCUCUCU",
      "UCUCUCUC",
      "CUCUCUCU" },
    // Level 7 — textured; introduces one-shot spike bricks ('S') for the speed kick.
    { "UCUCUCUC",
      "CSBUBUBS",
      "UCUCUCUC" },
    // Level 8 — GRAVITY starts (per BreakoutMath::GRAVITY_FROM_LEVEL). Simple brick
    // palette so the player learns the parabolic arc in isolation.
    { "BBBBBBBB",
      "BB....BB",
      "BBBBBBBB" },
    // Level 9 — gravity + persistent spike bricks ('P'). Most dangerous: every spike
    // hit boosts speed AND gravity keeps accelerating downward.
    { "BPBUBUPB",
      "CUBBBBUC",
      "BPBUBUPB" },
};

// Win / loss jingles — static const so the pointer outlives the playback.
static const AudioManager::ToneStep WIN_JINGLE[] = {
    { 523.25f, 100, 50 },  // C5
    { 659.25f, 100, 50 },  // E5
    { 783.99f, 200,  0 },  // G5
};
static const int WIN_JINGLE_LEN = sizeof(WIN_JINGLE) / sizeof(WIN_JINGLE[0]);

static const AudioManager::ToneStep LOSS_JINGLE[] = {
    { 392.00f, 200, 50 },  // G4
    { 261.63f, 400,  0 },  // C4
};
static const int LOSS_JINGLE_LEN = sizeof(LOSS_JINGLE) / sizeof(LOSS_JINGLE[0]);

BreakoutGame::BreakoutGame(ButtonManager& btnMgr, AudioManager& audioMgr)
    : display(HAL::displayProxy()), buttonManager(btnMgr), audioManager(audioMgr),
      paddleX(0.0f), paddleSpeed(2.0f),
      ballX(0.0f), ballY(0.0f), ballVX(1.0f), ballVY(-1.0f),
      currentLevel(0), livesRemaining(STARTING_LIVES),
      gameState(STATE_INPUT_MENU),
      inputMode(INPUT_ACCEL), menuCursor(0),
      leftHeld(false), rightHeld(false),
      deathCount(0), startTime(0), totalTime(0),
      brickSoundsEnabled(true)
{
    instance = this;
    // Initialize cells to empty so we have a defined state before any loadLevel.
    for (int r = 0; r < BRICK_ROWS; r++) {
        for (int c = 0; c < BRICK_COLS; c++) {
            cells[r][c] = CELL_EMPTY;
        }
    }
}

void BreakoutGame::begin() {
    registerButtonCallbacks();
    resetGame();
    setColorsOff();
}

void BreakoutGame::end() {
    unregisterButtonCallbacks();
    audioManager.stopTone();
    setColorsOff();
}

// Reset to a fresh run — back at the input-select menu, lives full, level 0.
void BreakoutGame::resetGame() {
    paddleX        = (SCREEN_WIDTH - PADDLE_WIDTH) / 2.0f;
    paddleSpeed    = 2.0f;
    currentLevel   = 0;
    livesRemaining = STARTING_LIVES;
    deathCount     = 0;
    gameState      = STATE_INPUT_MENU;
    menuCursor     = (int)inputMode;
    leftHeld       = false;
    rightHeld      = false;
    startTime      = millis();
    totalTime      = 0U;
    audioManager.stopTone();
    audioManager.stopSequence();

    for (int i = 0; i < NUM_LEVELS; i++) {
        levelTimes[i] = 0U;
    }

    // Clear cells; they'll be populated when the player picks an input mode
    // and we call loadLevel(0).
    for (int r = 0; r < BRICK_ROWS; r++) {
        for (int c = 0; c < BRICK_COLS; c++) {
            cells[r][c] = CELL_EMPTY;
        }
    }
}

// Populate cells[][] from LEVEL_LAYOUTS[idx], recenter ball/paddle, set speed.
void BreakoutGame::loadLevel(int idx) {
    if (idx < 0) idx = 0;
    if (idx >= NUM_LEVELS) idx = NUM_LEVELS - 1;
    currentLevel = idx;

    for (int r = 0; r < BRICK_ROWS; r++) {
        const char* row = LEVEL_LAYOUTS[idx][r];
        for (int c = 0; c < BRICK_COLS; c++) {
            char ch = row[c];
            if      (ch == 'B') cells[r][c] = CELL_BRICK;
            else if (ch == 'U') cells[r][c] = CELL_UNBREAKABLE;
            else if (ch == 'C') cells[r][c] = CELL_CRUMBLE_3;
            else if (ch == 'S') cells[r][c] = CELL_SPIKE_ONESHOT;
            else if (ch == 'P') cells[r][c] = CELL_SPIKE_PERSIST;
            else                cells[r][c] = CELL_EMPTY;
        }
    }

    paddleX = (SCREEN_WIDTH - PADDLE_WIDTH) / 2.0f;
    respawnBall();
    startTime = millis(); // time the level run; totalTime accumulates on win
}

// Re-center the ball with magnitude = LEVEL_SPEEDS[currentLevel], pick a random ±VX.
void BreakoutGame::respawnBall() {
    ballX = SCREEN_WIDTH / 2.0f;
    ballY = SCREEN_HEIGHT / 2.0f;
    float speed = LEVEL_SPEEDS[currentLevel];
    float dir = (random(2) ? 1.0f : -1.0f);
    ballVX = dir * speed * 0.7071f; // sin(45°)
    ballVY = -speed * 0.7071f;      // upward at 45°
}

// Update method called by AppManager at ~50Hz.
void BreakoutGame::update() {
    switch (gameState) {
        case STATE_INPUT_MENU:
            drawInputMenu();
            return;

        case STATE_GAME_OVER:
        case STATE_VICTORY:
            drawEndScreen();
            return;

        case STATE_PLAYING:
        default:
            break;
    }

    movePaddle();
    moveBall();
    checkCollisions();
    checkVictory();
    drawGame();
}

void BreakoutGame::movePaddle() {
    switch (inputMode) {
        case INPUT_ACCEL:
            movePaddleByAccel();
            break;
        case INPUT_BUTTONS: {
            const float BTN_SPEED = 2.0f;
            if (leftHeld)  paddleX -= BTN_SPEED;
            if (rightHeld) paddleX += BTN_SPEED;
            clampPaddle();
            break;
        }
        case INPUT_SLIDER:
            // sliderPosition_Percentage_Filtered is the HAL contract: [0..100] linear.
            // BreakoutMath::paddleXFromSlider clamps and converts. Same fn the tests pin.
            paddleX = BreakoutMath::paddleXFromSlider(
                sliderPosition_Percentage_Filtered, SCREEN_WIDTH, PADDLE_WIDTH);
            break;
        default:
            break;
    }
}

void BreakoutGame::movePaddleByAccel() {
    paddleX = BreakoutMath::paddleXFromAccel(
        paddleX, accelX, paddleSpeed, SCREEN_WIDTH, PADDLE_WIDTH);
}

void BreakoutGame::clampPaddle() {
    if (paddleX < 0.0f) {
        paddleX = 0.0f;
    } else if (paddleX + PADDLE_WIDTH > SCREEN_WIDTH) {
        paddleX = SCREEN_WIDTH - PADDLE_WIDTH;
    }
}

void BreakoutGame::moveBall() {
    // Gravity activates on L8+ (per BreakoutMath::GRAVITY_FROM_LEVEL). Adding a
    // small downward velocity each frame produces the parabolic arc. Note: paddle
    // deflection still preserves speed magnitude on each hit, so the player can
    // re-launch the ball upward; gravity decays vertical velocity between hits.
    if (currentLevel >= BreakoutMath::GRAVITY_FROM_LEVEL) {
        ballVY += BreakoutMath::GRAVITY_PER_FRAME;
    }
    ballX += ballVX;
    ballY += ballVY;
}

// Wall, paddle, brick collisions.
void BreakoutGame::checkCollisions() {
    // Left / right walls
    if (ballX <= 0.0f) {
        ballX = 0.0f;
        ballVX = -ballVX;
    } else if (ballX + BALL_SIZE >= SCREEN_WIDTH) {
        ballX = SCREEN_WIDTH - BALL_SIZE;
        ballVX = -ballVX;
    }

    // Top wall
    if (ballY <= 0.0f) {
        ballY = 0.0f;
        ballVY = -ballVY;
    }

    // Bottom wall -> death
    if (ballY + BALL_SIZE >= SCREEN_HEIGHT) {
        deathCount++;
        livesRemaining--;
        if (livesRemaining <= 0) {
            livesRemaining = 0;
            gameState = STATE_GAME_OVER;
            audioManager.stopTone();
            audioManager.playSequence(LOSS_JINGLE, LOSS_JINGLE_LEN);
            return;
        }
        respawnBall();
        return;
    }

    // Paddle collision — classic Breakout angle reflection (texture-aware on L6+).
    if ((ballY + BALL_SIZE) >= PADDLE_Y && ballY <= (PADDLE_Y + PADDLE_HEIGHT)) {
        if ((ballX + BALL_SIZE) >= paddleX && ballX <= (paddleX + PADDLE_WIDTH)) {
            const float hitPos = BreakoutMath::computeHitPos(
                ballX, BALL_SIZE, paddleX, PADDLE_WIDTH);
            BreakoutMath::reflectFromPaddle(
                hitPos, ballVX, ballVY, currentLevel, ballVX, ballVY);
            ballY = PADDLE_Y - BALL_SIZE;
            playBounceSound();
            millis_APP_LASTINTERACTION = millis_NOW;
        }
    }

    // Brick / unbreakable / crumble collisions
    for (int r = 0; r < BRICK_ROWS; r++) {
        for (int c = 0; c < BRICK_COLS; c++) {
            if (cells[r][c] == CELL_EMPTY) continue;

            int brickX = c * BRICK_WIDTH;
            int brickY = r * BRICK_HEIGHT;
            if ((ballX + BALL_SIZE) >= brickX &&
                ballX <= (brickX + BRICK_WIDTH) &&
                (ballY + BALL_SIZE) >= brickY &&
                ballY <= (brickY + BRICK_HEIGHT))
            {
                ballVY = -ballVY;

                // Un-stick ball
                if (ballVY > 0) {
                    ballY = brickY + BRICK_HEIGHT;
                } else {
                    ballY = brickY - BALL_SIZE;
                }

                switch (cells[r][c]) {
                    case CELL_BRICK:
                        cells[r][c] = CELL_EMPTY;
                        if (brickSoundsEnabled) playBounceSound();
                        break;
                    case CELL_CRUMBLE_3:
                        cells[r][c] = CELL_CRUMBLE_2;
                        if (brickSoundsEnabled) playBounceSound();
                        break;
                    case CELL_CRUMBLE_2:
                        cells[r][c] = CELL_CRUMBLE_1;
                        if (brickSoundsEnabled) playBounceSound();
                        break;
                    case CELL_CRUMBLE_1:
                        cells[r][c] = CELL_EMPTY;
                        if (brickSoundsEnabled) playBounceSound();
                        break;
                    case CELL_SPIKE_ONESHOT:
                        // Destroyed on hit, with a one-time speed kick.
                        cells[r][c] = CELL_EMPTY;
                        ballVX *= BreakoutMath::SPIKE_ONESHOT_MULT;
                        ballVY *= BreakoutMath::SPIKE_ONESHOT_MULT;
                        playSpikeSound();
                        break;
                    case CELL_SPIKE_PERSIST:
                        // Persists; each hit boosts speed. Use a higher tone so the
                        // player audibly registers the danger.
                        ballVX *= BreakoutMath::SPIKE_PERSIST_MULT;
                        ballVY *= BreakoutMath::SPIKE_PERSIST_MULT;
                        playSpikeSound();
                        break;
                    case CELL_UNBREAKABLE:
                    default:
                        // Persists; plays a duller thud
                        playThudSound();
                        break;
                }
                millis_APP_LASTINTERACTION = millis_NOW;
                return; // one collision per frame
            }
        }
    }
}

// Level cleared when no breakable cells remain. Unbreakables and persistent
// spikes stay forever by design and are ignored.
void BreakoutGame::checkVictory() {
    for (int r = 0; r < BRICK_ROWS; r++) {
        for (int c = 0; c < BRICK_COLS; c++) {
            const CellType t = cells[r][c];
            if (t == CELL_BRICK ||
                t == CELL_CRUMBLE_3 ||
                t == CELL_CRUMBLE_2 ||
                t == CELL_CRUMBLE_1 ||
                t == CELL_SPIKE_ONESHOT) {
                return;
            }
        }
    }

    // Record how long this level took.
    levelTimes[currentLevel] = millis() - startTime;

    // Level cleared — advance or win.
    if (currentLevel + 1 >= NUM_LEVELS) {
        // Total = sum of all per-level times (cumulative full-clear duration).
        totalTime = 0U;
        for (int i = 0; i < NUM_LEVELS; i++) totalTime += levelTimes[i];
        gameState = STATE_VICTORY;
        audioManager.stopTone();
        audioManager.playSequence(WIN_JINGLE, WIN_JINGLE_LEN);
    } else {
        loadLevel(currentLevel + 1);
    }
}

void BreakoutGame::playBounceSound() {
    audioManager.playTone(600.0f, 100);
}

void BreakoutGame::playThudSound() {
    // Tuned from 180Hz/60ms (subliminal in playtest) to 300Hz/120ms.
    audioManager.playTone(300.0f, 120);
}

void BreakoutGame::playSpikeSound() {
    // High & short — sounds urgent. Differentiates from bounce (600Hz) and thud (300Hz).
    audioManager.playTone(1200.0f, 80);
}

void BreakoutGame::draw() {
    // Drawing is dispatched from update() based on gameState; kept for parity
    // with the AppManager calling convention.
    switch (gameState) {
        case STATE_INPUT_MENU: drawInputMenu(); break;
        case STATE_GAME_OVER:
        case STATE_VICTORY:    drawEndScreen(); break;
        case STATE_PLAYING:
        default:               drawGame();      break;
    }
}

void BreakoutGame::drawInputMenu() {
    display.clear();
    display.setTextAlignment(TEXT_ALIGN_LEFT);
    display.setFont(ArialMT_Plain_10);

    display.drawString(20, 0, "Select Control");

    const char* labels[INPUT_COUNT] = { "Accelerometer", "Buttons", "Slider" };
    for (int i = 0; i < INPUT_COUNT; i++) {
        int y = 16 + i * 12;
        if (i == menuCursor) {
            display.drawString(10, y, ">");
        }
        display.drawString(22, y, labels[i]);
    }

    display.setFont(ArialMT_Plain_10);
    display.drawString(0, SCREEN_HEIGHT - 11, "Up/Dn  Enter=OK");
    display.display();
}

void BreakoutGame::drawEndScreen() {
    display.clear();
    display.setFont(ArialMT_Plain_10);

    if (gameState == STATE_VICTORY) {
        display.setTextAlignment(TEXT_ALIGN_CENTER);
        display.drawString(64, 0, "YOU WIN!");

        // Two columns of "L{n}: 12.3s" lines. With NUM_LEVELS=9, column 0
        // holds rows 0..4 (L1-L5) and column 1 holds rows 0..3 (L6-L9).
        //   col0 at x=2,  col1 at x=66, rows at y=10,18,26,34,42
        display.setTextAlignment(TEXT_ALIGN_LEFT);
        constexpr int kRowsPerCol = 5;
        constexpr int kRowYStart  = 10;
        constexpr int kRowYStep   = 8; // tighter to fit 5 rows + total line
        for (int i = 0; i < NUM_LEVELS; i++) {
            const int col = i / kRowsPerCol;
            const int row = i % kRowsPerCol;
            const int x   = (col == 0) ? 2 : 66;
            const int y   = kRowYStart + row * kRowYStep;
            const float seconds = levelTimes[i] / 1000.0f;
            String line = "L";
            line += (i + 1);
            line += ":";
            line += String(seconds, 1);
            line += "s";
            display.drawString(x, y, line);
        }

        display.setTextAlignment(TEXT_ALIGN_CENTER);
        String totalLine = "Tot ";
        totalLine += String(totalTime / 1000.0f, 1);
        totalLine += "s  D:";
        totalLine += deathCount;
        display.drawString(64, 54, totalLine);
    } else {
        display.setTextAlignment(TEXT_ALIGN_CENTER);
        display.drawString(64, 4, "GAME OVER");

        String deathMsg = "Deaths: ";
        deathMsg += deathCount;
        display.drawString(64, 18, deathMsg);

        String lvlMsg = "Reached L";
        lvlMsg += (currentLevel + 1);
        display.drawString(64, 30, lvlMsg);

        display.drawString(64, 52, "Press Btn to Reset");
    }

    display.display();
}

void BreakoutGame::drawGame() {
    display.clear();
    display.setTextAlignment(TEXT_ALIGN_LEFT);
    display.setFont(ArialMT_Plain_10);

    // Paddle — textured (notched top) on L6+, flat on L1-L5.
    if (currentLevel >= BreakoutMath::TEXTURE_FROM_LEVEL) {
        drawTexturedPaddle();
    } else {
        display.fillRect(static_cast<int>(paddleX), PADDLE_Y, PADDLE_WIDTH, PADDLE_HEIGHT);
    }

    // Ball
    display.fillRect(static_cast<int>(ballX), static_cast<int>(ballY), BALL_SIZE, BALL_SIZE);

    // Cells
    for (int r = 0; r < BRICK_ROWS; r++) {
        for (int c = 0; c < BRICK_COLS; c++) {
            int brickX = c * BRICK_WIDTH;
            int brickY = r * BRICK_HEIGHT;
            const int bw = BRICK_WIDTH  - 1;
            const int bh = BRICK_HEIGHT - 1;
            switch (cells[r][c]) {
                case CELL_BRICK:
                    display.fillRect(brickX, brickY, bw, bh);
                    break;
                case CELL_CRUMBLE_3:
                    // Fresh crumble — solid with two single-pixel corner notches.
                    display.fillRect(brickX, brickY, bw, bh);
                    display.setColor(BLACK);
                    display.setPixel(brickX, brickY);
                    display.setPixel(brickX + bw - 1, brickY + bh - 1);
                    display.setColor(WHITE);
                    break;
                case CELL_CRUMBLE_2:
                    // Cracked — top + bottom 1-px stripes only.
                    display.drawHorizontalLine(brickX, brickY, bw);
                    display.drawHorizontalLine(brickX, brickY + bh - 1, bw);
                    break;
                case CELL_CRUMBLE_1:
                    // Very cracked — 1-px outline (single-pixel).
                    display.drawRect(brickX, brickY, bw, bh);
                    break;
                case CELL_UNBREAKABLE:
                    // Checkerboard fill — at 15x3 the prior "double outline" trick
                    // collapsed visually into a solid block. Stippled pattern reads
                    // clearly different from a filled brick at this resolution.
                    for (int dy = 0; dy < bh; dy++) {
                        for (int dx = 0; dx < bw; dx++) {
                            if (((dx + dy) & 1) == 0) {
                                display.setPixel(brickX + dx, brickY + dy);
                            }
                        }
                    }
                    break;
                case CELL_SPIKE_ONESHOT: {
                    // Spikes on the TOP edge only — looks like a brick with
                    // upward-pointing teeth. Filled body underneath.
                    if (bh >= 2) {
                        display.fillRect(brickX, brickY + 1, bw, bh - 1);
                    }
                    for (int dx = 0; dx < bw; dx += 3) {
                        display.setPixel(brickX + dx, brickY);
                    }
                    break;
                }
                case CELL_SPIKE_PERSIST: {
                    // Spikes on BOTH top and bottom — a skewer that lingers.
                    // Filled middle row only (or single row at bh=3).
                    if (bh >= 3) {
                        display.drawHorizontalLine(brickX, brickY + 1, bw);
                    } else {
                        display.fillRect(brickX, brickY, bw, bh);
                    }
                    for (int dx = 0; dx < bw; dx += 3) {
                        display.setPixel(brickX + dx, brickY);
                        display.setPixel(brickX + dx, brickY + bh - 1);
                    }
                    break;
                }
                case CELL_EMPTY:
                default:
                    break;
            }
        }
    }

    // HUD: L{n} Lives:{lives} D:{deaths}
    String hud = "L";
    hud += (currentLevel + 1);
    hud += " Lv:";
    hud += livesRemaining;
    hud += " D:";
    hud += deathCount;
    display.drawString(0, PADDLE_Y - 10, hud);

    display.display();
}

// Notched top edge — base rect inset by 1 from the top, then 1-pixel teeth
// on every other column to convey "rough surface" at this resolution.
void BreakoutGame::drawTexturedPaddle() {
    const int px = static_cast<int>(paddleX);
    // Body of the paddle (rows 1..PADDLE_HEIGHT-1)
    display.fillRect(px, PADDLE_Y + 1, PADDLE_WIDTH, PADDLE_HEIGHT - 1);
    // Top row: teeth every 2 pixels
    for (int dx = 0; dx < PADDLE_WIDTH; dx += 2) {
        display.setPixel(px + dx, PADDLE_Y);
    }
}

// ---- AppManager integration: button callbacks ----

void BreakoutGame::registerButtonCallbacks() {
    buttonManager.registerCallback(button_BottomLeftIndex,  onButtonBackPressed);
    buttonManager.registerCallback(button_BottomRightIndex, onBottomRight);
    buttonManager.registerCallback(button_TopLeftIndex,     onMenuUp);
    buttonManager.registerCallback(button_TopRightIndex,    onMenuDown);
    buttonManager.registerCallback(button_MiddleLeftIndex,  onPaddleLeft);
    buttonManager.registerCallback(button_MiddleRightIndex, onPaddleRight);
}

void BreakoutGame::unregisterButtonCallbacks() {
    buttonManager.unregisterCallback(button_BottomLeftIndex);
    buttonManager.unregisterCallback(button_BottomRightIndex);
    buttonManager.unregisterCallback(button_TopLeftIndex);
    buttonManager.unregisterCallback(button_TopRightIndex);
    buttonManager.unregisterCallback(button_MiddleLeftIndex);
    buttonManager.unregisterCallback(button_MiddleRightIndex);
}

void BreakoutGame::onButtonBackPressed(const ButtonEvent& event) {
    if (event.eventType == ButtonEvent_Released) {
        ESP_LOGI(TAG_MAIN, "onButtonBackPressed => calling end() + returning to menu...");
        instance->end();
        MenuManager::instance().returnToMenu();
    }
}

void BreakoutGame::onMenuUp(const ButtonEvent& event) {
    if (event.eventType != ButtonEvent_Released) return;
    if (instance->gameState != STATE_INPUT_MENU) return;
    instance->menuCursor--;
    if (instance->menuCursor < 0) instance->menuCursor = INPUT_COUNT - 1;
}

void BreakoutGame::onMenuDown(const ButtonEvent& event) {
    if (event.eventType != ButtonEvent_Released) return;
    if (instance->gameState != STATE_INPUT_MENU) return;
    instance->menuCursor++;
    if (instance->menuCursor >= INPUT_COUNT) instance->menuCursor = 0;
}

void BreakoutGame::onPaddleLeft(const ButtonEvent& event) {
    if (event.eventType == ButtonEvent_Pressed) {
        instance->leftHeld = true;
    } else if (event.eventType == ButtonEvent_Released) {
        instance->leftHeld = false;
    }
}

void BreakoutGame::onPaddleRight(const ButtonEvent& event) {
    if (event.eventType == ButtonEvent_Pressed) {
        instance->rightHeld = true;
    } else if (event.eventType == ButtonEvent_Released) {
        instance->rightHeld = false;
    }
}

// BottomRight:
//   STATE_INPUT_MENU       — confirm selection
//   STATE_GAME_OVER /
//   STATE_VICTORY          — restart from level 1
//   STATE_PLAYING          — PLAYTEST AID: skip current level
//
// The mid-play "skip level" is a playtest convenience so we can exercise
// higher levels without proving paddle skill first. It calls
// test_winLevel(), which clears all breakable cells; the next update tick's
// checkVictory() then advances normally (records levelTime, loads next
// level, or transitions to STATE_VICTORY on the final level).
//
// This is a debug-flavored binding on a production button. Either keep it
// (level-skip is a reasonable feature for a fidget toy) or gate it behind
// a build flag before release. Tracked in T-012's notes.
void BreakoutGame::onBottomRight(const ButtonEvent& event) {
    if (event.eventType != ButtonEvent_Pressed) return;
    switch (instance->gameState) {
        case STATE_INPUT_MENU:
            instance->inputMode = (InputMode)instance->menuCursor;
            instance->livesRemaining = STARTING_LIVES;
            instance->deathCount     = 0;
            for (int i = 0; i < NUM_LEVELS; i++) instance->levelTimes[i] = 0U;
            instance->loadLevel(0);
            instance->gameState = STATE_PLAYING;
            break;
        case STATE_GAME_OVER:
        case STATE_VICTORY:
            // Restart full run but stay in the same input mode.
            instance->audioManager.stopSequence();
            instance->livesRemaining = STARTING_LIVES;
            instance->deathCount     = 0;
            for (int i = 0; i < NUM_LEVELS; i++) instance->levelTimes[i] = 0U;
            instance->loadLevel(0);
            instance->gameState = STATE_PLAYING;
            break;
        case STATE_PLAYING:
            // Playtest skip — clears breakable cells; checkVictory() advances on next tick.
            instance->test_winLevel();
            break;
        default:
            break;
    }
}

// ─────────────────────────────────────────────────────────────────────────
// Internal test API — see header comment + N-011 for the graduation plan.
// ─────────────────────────────────────────────────────────────────────────

void BreakoutGame::test_setLevel(int n) {
    // 1-indexed for humans, 0-indexed internally.
    int idx = n - 1;
    if (idx < 0) idx = 0;
    if (idx >= NUM_LEVELS) idx = NUM_LEVELS - 1;
    loadLevel(idx);
    gameState = STATE_PLAYING;
}

void BreakoutGame::test_setLives(int n) {
    if (n < 0) n = 0;
    livesRemaining = n;
}

void BreakoutGame::test_killBall() {
    // Place the ball just below the screen so the next checkCollisions() runs
    // the death path naturally.
    ballY = SCREEN_HEIGHT;
}

void BreakoutGame::test_winLevel() {
    for (int r = 0; r < BRICK_ROWS; r++) {
        for (int c = 0; c < BRICK_COLS; c++) {
            const CellType t = cells[r][c];
            if (t == CELL_BRICK ||
                t == CELL_CRUMBLE_3 ||
                t == CELL_CRUMBLE_2 ||
                t == CELL_CRUMBLE_1 ||
                t == CELL_SPIKE_ONESHOT) {
                cells[r][c] = CELL_EMPTY;
            }
        }
    }
}

BreakoutGame::StateSnapshot BreakoutGame::test_snapshot() const {
    StateSnapshot s;
    s.state   = static_cast<uint8_t>(gameState);
    s.level   = currentLevel + 1; // 1-indexed
    s.lives   = livesRemaining;
    s.deaths  = deathCount;
    s.mode    = static_cast<uint8_t>(inputMode);
    s.paddleX = paddleX;
    return s;
}
