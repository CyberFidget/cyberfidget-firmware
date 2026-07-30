// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#ifndef MUSIC_PLAYER_APP_H
#define MUSIC_PLAYER_APP_H

#include <Arduino.h>
#include <vector>
#include <Preferences.h>

#include "HAL.h"
#include "AppDefs.h"
#include "ButtonManager.h"
#include "MenuManager.h"
#include "BTScanner.h"
#include "ID3Scanner.h"

// --- Audio Tools Includes (Diet Mode) ---
#include "AudioTools/CoreAudio/AudioLogger.h"
#include "AudioTools/Disk/AudioSource.h"
#include "AudioTools/CoreAudio/AudioPlayer.h"
#include "AudioTools/AudioLibs/A2DPStream.h"
#include "AudioTools/CoreAudio/AudioStreams.h"
#include "AudioTools/Disk/AudioSourceIdxSD.h"
#include "AudioTools/AudioCodecs/CodecMP3Helix.h"
#include "AudioTools/CoreAudio/AudioI2S/I2SStream.h"
#include "AudioTools/CoreAudio/VolumeStream.h"
#include "SpectrumAnalyzer.h"

enum VisualizerMode : uint8_t {
    VIZ_OFF = 0,
    VIZ_AMPLITUDE = 1,
    VIZ_SPECTRUM = 2
};

enum LEDEffectMode : uint8_t {
    LED_OFF = 0,
    LED_REACTIVE = 1,
    LED_PULSE = 2
};

enum MusicAppState {
    STATE_DEVICE_MENU,      // Remembered devices + "Scan for new..."
    STATE_BT_SCANNING,      // Active GAP discovery
    STATE_BT_CONNECTING,    // A2DP connection attempt
    STATE_CONNECT_FAIL,     // Connection failed
    STATE_MAIN_MENU,        // iPod-style hub menu
    STATE_BT_SUBMENU,       // Bluetooth settings
    STATE_LED_SUBMENU,      // LED settings (mode + per-LED on/off)
    STATE_FILE_BROWSER,     // Browse songs with metadata
    STATE_PLAYER,           // Now Playing
    STATE_SCANNING_LIBRARY, // Scanning MP3 ID3 tags
    STATE_BT_SWITCHING      // Disconnecting old + connecting new device
};

struct SavedDevice {
    String name;
    esp_bd_addr_t address;
};

class MusicPlayerApp {
public:
    MusicPlayerApp(ButtonManager& btnMgr);

    void begin();
    void update();
    void end();

    // Static button callbacks
    static void onButtonUp(const ButtonEvent& event);
    static void onButtonDown(const ButtonEvent& event);
    static void onButtonLeft(const ButtonEvent& event);
    static void onButtonRight(const ButtonEvent& event);
    static void onButtonEnter(const ButtonEvent& event);
    static void onButtonBack(const ButtonEvent& event);

    // Metadata callback (needs static access)
    static void onMetadata(audio_tools::MetaDataType type, const char* str, int len);

private:
    static MusicPlayerApp* instance;
    ButtonManager& buttonManager;

    MusicAppState currentState = STATE_DEVICE_MENU;
    int menuCursorIndex = 0;
    int menuScrollOffset = 0;

    // BT scanning
    BTScanner btScanner;
    bool scannerActive = false;

    // Saved devices (NVS)
    std::vector<SavedDevice> savedDevices;
    void loadSavedDevices();
    void saveDevice(const String& name, esp_bd_addr_t address);
    void forgetAllDevices();
    void writeSavedDevicesToNVS();

    // BT connection
    String connectingDeviceName;
    String connectedDeviceName;
    esp_bd_addr_t connectedAddress = {0};  // for esp_a2d_source_disconnect()
    unsigned long connectStartTime = 0;
    static const unsigned long CONNECT_TIMEOUT_MS = 15000;
    bool btConnected = false;
    bool connectFailNeedsRestart = false;

    // Device switching (disconnect old → connect new)
    SavedDevice switchTargetDevice;
    unsigned long switchStartTime = 0;
    bool switchWaitingForDisconnect = true;

    // AVRCP remote command (set from BT task, consumed in update())
    volatile uint8_t pendingAvrcCmd = 0;

    // Audio output mode
    bool usingOnboardSpeaker = false;  // true = I2S DAC, false = BT A2DP

