// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

// lib/VoiceRecorderApp/VoiceRecorderApp.h
//
// "Voice Notes" — record-first voice memo capture to SD as WAV.
// The mic pipeline (I2S port 1 + capture task + PSRAM SPSC ring) lives in
// the shared lib/MicCapture/ service, acquired exclusively for the app's
// session; update() drains the ring to /recordings/REC_NNNN.wav.

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
#include "AudioTools/CoreAudio/VolumeStream.h"
#include "AudioTools/CoreAudio/StreamCopy.h"
#include "AudioTools/CoreAudio/AudioStreams.h"  // MemoryStream (SD-less demo playback)
#include "AudioTools/AudioCodecs/CodecWAV.h"
#include "AudioTools/AudioCodecs/AudioEncoded.h"

#include "MicCapture.h"
#include "RecNaming.h"

enum VoiceRecState {
    REC_STATE_HOME,         // tape-deck screen, reels frozen, ready to record
    REC_STATE_RECORDING,    // reels spinning, ring -> SD
    REC_STATE_SAVING,       // stop requested: drain ring, patch WAV header
    REC_STATE_SAVED_NOTE,   // brief "Saved REC_NNNN" note, then HOME
    REC_STATE_NO_SD,        // "NO TAPE" — card absent or mount failed
    REC_STATE_FAULT,        // unrecoverable init failure (RAM / mic)
    REC_STATE_LIST,         // browse recordings (newest first)
    REC_STATE_PLAYBACK,     // tape-deck "now playing": decode WAV -> speaker
    REC_STATE_CONFIRM_DELETE,// "Delete REC_NNNN?" guard before removing a file
    REC_STATE_DEMO_HOME,    // no card: ready to record a short PSRAM demo clip
    REC_STATE_DEMO_RECORDING// capturing the demo clip into PSRAM (bounded)
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
    static void onButtonEnter(const ButtonEvent& event);  // record / play / pause / confirm
    static void onButtonBack(const ButtonEvent& event);   // Select: universal "back"
    static void onButtonUp(const ButtonEvent& event);     // HOME: quality toggle; LIST: cursor up
    static void onButtonDown(const ButtonEvent& event);   // HOME: open list; LIST: cursor down
    static void onButtonRight(const ButtonEvent& event);  // LIST: delete (-> confirm)

private:
    static VoiceRecorderApp* instance;
    ButtonManager& buttonManager;

    VoiceRecState state = REC_STATE_HOME;
    VoiceRecStopReason stopReason = REC_STOP_USER;

    // --- Recording quality (persisted in NVS namespace "voicerec") ---
    // 0 = Standard (16kHz / 32KB/s), 1 = High (48kHz / 96KB/s); both 16-bit
    // mono. recSampleRate / recByteRate are derived by applyQuality() and are
    // the single source of truth for the I2S rate, WAV AudioInfo, and every
    // byte-rate-dependent UX calculation (no hardcoded 32000 in the math).
    uint8_t  recQuality    = 0;
    uint32_t recSampleRate = 16000;
    uint32_t recByteRate   = 32000;

    // --- Capture pipeline (shared lib/MicCapture service) ---
    // Acquired in begin(), released in end(); micHeld tracks the balance so
    // a FAULT entry (acquire failed) doesn't release someone else's session.
    bool micHeld = false;
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
    uint32_t tailTrimBytes = 0;   // header-level tail trim for press-stop clicks
    uint32_t finalDataBytes = 0;  // declared data-chunk length after finalize
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

    // --- Recordings list (parsed from /recordings/index.csv, newest first) ---
    // Source of truth is index.csv; the list holds the newest REC_LIST_MAX rows
    // (a card can hold far more memos than fit on screen / in RAM). listTotal is
    // the full row count so the header can show "cursor/total".
    struct RecListEntry {
        // Sized to RecNaming::kMaxRecNameLen: the recorder writes "REC_0042.wav"
        // (12), but a web-portal rename can grow it — too small a buffer would
        // make parseIndexRow drop the row and the note would vanish here.
        char     name[RecNaming::kMaxRecNameLen + 1];
        char     timestamp[20];  // "2026-06-09T14:23:11" or "" when clock unset
        uint32_t durationS;
        uint64_t bytes;
    };
    static const int REC_LIST_MAX = 64;
    RecListEntry listEntries[REC_LIST_MAX];
    int listCount = 0;           // entries actually loaded (<= REC_LIST_MAX)
    int listTotal = 0;           // total data rows in index.csv
    int listCursor = 0;          // selected index into listEntries
    int listScroll = 0;          // first visible row
    // Horizontal marquee for the selected row's name when it's wider than the
    // row (portal-renamed long names). Reset to the head on every cursor move.
    int listMarqueeOffset = 0;
    unsigned long lastMarqueeMs = 0;

