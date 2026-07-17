#include "Timers.h"
#include "globals.h"
#include "HAL.h"
#include "MenuManager.h"
#include "RGBController.h"

Timers timersApp(HAL::buttonManager(), HAL::audioManager());
Timers* Timers::instance = nullptr;

Timers::Timers(ButtonManager& btnMgr, AudioManager& audioMgr) :
    buttonManager(btnMgr), audio(audioMgr), display(HAL::displayProxy()) {
    instance = this;
}

// -------------------- Lifecycle --------------------
void Timers::begin() {
    mode = MODE_MENU;
    menuSelection = 0;

    timerSetMs = 60000;        // default 1 minute
    timerRemainingMs = timerSetMs;
    timerRunning = false;
    timerFinished = false;
    editDigit = 2;             // start editing seconds

    stopwatchElapsedMs = 0;
    stopwatchRunning = false;
    lapSplitMs = 0;
    lastLapMs = 0;
    lapCount = 0;

    // Set up the multi-timer slots with sensible defaults
    for (int i = 0; i < MAX_MULTI; i++) {
        multiSetMs[i] = (long)(i + 1) * 60000L;  // 1,2,3,4 minutes as defaults
        multiRemainingMs[i] = multiSetMs[i];
        multiRunning[i] = false;
        multiFinished[i] = false;
        multiActive[i] = false;
    }
    multiActive[0] = true;     // first slot active by default
    multiSelection = 0;
    multiEditDigit = 2;        // seconds
    multiLastTick = 0;

    alarmBlinkOn = false;
    alarmBlinkTimer = 0;
    lastLedUpdate = 0;

    audio.setVolume(1.0f);
    registerButtonCallbacks();
}

void Timers::end() {
    audio.stopTone();
    setColorsOff();
    unregisterButtonCallbacks();
}

// -------------------- Helpers --------------------
String Timers::formatTime(long ms, bool showMs) {
    if (ms < 0) ms = 0;
    long totalSec = ms / 1000;
    int hours = (int)(totalSec / 3600);
    int minutes = (int)((totalSec % 3600) / 60);
    int seconds = (int)(totalSec % 60);
    char buf[16];
    if (showMs) {
        int hundredths = (int)((ms % 1000) / 10);
        sprintf(buf, "%02d:%02d.%02d", minutes, seconds, hundredths);
    } else {
        sprintf(buf, "%02d:%02d:%02d", hours, minutes, seconds);
    }
    return String(buf);
}

// Returns true if any active multi-timer slot has finished (alarm should ring)
bool Timers::anyMultiFinished() {
    for (int i = 0; i < MAX_MULTI; i++) {
        if (multiActive[i] && multiFinished[i]) return true;
    }
    return false;
}

// -------------------- Update --------------------
void Timers::update() {
    display.clear();
    display.setColor(WHITE);

    // Advance the single timer countdown when running
    if (mode == MODE_TIMER && timerRunning && !timerFinished) {
        unsigned long now = millis();
        timerRemainingMs -= (long)(now - timerLastTick);
        timerLastTick = now;
        if (timerRemainingMs <= 0) {
            timerRemainingMs = 0;
            timerRunning = false;
            triggerAlarm();
        }
    }

    // Advance all running multi-timers
    if (mode == MODE_MULTI) {
        unsigned long now = millis();
        long delta = (long)(now - multiLastTick);
        multiLastTick = now;
        for (int i = 0; i < MAX_MULTI; i++) {
            if (multiActive[i] && multiRunning[i] && !multiFinished[i]) {
                multiRemainingMs[i] -= delta;
                if (multiRemainingMs[i] <= 0) {
                    multiRemainingMs[i] = 0;
                    multiRunning[i] = false;
                    multiFinished[i] = true;
                    triggerAlarm();
                }
            }
        }
    }

    // Advance the stopwatch while running
    if (mode == MODE_STOPWATCH && stopwatchRunning) {
        stopwatchElapsedMs = millis() - stopwatchStartMillis;
    }

    // Alarm visual effects
    bool alarmActive = (mode == MODE_TIMER && timerFinished) ||
                       (mode == MODE_MULTI && anyMultiFinished());
    if (alarmActive) {
        updateAlarm();
    } else {
        setColorsOff();
    }

    // Draw the correct screen
    switch (mode) {
        case MODE_MENU:      drawMenu(); break;
        case MODE_TIMER:     drawTimer(); break;
        case MODE_STOPWATCH: drawStopwatch(); break;
        case MODE_MULTI:     drawMulti(); break;
    }

    display.display();
}