    // Audio pipeline (heap-allocated for lifecycle management)
    audio_tools::A2DPStream* pA2dpStream = nullptr;
    audio_tools::I2SStream* pI2sOut = nullptr;         // I2S output for onboard speaker
    audio_tools::VolumeStream* pI2sVolume = nullptr;   // Volume control for I2S output
    audio_tools::AudioSourceIdxSD* pSourceSD = nullptr;
    audio_tools::MP3DecoderHelix decoder;
    audio_tools::AudioPlayer* pPlayer = nullptr;
    SpectrumAnalyzer* pSpectrumAnalyzer = nullptr;
    bool sdAvailable = false;
    bool audioPipelineReady = false;

    // Audio amplitude (for LED effects + visualizer)
    float currentAmplitude = 0.0f;

    // SD card
    void initSD();

    // Connection lifecycle
    void startConnectingByAddress(const SavedDevice& dev);
    void startOnboardSpeaker();
    void stopOnboardSpeaker();
    void createAudioPipeline();
    void destroyAudioPipeline();
    void disconnectBT();
    void showBluetoothNeedsRestart();

    // Library / metadata
    std::vector<TrackInfo> trackLibrary;
    bool libraryScanned = false;
    int libraryScanCurrent = 0;
    int libraryScanTotal = 0;
    void startLibraryScan();

    // Playback
    bool isPlaying = false;
    bool shuffleEnabled = false;
    int currentTrackIndex = -1;
    String nowPlayingTitle;
    String nowPlayingArtist;
    void playTrack(int index);
    void stopPlayback();
    void nextTrack();
    void prevTrack();
    void togglePlayPause();
    void updateVolumeFromSlider();

    // Scrubbing (hold left/right to seek within track)
    int scrubButtonDir = 0;             // -1=left held, +1=right held, 0=not held
    bool scrubActive = false;
    unsigned long scrubPressTime = 0;
    unsigned long lastScrubTime = 0;
    static const unsigned long SCRUB_HOLD_MS = 500;   // ms before scrub starts
    static const unsigned long SCRUB_INTERVAL_MS = 200; // ms between scrub jumps
    static const int SCRUB_SECONDS = 5;                // seconds per scrub jump

    // Playhead / progress tracking
    unsigned long lastVolumeChangeTime = 0;
    int currentTrackBitrate = 0;        // bits/sec from MP3 header (0 = unknown)
    size_t currentTrackFileSize = 0;

    // Resume state (NVS)
    void savePlaybackState();
    void loadPlaybackState();
    void clearPlaybackState();
    String resumeTrackPath;
    uint32_t resumeBytePosition = 0;
    bool hasResumeState = false;

    // Marquee
    int marqueeOffset = 0;
    unsigned long lastMarqueeUpdate = 0;

    // LED effects
    LEDEffectMode ledEffectMode = LED_OFF;
    uint8_t ledEnableMask = 0x0F;  // bitmask: bit0=Back, bit1=FrontTop, bit2=FrontMid, bit3=FrontBot
    unsigned long lastLEDUpdate = 0;
    float ledSmoothedAmplitude = 0.0f;
    void updateLEDs();

    // OLED visualizer
    VisualizerMode visualizerMode = VIZ_OFF;
    float amplitudeHistory[16] = {0};
    int amplitudeHistoryIndex = 0;
    unsigned long lastVisualizerSample = 0;
    float spectrumBands[16] = {0};  // smoothed spectrum for display

    // Settings persistence (NVS)
    void saveSettings();
    void loadSettings();

    // Navigation
    void handleUp();
    void handleDown();
    void handleLeft();
    void handleRight();
    void handleEnter();
    void handleBack();
    void exitApp();

    int getListSize() const;
    void resetCursor();
    void setState(MusicAppState newState);

    // Main menu items
    static const int MAIN_MENU_COUNT = 6;
    String getMainMenuItem(int index) const;

    // BT submenu items
    int getBtSubMenuCount() const;
    String getBtSubMenuItem(int index) const;

    // LED submenu items
    int getLedSubMenuCount() const;
    String getLedSubMenuItem(int index) const;

    // Rendering
    void drawHeader(const String& title);
    void drawList(const std::vector<String>& items, int cursor, int scrollOffset);
    void drawScrollBar(int totalItems, int visibleItems, int scrollOffset);

    void renderDeviceMenu();
    void renderBtScanning();
    void renderConnecting();
    void renderConnectFail();
    void renderMainMenu();
    void renderBtSubMenu();
    void renderLedSubMenu();
    void renderFileBrowser();
    void renderPlayer();
    void renderScanningLibrary();
    void renderSwitching();
};

extern MusicPlayerApp musicPlayerApp;

#endif // MUSIC_PLAYER_APP_H
