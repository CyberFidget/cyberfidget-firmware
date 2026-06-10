// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

// lib/VoiceRecorderApp/VoiceRecorderApp.h
//
// "Voice Notes" — record-first voice memo capture to SD as WAV.
// Owns I2S port 1 directly (AudioManager's mic task only opens the port
// when level metering is requested — same ownership precedent as
// MusicPlayerApp owning port 0). Capture task on core 0 feeds a PSRAM
// SPSC ring; update() drains the ring to /recordings/REC_NNNN.wav.

#ifndef VOICE_RECORDER_APP_H
#define VOICE_RECORDER_APP_H

#include <Arduino.h>
#include <Preferences.h>
#include <SD.h>
#include <atomic>

#include "HAL.h"
#include "AppDefs.h"
#include "ButtonManager.h"
#include "MenuManager.h"

// --- Audio Tools Includes (Diet Mode) ---
#include "AudioTools/CoreAudio/AudioLogger.h"
#include "AudioTools/CoreAudio/AudioI2S/I2SStream.h"
#include "AudioTools/AudioCodecs/CodecWAV.h"
#include "AudioTools/AudioCodecs/AudioEncoded.h"

#include "RecRingBuffer.h"
#include "RecNaming.h"

enum VoiceRecState {
    REC_STATE_HOME,        // tape-deck screen, reels frozen, ready to record
    REC_STATE_RECORDING,   // reels spinning, ring -> SD
    REC_STATE_SAVING,      // stop requested: drain ring, patch WAV header
    REC_STATE_SAVED_NOTE,  // brief "Saved REC_NNNN" note, then HOME
    REC_STATE_NO_SD,       // "NO TAPE" — card absent or mount failed
    REC_STATE_FAULT        // unrecoverable init failure (RAM / mic)
};

enum VoiceRecStopReason {
    REC_STOP_USER,
    REC_STOP_CARD_FULL,
    REC_STOP_WRITE_ERROR
};

class VoiceRecorderApp {
public:
    VoiceRecorderApp(ButtonManager& btnMgr);

    void begin();
    void update();
    void end();

    // Static button callbacks (dispatched from AppManager's main loop —
    // same context as update(), so no locking needed against it)
    static void onButtonEnter(const ButtonEvent& event);
    static void onButtonBack(const ButtonEvent& event);

private:
    static VoiceRecorderApp* instance;
    ButtonManager& buttonManager;

    VoiceRecState state = REC_STATE_HOME;
    VoiceRecStopReason stopReason = REC_STOP_USER;

    // --- Capture pipeline ---
    RecRingBuffer ring;
    uint8_t* ringStorage = nullptr;
    uint32_t ringCapacity = 0;
    audio_tools::I2SStream i2sIn;
    audio_tools::I2SConfig micCfg;
    bool i2sOpened = false;
    TaskHandle_t captureTaskHandle = nullptr;
    std::atomic<bool> exitRequested{false};
    std::atomic<bool> recordingActive{false};
    std::atomic<bool> captureTaskExited{true};
    std::atomic<uint16_t> vuPeak{0};
    uint8_t captureBuf[1024];   // capture task only
    uint8_t drainBuf[2048];     // update() only

    // --- Recording session ---
    File recFile;
    audio_tools::WAVEncoder wavEncoder;
    audio_tools::EncodedAudioOutput* pEncOut = nullptr;
    char recPath[40];
    uint32_t recCounter = 0;
    time_t recStartEpoch = 0;
    unsigned long recStartMs = 0;
    uint64_t bytesWritten = 0;
    bool writeErrorSeen = false;

    // --- SD / space ---
    bool sdMounted = false;
    uint64_t cachedFreeBytes = 0;

    // --- Save-note / fault UI ---
    uint32_t savedCounter = 0;
    uint32_t savedDroppedBytes = 0;
    unsigned long noteUntilMs = 0;
    const char* faultMsg = nullptr;
    const char* homeNoteMsg = nullptr;  // overrides the per-stop-reason note text

    // --- Gesture state ---
    bool armedHoldStop = false;  // true between record-start press and its release

    // --- UI state ---
    uint16_t uiVuLevel = 0;
    uint32_t frameCounter = 0;
    uint8_t reelAngle = 0;       // spoke rotation index, advances while recording
    uint8_t lastLedBright = 0xFF;

    // --- Helpers ---
    void handleEnterEvent(const ButtonEvent& event);
    void handleBackEvent(const ButtonEvent& event);
    bool mountSD();
    void enterHomeOrNoSd();
    void refreshFreeBytes();
    void startRecording();
    void requestStop(VoiceRecStopReason reason);
    uint32_t drainToFile(uint32_t maxBytes);
    bool closeRecordingFile();
    void finishSave();
    bool finalizeWav();
    void appendIndexRow();
    void showNote(const char* msg);
    void dismissNote();
    uint32_t remainingSeconds() const;
    static void captureTaskThunk(void* arg);
    void captureTaskLoop();

    // --- Rendering ---
    void render();
    void drawCassette(bool spinning, bool xOut);
    void drawHomeOrRecording();
    void drawSavedNote();
    void drawNoSd();
    void drawFault();
    void updateLed();
};

extern VoiceRecorderApp voiceRecorderApp;

#endif // VOICE_RECORDER_APP_H