// -------------------- Drawing --------------------
void Timers::drawMenu() {
    display.setTextAlignment(TEXT_ALIGN_CENTER);
    display.setFont(ArialMT_Plain_16);
    display.drawString(64, 0, "CYBER TIMERS");

    display.setFont(ArialMT_Plain_10);
    // Three options stacked
    const char* names[] = {"TIMER", "STOPWATCH", "MULTI TIMER"};
    for (int i = 0; i < 3; i++) {
        int y = 20 + i * 14;
        if (menuSelection == i) {
            display.fillRect(14, y, 100, 13);
            display.setColor(BLACK);
            display.drawString(64, y + 1, names[i]);
            display.setColor(WHITE);
        } else {
            display.drawString(64, y + 1, names[i]);
        }
    }
}

void Timers::drawTimer() {
    // If the alarm is going off, invert the screen while blinking
    if (timerFinished && alarmBlinkOn) {
        display.fillRect(0, 0, 128, 64);
        display.setColor(BLACK);
    }

    display.setTextAlignment(TEXT_ALIGN_CENTER);
    display.setFont(ArialMT_Plain_10);
    display.drawString(64, 0, timerFinished ? "TIME'S UP!" : (timerRunning ? "RUNNING" : "SET TIME"));

    // Big time in the middle
    display.setFont(ArialMT_Plain_24);
    display.drawString(64, 24, formatTime(timerRemainingMs, false));

    // When editing (not running), underline the digit group being edited
    if (!timerRunning && !timerFinished) {
        // The time string is "HH:MM:SS" centered. Approximate underline positions.
        int underlineX;
        if (editDigit == 0) underlineX = 34;       // hours
        else if (editDigit == 1) underlineX = 60;  // minutes
        else underlineX = 86;                       // seconds
        display.drawHorizontalLine(underlineX, 50, 20);
    }

    // Restore color if we inverted
    if (timerFinished && alarmBlinkOn) {
        display.setColor(WHITE);
    }
}

void Timers::drawStopwatch() {
    display.setTextAlignment(TEXT_ALIGN_CENTER);
    display.setFont(ArialMT_Plain_10);
    display.drawString(64, 0, stopwatchRunning ? "RUNNING" : "STOPPED");

    // Big elapsed time
    display.setFont(ArialMT_Plain_24);
    display.drawString(64, 16, formatTime((long)stopwatchElapsedMs, true));

    // Show last lap split
    display.setFont(ArialMT_Plain_10);
    if (lapCount > 0) {
        String lapStr = "Lap " + String(lapCount) + ": " + formatTime(lapSplitMs, true);
        display.drawString(64, 48, lapStr);
    } else {
        display.drawString(64, 48, "MidRight = Lap");
    }
}

void Timers::drawMulti() {
    bool alarm = anyMultiFinished();

    // Invert whole screen while alarm blinks
    if (alarm && alarmBlinkOn) {
        display.fillRect(0, 0, 128, 64);
        display.setColor(BLACK);
    }

    display.setTextAlignment(TEXT_ALIGN_LEFT);
    display.setFont(ArialMT_Plain_10);
    display.drawString(2, 0, "MULTI TIMERS");

    // Each slot on its own row
    for (int i = 0; i < MAX_MULTI; i++) {
        int y = 13 + i * 12;

        // Highlight the currently selected slot with a box
        bool selected = (i == multiSelection);
        if (selected && !(alarm && alarmBlinkOn)) {
            display.drawRect(0, y - 1, 128, 12);
        }

        // Slot label and time
        String line = String(i + 1) + ": ";
        if (!multiActive[i]) {
            line += "-- off --";
        } else {
            line += formatTime(multiRemainingMs[i], false);
            if (multiFinished[i]) line += " DONE!";
            else if (multiRunning[i]) line += " >";
        }
        display.setTextAlignment(TEXT_ALIGN_LEFT);
        display.drawString(4, y, line);
    }

    // Restore color if we inverted
    if (alarm && alarmBlinkOn) {
        display.setColor(WHITE);
    }
}

// -------------------- Alarm --------------------
void Timers::triggerAlarm() {
    // Only reset the blink state if the alarm wasn't already going
    alarmBlinkTimer = millis();
    alarmBlinkOn = true;
    timerFinished = (mode == MODE_TIMER) ? true : timerFinished;
}

void Timers::updateAlarm() {
    unsigned long now = millis();
    // Blink LEDs and toggle screen invert every 300ms
    if (now - alarmBlinkTimer > 300) {
        alarmBlinkTimer = now;
        alarmBlinkOn = !alarmBlinkOn;
        if (alarmBlinkOn) {
            setDeterminedColorsAll(255, 0, 0, 0);  // red flash
            audio.playTone(880, 200);
        } else {
            setColorsOff();
        }
    }
}

