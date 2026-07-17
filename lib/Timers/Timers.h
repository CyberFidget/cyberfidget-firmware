#ifndef CYBERFIDGET_TIMERS_APP_H
#define CYBERFIDGET_TIMERS_APP_H
#include "DisplayProxy.h"
#include "ButtonManager.h"
#include "AudioManager.h"

class Timers {
public:
    Timers(ButtonManager& btnMgr, AudioManager& audioMgr);
    void begin();
    void update();
    void end();
private:
    ButtonManager& buttonManager;
    AudioManager& audio;
    DisplayProxy& display;
    static Timers* instance;

    // Which mode we are in
    enum Mode { MODE_MENU, MODE_TIMER, MODE_STOPWATCH, MODE_MULTI };
    Mode mode;

    // ---- Menu state ----
    int menuSelection;  // 0 = timer, 1 = stopwatch, 2 = multi timer

    // ---- Timer state ----
    // The set duration in milliseconds, and how much time is left
    long timerSetMs;          // total duration set by user
    long timerRemainingMs;    // remaining while running
    bool timerRunning;
    bool timerFinished;       // alarm going off
    unsigned long timerLastTick;
    int editDigit;            // which magnitude we're editing: 0=hours,1=minutes,2=seconds
    // Alarm blink timing
    unsigned long alarmBlinkTimer;
    bool alarmBlinkOn;

    // ---- Stopwatch state ----
    unsigned long stopwatchElapsedMs;   // accumulated time
    unsigned long stopwatchStartMillis; // when current run began
    bool stopwatchRunning;
    long lastLapMs;                     // total time at last lap
    long lapSplitMs;                    // last lap split shown
    int lapCount;

    // ---- Multi-timer state ----
    static const int MAX_MULTI = 4;     // up to 4 concurrent timers
    long multiSetMs[MAX_MULTI];         // duration each was set to
    long multiRemainingMs[MAX_MULTI];   // remaining time
    bool multiRunning[MAX_MULTI];       // is this slot counting down
    bool multiFinished[MAX_MULTI];      // has this slot hit zero
    bool multiActive[MAX_MULTI];        // is this slot in use at all
    unsigned long multiLastTick;        // shared tick reference
    int multiSelection;                 // which slot is highlighted
    int multiEditDigit;                 // which magnitude we're editing in multi mode

    // Alarm melody
    unsigned long lastLedUpdate;

    void registerButtonCallbacks();
    void unregisterButtonCallbacks();

    void drawMenu();
    void drawTimer();
    void drawStopwatch();
    void drawMulti();
    String formatTime(long ms, bool showMs);

    void triggerAlarm();
    void updateAlarm();

    bool anyMultiFinished();

    // Button handlers
    static void onUp(const ButtonEvent& event);
    static void onDown(const ButtonEvent& event);
    static void onLeft(const ButtonEvent& event);
    static void onRight(const ButtonEvent& event);
    static void onEnter(const ButtonEvent& event);
    static void onBack(const ButtonEvent& event);
};

extern Timers timersApp;
#endif