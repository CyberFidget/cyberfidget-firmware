// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#include "Booper.h"
#include "globals.h" // For slider position and button indices
#include "HAL.h"  // Use our proxy header
#include "MenuManager.h" // AppManager integration
#include "RGBController.h"

Booper booper(HAL::buttonManager(), HAL::audioManager());

Booper* Booper::instance = nullptr;

Booper::Booper(ButtonManager& btnMgr, AudioManager& audioMgr) :
    buttonManager(btnMgr),
    audioManager(audioMgr),
    display(HAL::displayProxy()),  // Use the DisplayProxy from HAL
    volume(0.5f),
    octave(0),
    toneStopTime(0)
{
    ESP_LOGI(TAG_MAIN, "Booper constructor! volume=%.2f, &this=%p", volume, this);
    instance = this;
    for (int i = 0; i < kButtonCount; ++i) noteHandle[i] = -1;
}

void Booper::stopAllNotes() {
    audioManager.stopNotes();
    for (int i = 0; i < kButtonCount; ++i) noteHandle[i] = -1;
}

void Booper::begin() {
    ESP_LOGI(TAG_MAIN, "begin() => registering booper callbacks...");
    registerButtonCallbacks();
    audioManager.enableMic(true); // Enable mic input
}

void Booper::end() {
    ESP_LOGI(TAG_MAIN, "end() => unregistering booper callbacks...");
    
    unregisterButtonCallbacks();
    // Stop any playing notes
    stopAllNotes();
    audioManager.enableMic(false); // Disable mic input
    // Turn off LEDs when leaving the app
    setColorsOff();
}

void Booper::update() {
    // Update volume from slider
    updateVolumeFromSlider();

    // --- Read mic levels once ---
    const float micLin = audioManager.getMicVolumeLinear(); // 0..1
    const float micDb  = audioManager.getMicVolumeDb();     // dBFS (<=0)

    // ---- Choose your feel: dB or linear ----
    // Flip this flag to try the other curve
    constexpr bool USE_DB = false;

    float norm = 0.0f; // normalized 0..1 brightness input (before smoothing)

    if (USE_DB) {
        // Map dBFS window [-60 .. -5] -> [0 .. 1]
        // Adjust these to taste
        const float dbMin = -60.0f;  // gate below this => 0
        const float dbMax =  -5.0f;  // near full-scale speech/claps
        norm = (micDb - dbMin) / (dbMax - dbMin);
    } else {
        // Map linear window [linMin .. linMax] -> [0 .. 1]
        // linMin is your noise floor, linMax is “loud”
        const float linMin = 0.02f;  // gate
        const float linMax = 0.60f;  // strong input
        norm = (micLin - linMin) / (linMax - linMin);
    }

    // Clamp
    if (norm < 0.0f) norm = 0.0f;
    if (norm > 1.0f) norm = 1.0f;

    // Perceptual curve (optional): <1.0 makes it poppier/snappier
    const float gamma = 0.6f;
    float curved = powf(norm, gamma);

    // Smooth (EMA)
    static float smooth = 0.0f;
    const float alpha = 0.25f; // raise for faster attack
    smooth = (1.0f - alpha) * smooth + alpha * curved;

    // Apply to LEDs (white only); HAL loop should call updateStrip()
    const uint8_t brightness = (uint8_t)lroundf(smooth * 255.0f);
    setDeterminedColorsAll(0, 0, 0, brightness);

    // --- UI ---
    display.clear();
    display.setTextAlignment(TEXT_ALIGN_CENTER);
    display.setFont(ArialMT_Plain_10);
    display.drawString(64, 10, "Booper");
    display.drawString(64, 22, "Volume: " + String((int)(volume * 100)) + "%");
    display.drawString(64, 34, "Octave: " + String(octave));
    display.drawString(64, 46, "Mic: " + String(micLin, 3) + " | " + String(micDb, 1) + " dBFS");
    display.display();
}

