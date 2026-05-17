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
    // Pointers passed to playSequence() must outlive playback — use static const arrays.
    struct ToneStep {
        float    freq;
        uint16_t durationMs;
        uint16_t gapAfterMs;
    };

    AudioManager();

    void init();
    void loop(); // Call this regularly to process audio

    // Tone control
    void setVolume(float volume);                       // 0.0..1.0
    void playTone(float frequency, int durationMs = 0); // 0 = indefinite
    void stopTone();

    // Sequence control — play a series of tones with timing.
    void playSequence(const ToneStep* steps, int count);
    void stopSequence();
    bool isSequencePlaying() const { return currentSequence != nullptr; }
    
    // I2S port sharing — music player needs I2S0 for onboard speaker output
    void releaseI2S();   // Stop I2S TX so another stream can use port 0
    void reclaimI2S();   // Restart I2S TX for tone generation

    // Mic control
    void enableMic(bool on);
    bool isMicEnabled() const { return micEnabled; }

    // Mic level (consumer API)
    float getMicVolumeLinear() const { return micVolumeAtomic; }
    float getMicVolumeDb() const;

private:
    // --- Tone state ---
    float currentFrequency;
    bool  isPlaying;
    unsigned long stopAtMillis;

    // --- Sequence state ---
    const ToneStep* currentSequence = nullptr;
    int             currentSequenceLen = 0;
    int             currentSequenceIdx = 0;
    unsigned long   nextStepAtMs = 0;

    // --- Tone chain (TX) ---
    I2SStream i2s;                           // TX to MAX98357A
    SineWaveGenerator<int16_t> generator;
    GeneratedSoundStream<int16_t> in;
    VolumeStream volume;
    StreamCopy copier;                       // volume -> i2s

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