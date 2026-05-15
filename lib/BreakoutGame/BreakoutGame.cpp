// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

// lib/BreakoutGame/BreakoutGame.cpp

#include "BreakoutGame.h"
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

// 'B' = brick (breakable), 'U' = unbreakable, '.' = empty.
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
    // Level 4 — carved-out top/bottom, unbreakable pillars mid
    { "B.BBBB.B",
      "BBUBBUBB",
      "B.BBBB.B" },
    // Level 5 — checkerboard of bricks and unbreakables
    { "BUBUBUBU",
      "UBUBUBUB",
      "BUBUBUBU" },
};

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
        case INPUT_SLIDER: {
            // sliderPosition_Percentage_Filtered is clamped to [0, 100].
            float pct = sliderPosition_Percentage_Filtered;
            if (pct < 0.0f) pct = 0.0f;
            if (pct > 100.0f) pct = 100.0f;
            paddleX = (pct / 100.0f) * (SCREEN_WIDTH - PADDLE_WIDTH);
            break;
        }
        default:
            break;
    }
}

void BreakoutGame::movePaddleByAccel() {
    paddleX += accelX * paddleSpeed * -0.01f;
    clampPaddle();
}

void BreakoutGame::clampPaddle() {
    if (paddleX < 0.0f) {
        paddleX = 0.0f;
    } else if (paddleX + PADDLE_WIDTH > SCREEN_WIDTH) {
        paddleX = SCREEN_WIDTH - PADDLE_WIDTH;
    }
}

void BreakoutGame::moveBall() {
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
            return;
        }
        respawnBall();
        return;
    }

    // Paddle collision — classic Breakout angle reflection.
    if ((ballY + BALL_SIZE) >= PADDLE_Y && ballY <= (PADDLE_Y + PADDLE_HEIGHT)) {
        if ((ballX + BALL_SIZE) >= paddleX && ballX <= (paddleX + PADDLE_WIDTH)) {
            float paddleCenter = paddleX + PADDLE_WIDTH / 2.0f;
            float ballCenter   = ballX + BALL_SIZE / 2.0f;
            float hitPos       = (ballCenter - paddleCenter) / (PADDLE_WIDTH / 2.0f);
            if (hitPos < -1.0f) hitPos = -1.0f;
            if (hitPos >  1.0f) hitPos =  1.0f;

            const float MAX_ANGLE_RAD = 1.0f; // ~57° off vertical
            float angle = hitPos * MAX_ANGLE_RAD;
            float speed = sqrtf(ballVX * ballVX + ballVY * ballVY);
            if (speed < 0.1f) speed = LEVEL_SPEEDS[currentLevel]; // safety
            ballVX = speed * sinf(angle);
            ballVY = -speed * cosf(angle); // always up after paddle hit
            ballY  = PADDLE_Y - BALL_SIZE;

            playBounceSound();
            millis_APP_LASTINTERACTION = millis_NOW;
        }
    }

    // Brick / unbreakable collisions
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

                if (cells[r][c] == CELL_BRICK) {
                    cells[r][c] = CELL_EMPTY;
                    if (brickSoundsEnabled) {
                        playBounceSound();
                        millis_APP_LASTINTERACTION = millis_NOW;
                    }
                } else {
                    // Unbreakable: persists, plays a duller thud
                    playThudSound();
                    millis_APP_LASTINTERACTION = millis_NOW;
                }

                return; // one collision per frame
            }
        }
    }
}

// Level cleared when no breakable bricks remain. Unbreakables are ignored.
void BreakoutGame::checkVictory() {
    for (int r = 0; r < BRICK_ROWS; r++) {
        for (int c = 0; c < BRICK_COLS; c++) {
            if (cells[r][c] == CELL_BRICK) return;
        }
    }

    // Level cleared — advance or win.
    if (currentLevel + 1 >= NUM_LEVELS) {
        totalTime = millis() - startTime; // last-level time (not cumulative)
        gameState = STATE_VICTORY;
        audioManager.stopTone();
    } else {
        loadLevel(currentLevel + 1);
    }
}

void BreakoutGame::playBounceSound() {
    audioManager.playTone(600.0f, 100);
}

void BreakoutGame::playThudSound() {
    audioManager.playTone(180.0f, 60);
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
    display.setTextAlignment(TEXT_ALIGN_CENTER);
    display.setFont(ArialMT_Plain_10);

    if (gameState == STATE_VICTORY) {
        display.drawString(64, 4, "YOU WIN!");
    } else {
        display.drawString(64, 4, "GAME OVER");
    }

    String deathMsg = "Deaths: ";
    deathMsg += deathCount;
    display.drawString(64, 16, deathMsg);

    if (gameState == STATE_VICTORY) {
        float seconds = totalTime / 1000.0f;
        String timeMsg = "Time: ";
        timeMsg += String(seconds, 2);
        timeMsg += "s";
        display.drawString(64, 28, timeMsg);
    } else {
        String lvlMsg = "Reached L";
        lvlMsg += (currentLevel + 1);
        display.drawString(64, 28, lvlMsg);
    }

    display.drawString(64, 44, "Press Btn to Reset");
    display.display();
}

void BreakoutGame::drawGame() {
    display.clear();
    display.setTextAlignment(TEXT_ALIGN_LEFT);
    display.setFont(ArialMT_Plain_10);

    // Paddle
    display.fillRect(static_cast<int>(paddleX), PADDLE_Y, PADDLE_WIDTH, PADDLE_HEIGHT);

    // Ball
    display.fillRect(static_cast<int>(ballX), static_cast<int>(ballY), BALL_SIZE, BALL_SIZE);

    // Cells: bricks filled, unbreakables outlined
    for (int r = 0; r < BRICK_ROWS; r++) {
        for (int c = 0; c < BRICK_COLS; c++) {
            int brickX = c * BRICK_WIDTH;
            int brickY = r * BRICK_HEIGHT;
            if (cells[r][c] == CELL_BRICK) {
                display.fillRect(brickX, brickY, BRICK_WIDTH - 1, BRICK_HEIGHT - 1);
            } else if (cells[r][c] == CELL_UNBREAKABLE) {
                display.drawRect(brickX, brickY, BRICK_WIDTH - 1, BRICK_HEIGHT - 1);
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

// BottomRight: confirm in input menu; reset after game-over / victory.
// Mid-game press is a no-op (lives system supersedes the old hard-reset).
void BreakoutGame::onBottomRight(const ButtonEvent& event) {
    if (event.eventType != ButtonEvent_Pressed) return;
    switch (instance->gameState) {
        case STATE_INPUT_MENU:
            instance->inputMode = (InputMode)instance->menuCursor;
            instance->livesRemaining = STARTING_LIVES;
            instance->deathCount     = 0;
            instance->loadLevel(0);
            instance->gameState = STATE_PLAYING;
            break;
        case STATE_GAME_OVER:
        case STATE_VICTORY:
            // Restart full run but stay in the same input mode.
            instance->livesRemaining = STARTING_LIVES;
            instance->deathCount     = 0;
            instance->loadLevel(0);
            instance->gameState = STATE_PLAYING;
            break;
        case STATE_PLAYING:
        default:
            break;
    }
}