void Booper::registerButtonCallbacks() {
    // Register callbacks for the buttons
    buttonManager.registerCallback(button_TopLeftIndex,     buttonPressedCallback);
    buttonManager.registerCallback(button_TopRightIndex,    buttonPressedCallback);
    buttonManager.registerCallback(button_MiddleLeftIndex,  buttonPressedCallback);
    buttonManager.registerCallback(button_MiddleRightIndex, buttonPressedCallback);

    // Exit App
    buttonManager.registerCallback(button_BottomLeftIndex, onButtonBackPressed);
    buttonManager.registerCallback(button_BottomRightIndex,onButtonSelectPressed);
}

void Booper::unregisterButtonCallbacks() {
    // Unregister callbacks
    buttonManager.unregisterCallback(button_TopLeftIndex);
    buttonManager.unregisterCallback(button_TopRightIndex);
    buttonManager.unregisterCallback(button_MiddleLeftIndex);
    buttonManager.unregisterCallback(button_MiddleRightIndex);
    buttonManager.unregisterCallback(button_BottomLeftIndex);
    buttonManager.unregisterCallback(button_BottomRightIndex);
}

void Booper::buttonPressedCallback(const ButtonEvent& event) {
    if (instance) {
        instance->handleButtonEvent(event);
    }
}

void Booper::handleButtonEvent(const ButtonEvent& event) {
    // Handle button events
    // Each held button plays its own note, so pressing several makes a chord.
    const int b = event.buttonIndex;
    if (b < 0 || b >= kButtonCount) return;
    if (event.eventType == ButtonEvent_Pressed) {
        if (noteHandle[b] > 0) audioManager.stopNote(noteHandle[b]);
        float freq = getFrequencyForButton(b);
        noteHandle[b] = audioManager.playNote(freq);
    } else if (event.eventType == ButtonEvent_Released) {
        // Stop this button's note when it is released
        if (b == button_TopLeftIndex ||
            b == button_TopRightIndex ||
            b == button_MiddleLeftIndex ||
            b == button_MiddleRightIndex) {
            if (noteHandle[b] > 0) audioManager.stopNote(noteHandle[b]);
            noteHandle[b] = -1;
        }
    }
}

float Booper::getFrequencyForButton(int buttonIndex) {
    // Base frequencies for buttons
    float baseFrequencies[] = { 261.63f, 293.66f, 329.63f, 349.23f }; // C4, D4, E4, F4

    int buttonOrder[] = {
        button_TopLeftIndex,
        button_TopRightIndex,
        button_MiddleLeftIndex,
        button_MiddleRightIndex
    };

    // Find index of the button
    int idx = -1;
    for (int i = 0; i < 4; i++) {
        if (buttonIndex == buttonOrder[i]) {
            idx = i;
            break;
        }
    }

    if (idx >= 0) {
        // Adjust frequency based on octave
        float freq = baseFrequencies[idx] * pow(2, octave);
        return freq;
    } else {
        return 440.0f; // Default frequency
    }
}



void Booper::adjustOctave(int delta) {
    octave += delta;
    // Constrain octave between -2 and +2, for example
    octave = constrain(octave, -2, 2);
}

void Booper::updateVolumeFromSlider() {
    // Assume sliderPosition_Percentage_Filtered is a global variable [0, 100]
    volume = sliderPosition_Percentage_Inverted_Filtered / 100.0f;
    audioManager.setVolume(volume);
}



void Booper::onButtonBackPressed(const ButtonEvent& event)
{    // Press
    if (event.eventType == ButtonEvent_Released){
        ESP_LOGI(TAG_MAIN, "onButtonBackPressed => calling end() + returning to menu...");
        instance->end();
        MenuManager::instance().returnToMenu();
    } 
}
void Booper::onButtonSelectPressed(const ButtonEvent& event)
{    
    // Press
    if (event.eventType == ButtonEvent_Pressed){
        //instance().selectCurrentItem();
    }
}
