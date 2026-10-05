// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

// lib/AudioManager/AudioManager.h
#ifndef AUDIO_MANAGER_H
#define AUDIO_MANAGER_H

#include <Arduino.h>
#include "AudioTools.h"
using namespace audio_tools;

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

class AudioManager {
public:
    // A single step in a tone sequence.
    // freq=0 means rest (silence). gapAfterMs adds inter-step silence.
    // The device copies up to 128 steps when playSequence() is called (longer
    // lists are cut to 128). Keep arrays static const anyway: the emulator
    // still reads them during playback.
    struct ToneStep {
        float    freq;
        uint16_t durationMs;
        uint16_t gapAfterMs;
    };

    AudioManager();

    // Tones and sequences are rendered by the audio engine's own task
    // (lib/AudioEngine, AudioEngineTask), which owns I2S port 0 from init()
    // on; their timing is sample-exact and independent of loop().
    void init();
    void loop(); // Kept for callers; the engine needs nothing from it

    // Tone control
    void setVolume(float volume);                       // 0.0..1.0
    void playTone(float frequency, int durationMs = 0); // 0 = indefinite
    void stopTone();

    // Sequence control — play a series of tones with timing.
    void playSequence(const ToneStep* steps, int count);
    void stopSequence();
    bool isSequencePlaying() const;

    // I2S port sharing — music player needs I2S0 for onboard speaker output
    void releaseI2S();   // Silence tones/sequences and free port 0 for another stream
    void reclaimI2S();   // Take port 0 back for tones

    // Mic control
    void enableMic(bool on);
    bool isMicEnabled() const { return micEnabled; }

    // Mic level (consumer API)
    float getMicVolumeLinear() const { return micVolumeAtomic; }
    float getMicVolumeDb() const;

private:
    // --- Tone and sequence control state (the engine task renders) ---
    float    volume = 0.7f;            // last setVolume(), re-applied on reclaimI2S()
    bool     i2sReleased = false;
    uint32_t seqGen = 0;               // tag of the last sequence play/stop sent
    bool     seqWanted = false;        // whether that command asked to play

    // --- Mic chain (RX) ---
    I2SConfig            micCfg;             // persisted RX config
    I2SStream            i2sIn;              // RX from ICS-43434
    VolumeMeter          micMeter;           // measures amplitude
    StreamCopy           micCopy;            // convIn -> micMeter

    // Mic State
    bool  micEnabled = false;
    volatile bool micRunRequested = false; // set by enableMic()
    bool micRunning = false; 

    // cross-core safe handoff, shared mic level (0..1), produced in mic task, read in loop/UI
    volatile float micVolumeAtomic = 0.0f;

    // Mic task
    static void micTaskThunk(void *arg);
    void micTaskLoop();
    TaskHandle_t micTaskHandle = nullptr;
    
};

#endif // AUDIO_MANAGER_H