    // --- Playback pipeline (REC_STATE_PLAYBACK) ---
    // File -> EncodedAudioStream(WAVDecoder) -> VolumeStream -> I2SStream port 0,
    // borrowing port 0 from AudioManager's tone chain via releaseI2S/reclaimI2S
    // exactly like MusicPlayerApp. The mic capture task keeps port 1 — the two
    // I2S peripherals are independent, so playback doesn't disturb capture.
    File playFile;
    // The decoder is allocated FRESH per playback (not a reused member): it
    // registers each playback stream into its own notify list and never clears
    // it, so a persistent decoder would dangle a pointer to the prior (freed)
    // stream and crash on the next header parse.
    audio_tools::WAVDecoder*         pWavDecoder = nullptr;
    audio_tools::EncodedAudioStream* pDecode = nullptr;
    audio_tools::VolumeStream*       pVol    = nullptr;
    audio_tools::I2SStream*          pI2sOut = nullptr;
    audio_tools::StreamCopy*         pCopier = nullptr;
    bool port0Held = false;          // true while we hold I2S port 0 (release/reclaim balance)
    bool playbackActive = false;     // true while actively copying (paused = false)
    bool playbackEof = false;        // reached end of file (reels frozen, progress full)
    char     playName[RecNaming::kMaxRecNameLen + 1] = "";  // file currently playing
    uint32_t playDurationS = 0;      // total duration (from the list entry) for progress
    uint64_t playBytes = 0;          // total PCM data bytes (from the list entry)
    uint32_t playByteRate = 0;       // file's actual byte rate (from WAV header) for the pump budget
    unsigned long playStartMs = 0;
    float lastPlayVol = -1.0f;
    unsigned long volOverlayUntilMs = 0;  // briefly show the volume bar after a slider move

    // --- SD-less demo (REC_STATE_DEMO_*) ---
    // When no card is present, let the user taste record -> replay with a short
    // clip held in PSRAM (raw PCM, no WAV/index — the SD pipeline is untouched).
    // PSRAM is lost at deep sleep / power-off, so the clip is volatile by design
    // and the UI says so ("DEMO - not saved"). The buffer is allocated lazily on
    // demo entry and freed on app exit or when a card is inserted. Playback
    // reuses REC_STATE_PLAYBACK with demoMode = true (MemoryStream source, no
    // decoder); demoMode also routes "back" to DEMO_HOME instead of the list.
    bool demoMode = false;
    uint8_t* demoBuf = nullptr;
    uint32_t demoCapacity = 0;        // allocated buffer size (the byte cap)
    uint32_t demoLen = 0;             // bytes recorded so far
    uint32_t demoByteRate = 0;        // rate the clip was captured at (for duration + playback clock)
    uint32_t demoSampleRate = 0;
    audio_tools::MemoryStream* pMemStream = nullptr;  // demo playback source

    // --- Helpers ---
    void handleEnterEvent(const ButtonEvent& event);
    void handleBackEvent(const ButtonEvent& event);
    void handleUpEvent(const ButtonEvent& event);    // HOME quality / LIST cursor up
    void handleDownEvent(const ButtonEvent& event);  // HOME open list / LIST cursor down
    void handleRightEvent(const ButtonEvent& event); // LIST delete
    void applyQuality(uint8_t q);   // set recQuality + derived rates
    void toggleQuality();           // flip, persist, request I2S re-clock
    bool mountSD();
    void enterHomeOrNoSd();
    void refreshFreeBytes();
    void startRecording();
    void requestStop(VoiceRecStopReason reason, bool trimPressClick = false);
    uint32_t drainToFile(uint32_t maxBytes);
    bool closeRecordingFile();
    void finishSave();
    bool finalizeWav();
    void appendIndexRow();
    void showNote(const char* msg);
    void dismissNote();
    uint32_t remainingSeconds() const;

    // --- Recordings list (REC_STATE_LIST) ---
    void enterList();
    void loadRecordingsList();        // (re)read /recordings/index.csv, newest first
    void listMoveCursor(int delta);

    // --- Playback (REC_STATE_PLAYBACK) ---
    void startPlayback(int listIdx);
    void stopPlayback();              // tear down pipeline, reclaim I2S, back to LIST
    void restartPlaybackStream();     // replay from the top (after EOF)
    bool buildPlaybackPipeline(const char* path);
    void destroyPlaybackPipeline();
    void updatePlaybackVolume();

    // --- Delete (REC_STATE_CONFIRM_DELETE) ---
    void performDelete(int listIdx);  // remove wav + index row + transcript sidecar
    bool deleteIndexRow(const char* filename);

    // --- SD-less demo (REC_STATE_DEMO_*) ---
    void enterDemo();                 // alloc PSRAM buffer, -> DEMO_HOME
    void exitDemo();                  // free buffer, clear demoMode (card found / app exit)
    void startDemoRecording();
    void stopDemoRecording();
    void drainDemo();                 // ring -> demoBuf until the cap
    void startDemoPlayback();
    uint32_t demoLimitSeconds() const;

    // --- Rendering ---
    void render();
    void drawCassette(bool spinning, bool xOut);
    void drawHomeOrRecording();
    void drawSavedNote();
    void drawNoSd();
    void drawFault();
    void drawList();
    void drawPlayback();
    void drawConfirmDelete();
    void drawDemo();
    void updateLed();
};

extern VoiceRecorderApp voiceRecorderApp;

#endif // VOICE_RECORDER_APP_H
