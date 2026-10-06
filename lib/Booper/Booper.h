// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#ifndef BOOPER_H
#define BOOPER_H

#include "ButtonManager.h"
#include "AudioManager.h"
#include "DisplayProxy.h"
#include "BooperChords.h"

class Booper {
public:
    Booper(ButtonManager& btnMgr, AudioManager& audioMgr);

    void begin();
    void update(); // Call this in the main loop
    void end();    // Unregister callbacks and cleanup

private:
    ButtonManager& buttonManager;
    AudioManager& audioManager;
    DisplayProxy& display;

    float volume; // Volume level (0.0 to 1.0)
    int octave;   // Octave shift for tones
    int chordSet = 0; // Index into kBooperChordSets; RAM only, resets on reboot
    void cycleChordSet();

    void registerButtonCallbacks();
    void unregisterButtonCallbacks();

    // Button callback functions
    static void buttonPressedCallback(const ButtonEvent& event);
    void handleButtonEvent(const ButtonEvent& event); // Define this function

    // Helper methods
    float getFrequencyForButton(int buttonIndex);
    void adjustOctave(int delta);

    // For handling button presses
    static Booper* instance; // To allow static callbacks

    // Volume control via slider
    void updateVolumeFromSlider();

    // Time to stop tone after button release
    unsigned long toneStopTime;

    // The note each held button is playing (AudioManager::playNote handle,
    // -1 = none), indexed by button index, so chords work.
    static constexpr int kButtonCount = 6;
    int noteHandle[kButtonCount];
    void stopAllNotes();
    static const unsigned long TONE_DURATION_MS = 0; // 0 for continuous tone while button is held

    // We will have one callback function for each button
    static void onButtonLeftPressed(const ButtonEvent& event);
    static void onButtonRightPressed(const ButtonEvent& event);
    static void onButtonUpPressed(const ButtonEvent& event);
    static void onButtonDownPressed(const ButtonEvent& event);
    static void onButtonBackPressed(const ButtonEvent& event);
    static void onButtonSelectPressed(const ButtonEvent& event);
};

// Declare that there's a global object called below somewhere for AppDefs to use
// must be after the reference class definition exists
extern Booper booper;

#endif // BOOPER_H