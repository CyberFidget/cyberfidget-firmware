#ifndef CRONCHYBIRD_H
#define CRONCHYBIRD_H

#include "ButtonManager.h"
#include "DisplayProxy.h"

// Game constants
#define CB_SCREEN_W       128
#define CB_SCREEN_H       64
#define CB_BIRD_X         20
#define CB_BIRD_RADIUS    4
#define CB_GRAVITY        0.38f
#define CB_FLAP_FORCE    -3.4f
#define CB_PIPE_WIDTH     10
#define CB_PIPE_GAP       18
#define CB_PIPE_SPEED     1.8f
#define CB_PIPE_COUNT     3
#define CB_PIPE_SPACING   52

struct Pipe {
    float x;
    int gapY;     // top of gap
    bool scored;
};

enum CronchyState {
    CB_STATE_TITLE,
    CB_STATE_PLAYING,
    CB_STATE_DEAD
};

class Cronchybird {
public:
    Cronchybird(ButtonManager& btnMgr);
    void begin();
    void update();
    void end();

private:
    ButtonManager& buttonManager;
    DisplayProxy& display;

    CronchyState state;

    // Bird physics
    float birdY;
    float birdVY;

    // Pipes
    Pipe pipes[CB_PIPE_COUNT];

    // Score
    int score;
    int highScore;

    // Timing
    unsigned long lastUpdate;
    unsigned long deathTime;

    // Flap flag (set by button callback, consumed in update)
    bool flapPending;

    // Animation tick for title screen
    unsigned long titleTick;

    void resetGame();
    void spawnPipe(int index, float startX);
    bool checkCollision();
    void drawBird(int x, int y);
    void drawPipe(int x, int topH, int botY);
    void setLedForScore(int s);

    static Cronchybird* instance;
    static void onFlapButton(const ButtonEvent& event);
    static void onBackButton(const ButtonEvent& event);
};

extern Cronchybird cronchybirdApp;

#endif