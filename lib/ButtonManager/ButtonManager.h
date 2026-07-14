// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#ifndef BUTTON_MANAGER_H
#define BUTTON_MANAGER_H

#include <Arduino.h>
#include "globals.h"

/**
 * @brief Different types of events a button can generate.
 */
enum ButtonEventType {
    ButtonEvent_None,    ///< No new event (used internally)
    ButtonEvent_Pressed, ///< The button was just pressed (transition LOW if active-low)
    ButtonEvent_Released,///< The button was just released (transition HIGH if active-low)
    ButtonEvent_Held     ///< The button was held longer than holdThreshold
};

/**
 * @brief A struct describing a single button event.
 */
struct ButtonEvent {
    int buttonIndex;           ///< Which button (0..N-1)
    ButtonEventType eventType; ///< The event type (Pressed, Released, Held)
    unsigned long duration;    ///< How long the button was pressed or held (ms)
};

/**
 * @brief Callback function type for handling button events.
 */

typedef void (*ButtonCallback)(const ButtonEvent&);

/**
 * @brief Manages debouncing, press, hold, and release logic for multiple buttons.
 */
class ButtonManager {
public:
    /**
     * @brief Constructor for the ButtonManager
     * 
     * @param pins Array of button pins
     * @param usePullups Array of booleans for each pin: true = INPUT_PULLUP, false = INPUT
     * @param numButtons How many buttons are being managed
     * @param debounceMs Debounce duration in milliseconds (default 20ms)
     * @param holdThresholdMs Time in ms after which a button press is considered "held" (default 1000ms)
     */
    ButtonManager(const int* pins,
                  const bool* usePullups,
                  int numButtons,
                  unsigned long debounceMs = 20,
                  unsigned long holdThresholdMs = 1000);

    /**
     * @brief Retrieve the callback function for a specific button.
     * 
     * @param buttonIndex The index of the button.
     * @return ButtonCallback The callback function pointer, or nullptr if none.
     */
    ButtonCallback getCallback(int buttonIndex) const;

    /**
     * @brief Call in setup() to configure pinModes, etc.
     */
    void init();

    /**
     * @brief Call frequently in loop() to update the button states and record any new events.
     */
    void update();

    /**
     * @brief Retrieve the next event from the internal queue.
     * 
     * @param event (out) A reference to a ButtonEvent struct where event data is stored
     * @return true if an event was retrieved, false if the queue is empty
     */
    bool getNextEvent(ButtonEvent &event);

    /**
     * @brief Check if a given button is currently pressed (active-low means digitalRead == LOW).
     * 
     * @param buttonIndex The index of the button to check
     * @return true if the button is in the pressed state, false otherwise
     */
    bool isPressed(int buttonIndex) const;

    /**
     * @brief Method to register a callback for a specific button.
     * 
     * @param buttonIndex The index of the button to register the callback for.
     * @param callback The callback function to be called when an event occurs for the specified button.
     */
    void registerCallback(int buttonIndex, ButtonCallback callback);
    void unregisterCallback(int buttonIndex);

#ifdef CF_TEST_CLI
    /**
     * @brief Test-only (CF_TEST_CLI): inject a button event as if the
     * physical pin had changed. Feeds the same event queue / callback path
     * physical presses use, so it works for native and wasm apps alike.
     * Pressed also latches the pressed state (isPressed() agrees, and Held
     * fires naturally if not released) until a Released injection.
     * Only ButtonEvent_Pressed / ButtonEvent_Released are supported.
     */
    void injectEvent(int buttonIndex, ButtonEventType type);
#endif

    // Getter methods
    int getNumButtons() const { return _numButtons; }
    bool hasCallback(int buttonIndex) const;

    /**
     * @brief Call to save button counters.
     */
    void saveButtonCounters();

    /**
     * @brief Call to load button counters.
     */
    void loadButtonCounters();
    
private:
    // --- Configuration ---
    const int* _pins;            ///< Array of button pins
    const bool* _usePullups;     ///< Whether each pin uses an internal pullup
    int _numButtons;             ///< Number of buttons
    unsigned long _debounceMs;   ///< Debounce time in ms
    unsigned long _holdThresholdMs; ///< Time in ms to consider a press as "held"

    // --- State tracking ---
    bool* _currentState;         ///< Current stable state (true = pressed, false = not pressed)
    bool* _previousState;        ///< Previous stable state
    unsigned long* _lastChangeTime; ///< Last time (ms) the reading changed
    unsigned long* _pressStartTime; ///< When the button was pressed (for press duration)
    bool* _heldReported;         ///< Whether we've already sent a "Held" event for this press
#ifdef CF_TEST_CLI
    bool* _injectedHold;         ///< Test-only: pin scan treats button as pressed while set
#endif

    // --- Event queue ---
    static const int MAX_EVENTS = 16;
    ButtonEvent _events[MAX_EVENTS];
    int _eventWriteIndex; // Where we write the next event
    int _eventReadIndex;  // Where the main code reads events

    // --- Callbacks ---
    static const int MAX_BUTTONS_SUPPORTED = numButtons;
    ButtonCallback buttonCallbacks[MAX_BUTTONS_SUPPORTED] = { nullptr };

    /**
     * @brief Helper to push a new event into the ring buffer.
     */
    void pushEvent(int buttonIndex, ButtonEventType type, unsigned long duration);
};

#endif
