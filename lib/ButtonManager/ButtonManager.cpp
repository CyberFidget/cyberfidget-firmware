// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#include "ButtonManager.h"
#include "globals.h" 
#include <Preferences.h>
Preferences preferencesMainApp;

ButtonManager::ButtonManager(const int* pins,
                             const bool* usePullups,
                             int numButtons,
                             unsigned long debounceMs,
                             unsigned long holdThresholdMs)
: _pins(pins),
  _usePullups(usePullups),
  _numButtons(numButtons),
  _debounceMs(debounceMs),
  _holdThresholdMs(holdThresholdMs),
  _eventWriteIndex(0),
  _eventReadIndex(0)
{
    // Allocate memory for state arrays
    _currentState    = new bool[_numButtons];
    _previousState   = new bool[_numButtons];
    _lastChangeTime  = new unsigned long[_numButtons];
    _pressStartTime  = new unsigned long[_numButtons];
    _heldReported    = new bool[_numButtons];
#ifdef CF_TEST_CLI
    _injectedHold    = new bool[_numButtons];
#endif

    // Initialize them
    for (int i = 0; i < _numButtons; i++) {
        _currentState[i]   = false; // default to not pressed
        _previousState[i]  = false;
        _lastChangeTime[i] = 0;
        _pressStartTime[i] = 0;
        _heldReported[i]   = false;
#ifdef CF_TEST_CLI
        _injectedHold[i]   = false;
#endif
    }

    // Clear event queue
    for (int i = 0; i < MAX_EVENTS; i++) {
        _events[i].buttonIndex = -1;
        _events[i].eventType   = ButtonEvent_None;
        _events[i].duration    = 0;
    }
    
    // Initialize callback array to nullptr
    for (int i = 0; i < MAX_BUTTONS_SUPPORTED; i++) {
        buttonCallbacks[i] = nullptr;
    }
}

ButtonCallback ButtonManager::getCallback(int buttonIndex) const {
    if (buttonIndex < 0 || buttonIndex >= _numButtons) return nullptr;
    return buttonCallbacks[buttonIndex];
}

void ButtonManager::init() {
    // Configure each pin as INPUT or INPUT_PULLUP
    for (int i = 0; i < _numButtons; i++) {
        if (_usePullups[i]) {
            pinMode(_pins[i], INPUT_PULLUP);
        } else {
            pinMode(_pins[i], INPUT);
        }
    }
    loadButtonCounters();
}

void ButtonManager::update() {
    unsigned long now = millis();

    // For each button, read the raw input
    // Because buttons are active-low, the "pressed" state is (digitalRead(...) == LOW)
    for (int i = 0; i < _numButtons; i++) {
        bool rawPressed = (digitalRead(_pins[i]) == LOW);
        // If using external pull-ups or internal pull-ups, the logic is the same:
        // "pressed" means the pin reads LOW
#ifdef CF_TEST_CLI
        // Injected presses (Serial `btn` command) hold the button "down"
        // until the matching injected release, so the pin scan doesn't emit
        // a spurious Released after the debounce window.
        if (_injectedHold[i]) rawPressed = true;
#endif

        // Compare with current stable state
        if (rawPressed != _currentState[i]) {
            // The reading has changed from the stable state, so start or check debounce
            if ((now - _lastChangeTime[i]) > _debounceMs) {
                // It's been longer than debounce time, so accept the new state
                _currentState[i] = rawPressed;

                // If the new state is pressed, record pressStartTime
                if (_currentState[i]) {
                    _pressStartTime[i] = now;
                    _heldReported[i]   = false;
                    // Generate "Pressed" event
                    pushEvent(i, ButtonEvent_Pressed, 0 /*duration not relevant yet*/);
                } else {
                    // The button was just released
                    unsigned long pressDuration = now - _pressStartTime[i];
                    pushEvent(i, ButtonEvent_Released, pressDuration);
                }

                // Update the lastChangeTime
                _lastChangeTime[i] = now;

                // Reset the last interaction timer
                millis_APP_LASTINTERACTION = millis_NOW;
            }
        } else {
            // The raw reading matches the stable state; check if it's pressed long enough for "Held"
            if (_currentState[i]) {
                unsigned long pressDuration = now - _pressStartTime[i];
                if (!_heldReported[i] && (pressDuration >= _holdThresholdMs)) {
                    // Fire a Held event
                    _heldReported[i] = true;
                    pushEvent(i, ButtonEvent_Held, pressDuration);
                }
            }
        }
        // Save this reading as previous
        _previousState[i] = _currentState[i];
    }
}

