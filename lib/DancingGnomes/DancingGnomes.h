#ifndef DANCING_GNOMES_H
#define DANCING_GNOMES_H

#include "DisplayProxy.h"
#include "ButtonManager.h"
#include "AudioManager.h"

// Each gnome has its own position, dance style, and animation phase
struct Gnome {
    int x;          // center x
    int baseY;      // baseline y (feet)
    int phase;      // animation frame 0-3
    int style;      // dance style index 0-3
    int bobDir;     // +1 or -1 for bob direction
    float speed;    // how fast this gnome dances
    float timer;    // sub-frame accumulator
};

class DancingGnomes {
public:
    DancingGnomes(ButtonManager& btnMgr, AudioManager& audioMgr);
    void begin();
    void update();
    void end();

private:
    ButtonManager& buttonManager;
    AudioManager&  audioManager;
    DisplayProxy&  display;

    static DancingGnomes* instance;

    static const int NUM_GNOMES = 6;
    Gnome gnomes[NUM_GNOMES];

    // Beat / mic state
    float  micLevel;         // 0.0-1.0
    bool   isBeatActive;
    unsigned long lastBeatMs;
    unsigned long lastUpdateMs;
    unsigned long colorCycleMs;
    int    colorHue;         // 0-5 cycling through simple palette

    // Tone syncing
    bool   toneOn;
    int    beatToneIndex;
    static const int NUM_BEAT_TONES = 4;
    static const float beatTones[NUM_BEAT_TONES];

    void initGnomes();
    void advanceGnomes(float danceSpeed);
    void drawGnome(const Gnome& g, bool lit);

    // Each style draws a slightly different pose based on phase
    void drawStyleBob(int cx, int by, int phase);
    void drawStyleArms(int cx, int by, int phase);
    void drawStyleSpin(int cx, int by, int phase);
    void drawStyleShake(int cx, int by, int phase);

    static void onButtonBack(const ButtonEvent& event);
};

extern DancingGnomes dancingGnomesApp;

#endif