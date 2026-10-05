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

    // Notes, for built-in apps that want several sounds at once (chords).
    // Each note gets its own voice (never the one playTone uses) with the
    // same sound as playTone; when all are busy the oldest note is replaced.
    // playNote returns a handle (> 0), or -1 if the note could not start;
    // durationMs 0 = until stopNote. stopNote ends only that note - a handle
    // whose note was already replaced does nothing. stopNotes ends them all.
    // Leaving the app ends them too. Handles are never reused: after
    // 2^31 - 1 notes in one power cycle playNote returns -1.
    int  playNote(float frequency, int durationMs = 0);
    void stopNote(int handle);
    void stopNotes();

    // Sound is playing, played within the last second, or an app streams on
    // port 0. Cheap; for deferring flash writes that would stall audio.
    bool isAudioActive() const;

    // I2S port sharing — music player needs I2S0 for onboard speaker output.
    // releaseI2S: silence tones/sequences and free port 0. Returns false if
    // port 0 could not be freed - the caller must not open it then. A caller
    // that streams on port 0 passes its own stop function (ends its stream
    // and calls reclaimI2S); stopForSleep() uses it. That function must
    // return in bounded time (finite output waits); with hardShutdown true it
    // must not wait on the output at all.
    // reclaimI2S: take port 0 back for tones; on failure loop() keeps retrying.
    typedef void (*BorrowerStop)(bool hardShutdown);
    bool releaseI2S(BorrowerStop stopBorrower = nullptr);
    bool reclaimI2S();

    // Before deep sleep: stop whichever app streams on port 0 (its own stop
    // path), then stop the engine. True when port 0 is quiet and released.
    // From the first call on, the engine is never restarted (a deferred
    // sleep retries this instead).
    bool stopForSleep(bool hardShutdown = false);

    // Mic control
    void enableMic(bool on);
    bool isMicEnabled() const { return micEnabled; }

    // Mic level (consumer API)
    float getMicVolumeLinear() const { return micVolumeAtomic; }
    float getMicVolumeDb() const;

private:
    // --- Tone control state (the engine task renders; tone and sequence
    //     logic lives in AudioManager.cpp) ---
    float    volume = 0.7f;            // last setVolume(), re-applied on every engine start
    bool     engineWanted = false;     // set by init()/reclaimI2S(), cleared by releaseI2S()
    bool     volumePending = false;    // a volume change still to send
    uint32_t engineRetryAtMs = 0;
    BorrowerStop borrowerStop = nullptr;   // the current port-0 borrower's own stop
    bool     sleepHold = false;        // going to sleep: no engine restarts any more

    // --- Mic chain (RX) ---
    I2SConfig            micCfg;             // persisted RX config
    I2SStream            i2sIn;              // RX from ICS-43434
    VolumeMeter          micMeter;           // measures amplitude
    StreamCopy           micCopy;            // convIn -> micMeter

    bool startEngine();   // start the engine task and give it the volume

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