bool ButtonManager::getNextEvent(ButtonEvent &event) {
    // If read == write, queue is empty
    if (_eventReadIndex == _eventWriteIndex) {
        return false;
    }

    // Grab event
    event = _events[_eventReadIndex];
    // Advance read pointer
    _eventReadIndex = (_eventReadIndex + 1) % MAX_EVENTS;
    return true;
}

bool ButtonManager::isPressed(int buttonIndex) const {
    if (buttonIndex < 0 || buttonIndex >= _numButtons) return false;
    return _currentState[buttonIndex];
}

void ButtonManager::registerCallback(int buttonIndex, ButtonCallback callback) {
    if (buttonIndex >= 0 && buttonIndex < _numButtons && buttonIndex < MAX_BUTTONS_SUPPORTED) {
        buttonCallbacks[buttonIndex] = callback;
    }
}

void ButtonManager::unregisterCallback(int buttonIndex) {
    if (buttonIndex >= 0 && buttonIndex < _numButtons && buttonIndex < MAX_BUTTONS_SUPPORTED) {
        buttonCallbacks[buttonIndex] = nullptr;
    }
}

bool ButtonManager::hasCallback(int buttonIndex) const {
    if (buttonIndex < 0 || buttonIndex >= _numButtons) return false;
    return buttonCallbacks[buttonIndex] != nullptr;
}

#ifdef CF_TEST_CLI
void ButtonManager::injectEvent(int buttonIndex, ButtonEventType type) {
    if (buttonIndex < 0 || buttonIndex >= _numButtons) return;
    unsigned long now = millis();
    if (type == ButtonEvent_Pressed) {
        _injectedHold[buttonIndex]   = true;
        _currentState[buttonIndex]   = true;
        _pressStartTime[buttonIndex] = now;
        _heldReported[buttonIndex]   = false;
        _lastChangeTime[buttonIndex] = now;
        pushEvent(buttonIndex, ButtonEvent_Pressed, 0);
    } else if (type == ButtonEvent_Released) {
        _injectedHold[buttonIndex]   = false;
        _currentState[buttonIndex]   = false;
        _lastChangeTime[buttonIndex] = now;
        pushEvent(buttonIndex, ButtonEvent_Released, now - _pressStartTime[buttonIndex]);
    } else {
        return;  // Held is generated by update() timing, not injected
    }
    millis_APP_LASTINTERACTION = millis_NOW;  // same as a physical press
}
#endif

void ButtonManager::pushEvent(int buttonIndex, ButtonEventType type, unsigned long duration) {
    // Create an event
    ButtonEvent ev;
    ev.buttonIndex = buttonIndex;
    ev.eventType   = type;
    ev.duration    = duration;

    // Store it
    _events[_eventWriteIndex] = ev;
    // Move the index forward
    _eventWriteIndex = (_eventWriteIndex + 1) % MAX_EVENTS;

    // If the buffer is full, we essentially overwrite the oldest event
    // (if _eventWriteIndex == _eventReadIndex again).
}

// Check if a button was pressed
// Based on counters, which isn't a great way to do it
// these types of events should be triggered via state handshake so holds and releases can be accomodated
bool compareButtonCounters(volatile int* counter1, volatile int* counter2, int length) {
    for (int i = 0; i < length; i++) {
        if (counter1[i] != counter2[i]) {
        return false;
        }
    }
    return true;
}
  
// Button Use Counters
void ButtonManager::loadButtonCounters() {
    preferencesMainApp.begin("mainApp", true);

    // Retrieve each button counter value
    for (int i = 0; i < numButtons; i++) {
        buttonCounterSaved[i] = preferencesMainApp.getInt((String("button") + i).c_str(), 0); // Read the count back
        buttonCounter[i] = buttonCounterSaved[i]; // Keep the saved and lived counts synced
    }

    appActiveSaved = preferencesMainApp.getInt("appActive", 0);
    appActive = appActiveSaved;

    preferencesMainApp.end();
}

void ButtonManager::saveButtonCounters() {
    // Compare button counters
    if (compareButtonCounters(buttonCounter, buttonCounterSaved, numButtons) == false){
        ESP_LOGV(TAG_MAIN, "Button counters are not equal. Updating Saved Counters.");

        preferencesMainApp.begin("mainApp", false);

        // Store each button counter value
        for (int i = 0; i < numButtons; i++) {
        preferencesMainApp.putInt((String("button") + i).c_str(), buttonCounter[i]); // Write the live count
        buttonCounterSaved[i] = buttonCounter[i]; // Keep the saved and lived counts synced
        }

    preferencesMainApp.end();
    }

    if (appActive != appActiveSaved){
        preferencesMainApp.begin("mainApp", false);
        preferencesMainApp.putInt("appActive", appActive);
        preferencesMainApp.end();
    }
}