// -------------------- Button handlers --------------------
void Timers::onUp(const ButtonEvent& event) {
    if (event.eventType != ButtonEvent_Pressed) return;
    Timers* self = instance;

    if (self->mode == MODE_MENU) {
        self->menuSelection--;
        if (self->menuSelection < 0) self->menuSelection = 2;
        return;
    }
    if (self->mode == MODE_TIMER && !self->timerRunning && !self->timerFinished) {
        // Increase the selected unit
        long step = 1000;                 // seconds
        if (self->editDigit == 1) step = 60000;    // minutes
        else if (self->editDigit == 0) step = 3600000; // hours
        self->timerSetMs += step;
        if (self->timerSetMs > 359999000L) self->timerSetMs = 359999000L; // cap ~99h
        self->timerRemainingMs = self->timerSetMs;
        self->audio.playTone(660, 30);
    }
    if (self->mode == MODE_MULTI) {
        int s = self->multiSelection;
        // Only edit a slot that is active and not running
        if (self->multiActive[s] && !self->multiRunning[s] && !self->multiFinished[s]) {
            long step = 1000;
            if (self->multiEditDigit == 1) step = 60000;
            else if (self->multiEditDigit == 0) step = 3600000;
            self->multiSetMs[s] += step;
            if (self->multiSetMs[s] > 359999000L) self->multiSetMs[s] = 359999000L;
            self->multiRemainingMs[s] = self->multiSetMs[s];
            self->audio.playTone(660, 30);
        }
    }
}

void Timers::onDown(const ButtonEvent& event) {
    if (event.eventType != ButtonEvent_Pressed) return;
    Timers* self = instance;

    if (self->mode == MODE_MENU) {
        self->menuSelection++;
        if (self->menuSelection > 2) self->menuSelection = 0;
        return;
    }
    if (self->mode == MODE_TIMER && !self->timerRunning && !self->timerFinished) {
        long step = 1000;
        if (self->editDigit == 1) step = 60000;
        else if (self->editDigit == 0) step = 3600000;
        self->timerSetMs -= step;
        if (self->timerSetMs < 0) self->timerSetMs = 0;
        self->timerRemainingMs = self->timerSetMs;
        self->audio.playTone(550, 30);
    }
    if (self->mode == MODE_MULTI) {
        int s = self->multiSelection;
        if (self->multiActive[s] && !self->multiRunning[s] && !self->multiFinished[s]) {
            long step = 1000;
            if (self->multiEditDigit == 1) step = 60000;
            else if (self->multiEditDigit == 0) step = 3600000;
            self->multiSetMs[s] -= step;
            if (self->multiSetMs[s] < 0) self->multiSetMs[s] = 0;
            self->multiRemainingMs[s] = self->multiSetMs[s];
            self->audio.playTone(550, 30);
        }
    }
}

void Timers::onLeft(const ButtonEvent& event) {
    if (event.eventType != ButtonEvent_Pressed) return;
    Timers* self = instance;

    if (self->mode == MODE_TIMER && !self->timerRunning && !self->timerFinished) {
        // Move edit selection toward larger units
        self->editDigit--;
        if (self->editDigit < 0) self->editDigit = 0;
        self->audio.playTone(440, 20);
    }
    if (self->mode == MODE_MULTI) {
        // Move which slot is highlighted (up the list)
        self->multiSelection--;
        if (self->multiSelection < 0) self->multiSelection = MAX_MULTI - 1;
        self->audio.playTone(440, 20);
    }
}

void Timers::onRight(const ButtonEvent& event) {
    if (event.eventType != ButtonEvent_Pressed) return;
    Timers* self = instance;

    if (self->mode == MODE_TIMER && !self->timerRunning && !self->timerFinished) {
        // Move edit selection toward smaller units
        self->editDigit++;
        if (self->editDigit > 2) self->editDigit = 2;
        self->audio.playTone(440, 20);
    } else if (self->mode == MODE_STOPWATCH && self->stopwatchRunning) {
        // Record a lap split
        long total = (long)self->stopwatchElapsedMs;
        self->lapSplitMs = total - self->lastLapMs;
        self->lastLapMs = total;
        self->lapCount++;
        self->audio.playTone(880, 40);
    } else if (self->mode == MODE_MULTI) {
        // Move which slot is highlighted (down the list)
        self->multiSelection++;
        if (self->multiSelection >= MAX_MULTI) self->multiSelection = 0;
        self->audio.playTone(440, 20);
    }
}

