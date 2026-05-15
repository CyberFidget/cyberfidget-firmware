// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

// lib/BreakoutGame/BreakoutGame.h

#ifndef BREAKOUT_GAME_H
#define BREAKOUT_GAME_H

#include "DisplayProxy.h"
#include "ButtonManager.h"
#include "AudioManager.h"
#include "globals.h" // For button indices
#include <functional> // For std::function
#include <stdint.h>

class BreakoutGame {
public:
    BreakoutGame(ButtonManager& buttonManager, AudioManager& audioManager);

    void begin();  // Start the game
    void update(); // Update game logic
    void end();    // Cleanup and unregister callbacks
    void draw();   // Draw the game

    // Static instance pointer for callbacks
    static BreakoutGame* instance;

    // ─────────────────────────────────────────────────────────────────────
    // Internal test API — NOT for user code. Public so the native test
    // target in lib/BreakoutGame/test/ can call directly. Will move behind
    // HAL::commandRegistry when N-011 is implemented; see N-011 for the
    // graduation path.
    // ─────────────────────────────────────────────────────────────────────
    struct StateSnapshot {
        uint8_t state;     // GameState as uint8_t (avoid exposing the enum)
        int     level;     // 1-indexed for humans
        int     lives;
        int     deaths;
        uint8_t mode;      // InputMode as uint8_t
        float   paddleX;
    };
    void          test_setLevel(int n);       // 1-indexed; clamps + loadLevel
    void          test_setLives(int n);
    void          test_killBall();            // forces ball below screen this update
    void          test_winLevel();            // empties all BRICK / CRUMBLE cells
    StateSnapshot test_snapshot() const;

private:
    // AppManager Integration
    void registerButtonCallbacks();
    void unregisterButtonCallbacks();
    static void onButtonBackPressed(const ButtonEvent& event);
    static void onMenuUp(const ButtonEvent& event);
    static void onMenuDown(const ButtonEvent& event);
    static void onPaddleLeft(const ButtonEvent& event);
    static void onPaddleRight(const ButtonEvent& event);
    static void onBottomRight(const ButtonEvent& event); // Confirm in menu, reset elsewhere

    // References to hardware components
    DisplayProxy& display;
    ButtonManager& buttonManager;
    AudioManager& audioManager;

    // Game properties and state variables
    // Screen dimensions
    static constexpr int SCREEN_WIDTH  = 128;
    static constexpr int SCREEN_HEIGHT = 64;

    // Paddle properties
    float paddleX;                     // Left edge of the paddle
    static constexpr int PADDLE_Y      = SCREEN_HEIGHT - 8; // Near bottom
    static constexpr int PADDLE_WIDTH  = 20;
    static constexpr int PADDLE_HEIGHT = 3;
    float paddleSpeed; // Multiplier for accelerometer-driven paddle motion

    // Ball properties
    float ballX, ballY;    // Ball position
    float ballVX, ballVY;  // Ball velocity
    static constexpr int BALL_SIZE = 2; // 2x2 pixel ball

    // Bricks / cells
    static constexpr int BRICK_ROWS   = 3;
    static constexpr int BRICK_COLS   = 8;
    static constexpr int BRICK_WIDTH  = 16;
    static constexpr int BRICK_HEIGHT = 4;

    enum CellType : uint8_t {
        CELL_EMPTY         = 0,
        CELL_BRICK         = 1,
        CELL_UNBREAKABLE   = 2,
        CELL_CRUMBLE_3     = 3, // fresh, 3 hits remaining
        CELL_CRUMBLE_2     = 4, // cracked
        CELL_CRUMBLE_1     = 5, // very cracked
        CELL_SPIKE_ONESHOT = 6, // destroyed on hit, ~1.5x speed kick (L7+)
        CELL_SPIKE_PERSIST = 7, // persists, ~1.3x speed each hit (L9+)
    };
    CellType cells[BRICK_ROWS][BRICK_COLS];

    // Level progression — 9 levels. L6+ textured paddle, L8+ gravity. See BreakoutMath.h.
    static constexpr int   NUM_LEVELS = 9;
    static constexpr float LEVEL_SPEEDS[NUM_LEVELS] =
        { 1.0f, 1.2f, 1.4f, 1.6f, 1.8f, 1.8f, 2.0f, 1.6f, 1.6f };
    // Gravity slows the start; L8 (idx 7) and L9 (idx 8) trade horizontal speed
    // for vertical acceleration.
    static const char* const LEVEL_LAYOUTS[NUM_LEVELS][BRICK_ROWS]; // defined in .cpp
    int currentLevel;                       // 0-indexed
    unsigned long levelTimes[NUM_LEVELS];   // ms per level; 0 if not yet completed

    // Lives
    static constexpr int STARTING_LIVES = 3;
    int livesRemaining;

    // Game state machine
    enum GameState : uint8_t {
        STATE_INPUT_MENU = 0,
        STATE_PLAYING    = 1,
        STATE_GAME_OVER  = 2,
        STATE_VICTORY    = 3,
    };
    GameState gameState;

    // Input selection
    enum InputMode : uint8_t {
        INPUT_ACCEL   = 0,
        INPUT_BUTTONS = 1,
        INPUT_SLIDER  = 2,
        INPUT_COUNT   = 3,
    };
    InputMode inputMode;
    int  menuCursor;       // 0..INPUT_COUNT-1
    bool leftHeld;         // for INPUT_BUTTONS
    bool rightHeld;        // for INPUT_BUTTONS

    // Tally / timing
    int  deathCount;
    unsigned long startTime; // when current run started
    unsigned long totalTime; // final time once won

    // Option to enable/disable brick hit sounds
    bool brickSoundsEnabled;

    // Private methods
    void resetGame();             // Reset to fresh run (back to input menu)
    void loadLevel(int idx);      // Load level idx, recenter ball/paddle, set speed
    void respawnBall();           // After death, re-center ball at current level speed
    void movePaddle();            // Dispatch on inputMode
    void movePaddleByAccel();     // Existing accelerometer path
    void clampPaddle();
    void moveBall();
    void checkCollisions();
    void checkVictory();
    void drawGame();
    void drawTexturedPaddle();    // L6+ — serrated top edge
    void drawInputMenu();
    void drawEndScreen();         // GAME OVER or YOU WIN
    void playBounceSound();
    void playThudSound();         // For unbreakable hits
    void playSpikeSound();        // For spike-brick hits (one-shot + persistent)
};

extern BreakoutGame breakoutGame;

#endif // BREAKOUT_GAME_H
