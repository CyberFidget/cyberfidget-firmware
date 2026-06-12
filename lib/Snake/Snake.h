#ifndef SNAKE_H
#define SNAKE_H

#include "DisplayProxy.h"
#include "ButtonManager.h"

// Grid dimensions: 16x8 cells at 8px each = 128x64
#define SNAKE_COLS 16
#define SNAKE_ROWS 8
#define SNAKE_CELL 8
#define SNAKE_MAX_LENGTH 128

// Ripple: 3 front LEDs light up in sequence, each held for this many ms
#define RIPPLE_STEP_MS 60
#define RIPPLE_STEPS 3

enum SnakeDirection { DIR_UP, DIR_DOWN, DIR_LEFT, DIR_RIGHT };
enum SnakeState { STATE_PLAYING, STATE_DEAD, STATE_WIN };

struct SnakeCell {
    int8_t x, y;
};

class Snake {
public:
    Snake(ButtonManager& btnMgr);
    void begin();
    void update();
    void end();

private:
    ButtonManager& buttonManager;
    DisplayProxy& display;

    static Snake* instance;

    // Snake body ring buffer
    SnakeCell body[SNAKE_MAX_LENGTH];
    int headIndex;
    int length;

    SnakeCell food;
    SnakeDirection dir;
    SnakeDirection nextDir;
    SnakeState state;

    int score;
    unsigned long lastMoveTime;
    unsigned long moveInterval;   // ms per step
    unsigned long deathTime;      // for death screen delay

    bool dirChanged;              // prevent double-turn in one tick

    // Ripple state
    bool rippleActive;
    int rippleStep;               // 0 = top, 1 = middle, 2 = bottom
    unsigned long rippleStepTime; // when the current step started

    void spawnFood();
    void moveSnake();
    bool checkSelfCollision();
    void drawGrid();
    void drawDeadScreen();
    void restartGame();

    void triggerRipple();
    void updateLeds();

    static void onButtonUp(const ButtonEvent& e);
    static void onButtonDown(const ButtonEvent& e);
    static void onButtonLeft(const ButtonEvent& e);
    static void onButtonRight(const ButtonEvent& e);
    static void onButtonEnter(const ButtonEvent& e);
    static void onButtonBack(const ButtonEvent& e);

    void registerButtonCallbacks();
    void unregisterButtonCallbacks();
};

extern Snake snakeApp;

#endif