void Timers::onEnter(const ButtonEvent& event) {
    if (event.eventType != ButtonEvent_Pressed) return;
    Timers* self = instance;

    // If the single-timer alarm is ringing, ENTER dismisses it
    if (self->mode == MODE_TIMER && self->timerFinished) {
        self->timerFinished = false;
        self->timerRemainingMs = self->timerSetMs;
        self->audio.stopTone();
        setColorsOff();
        return;
    }

    // If any multi-timer alarm is ringing, ENTER dismisses all finished slots
    if (self->mode == MODE_MULTI && self->anyMultiFinished()) {
        for (int i = 0; i < MAX_MULTI; i++) {
            if (self->multiFinished[i]) {
                self->multiFinished[i] = false;
                self->multiRemainingMs[i] = self->multiSetMs[i];
            }
        }
        self->audio.stopTone();
        setColorsOff();
        return;
    }

    if (self->mode == MODE_MENU) {
        // Choose which app to open
        if (self->menuSelection == 0) self->mode = MODE_TIMER;
        else if (self->menuSelection == 1) self->mode = MODE_STOPWATCH;
        else self->mode = MODE_MULTI;
        self->multiLastTick = millis();
        self->audio.playTone(784, 60);
        return;
    }

    if (self->mode == MODE_TIMER) {
        // Toggle start / pause
        if (!self->timerRunning) {
            if (self->timerRemainingMs <= 0) self->timerRemainingMs = self->timerSetMs;
            if (self->timerRemainingMs > 0) {
                self->timerRunning = true;
                self->timerLastTick = millis();
                self->alarmBlinkTimer = millis();
            }
        } else {
            self->timerRunning = false;
        }
        self->audio.playTone(784, 40);
        return;
    }

    if (self->mode == MODE_STOPWATCH) {
        // Toggle start / stop
        if (!self->stopwatchRunning) {
            self->stopwatchRunning = true;
            self->stopwatchStartMillis = millis() - self->stopwatchElapsedMs;
        } else {
            self->stopwatchRunning = false;
        }
        self->audio.playTone(784, 40);
        return;
    }

    if (self->mode == MODE_MULTI) {
        // Start / pause the highlighted slot (auto-activate it if it was off)
        int s = self->multiSelection;
        if (!self->multiActive[s]) {
            self->multiActive[s] = true;
            self->multiRemainingMs[s] = self->multiSetMs[s];
        } else if (self->multiRunning[s]) {
            self->multiRunning[s] = false;   // pause
        } else {
            if (self->multiRemainingMs[s] <= 0) self->multiRemainingMs[s] = self->multiSetMs[s];
            if (self->multiRemainingMs[s] > 0) {
                self->multiRunning[s] = true;   // start
                self->multiLastTick = millis();
            }
        }
        self->audio.playTone(784, 40);
        return;
    }
}

void Timers::onBack(const ButtonEvent& event) {
    if (event.eventType != ButtonEvent_Released) return;
    Timers* self = instance;

    // If single-timer alarm is ringing, back also dismisses it (like enter)
    if (self->mode == MODE_TIMER && self->timerFinished) {
        self->timerFinished = false;
        self->timerRemainingMs = self->timerSetMs;
        self->audio.stopTone();
        setColorsOff();
        return;
    }

    // If multi alarm is ringing, back dismisses it too
    if (self->mode == MODE_MULTI && self->anyMultiFinished()) {
        for (int i = 0; i < MAX_MULTI; i++) {
            if (self->multiFinished[i]) {
                self->multiFinished[i] = false;
                self->multiRemainingMs[i] = self->multiSetMs[i];
            }
        }
        self->audio.stopTone();
        setColorsOff();
        return;
    }

    if (self->mode == MODE_MENU) {
        // Leave the whole app
        self->end();
        MenuManager::instance().returnToMenu();
    } else {
        // From a sub-mode, go back to the picker menu
        self->audio.stopTone();
        setColorsOff();
        if (self->mode == MODE_TIMER) {
            self->timerRunning = false;
        } else if (self->mode == MODE_STOPWATCH) {
            self->stopwatchRunning = false;
        }
        // Multi timers keep their state so they keep counting when you return
        self->mode = MODE_MENU;
    }
}

void Timers::registerButtonCallbacks() {
    buttonManager.registerCallback(button_UpIndex, onUp);
    buttonManager.registerCallback(button_DownIndex, onDown);
    buttonManager.registerCallback(button_LeftIndex, onLeft);
    buttonManager.registerCallback(button_RightIndex, onRight);
    buttonManager.registerCallback(button_EnterIndex, onEnter);
    buttonManager.registerCallback(button_BottomLeftIndex, onBack);
}

void Timers::unregisterButtonCallbacks() {
    buttonManager.unregisterCallback(button_UpIndex);
    buttonManager.unregisterCallback(button_DownIndex);
    buttonManager.unregisterCallback(button_LeftIndex);
    buttonManager.unregisterCallback(button_RightIndex);
    buttonManager.unregisterCallback(button_EnterIndex);
    buttonManager.unregisterCallback(button_BottomLeftIndex);
}