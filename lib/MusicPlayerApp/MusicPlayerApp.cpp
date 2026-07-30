// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#include "MusicPlayerApp.h"
#include "AudioManager.h"
#include "RGBController.h"
#include "WebPortalApp.h"
#include "globals.h"        // millis_APP_LASTINTERACTION (sleep prevention)
#include <SD.h>
#include <SPI.h>
#include <esp_a2dp_api.h>  // esp_a2d_source_disconnect() — the correct API for source mode
#include <esp_avrc_api.h>  // ESP_AVRC_PT_CMD_* — AVRCP passthrough key codes

using namespace audio_tools;

// ---------------------------------------------------------------------------
// Debug logging — set to 1 to enable verbose serial output for BT lifecycle,
// audio pipeline, and state machine transitions. Invaluable for diagnosing
// the many edge cases in ESP32-A2DP + audio-tools interaction.
//
// Key findings documented here for future developers:
//
// ESP32-A2DP Library Constraints:
//   - BluetoothA2DPSource's internal FreeRTOS task (BtAppT) and heartbeat
//     timer are never shut down. The global `self_BluetoothA2DPSource` pointer
//     is never cleared. Deleting A2DPStream after playback causes crashes.
//   - The heartbeat timer fires every ~10s. In UNCONNECTED state, it
//     unconditionally calls esp_a2d_connect(peer_bd_addr). The library's
//     set_auto_reconnect(false) flag is NOT checked by this handler.
//     We patched BluetoothA2DPSource.cpp to add the missing guard.
//   - BluetoothA2DPCommon::disconnect() calls esp_a2d_sink_disconnect() —
//     the WRONG API for source mode. Use esp_a2d_source_disconnect() directly.
//   - A2DPStream, AudioPlayer, AudioSourceIdxSD, and the decoder must all
//     be kept alive across app enter/exit cycles. Recreating any of them
//     after active playback causes crashes due to intertwined internal state.
//
// A2DPStream BufferRTOS Blocking:
//   - The shared a2dp_buffer uses portMAX_DELAY for both read and write.
//     When is_a2dp_active is true and the buffer is empty (player stopped),
//     the data callback blocks indefinitely on xStreamBufferReceive(),
//     tying up the BTC task. Since esp_a2d_source_disconnect() dispatches
//     through the same BTC task, it hangs.
//     Fix: temporarily reduce readMaxWait to 50ms (so the callback returns
//     quickly on empty buffer), write silence to wake any currently-pending
//     read, then disconnect. Restore portMAX_DELAY on reconnect.
//     Do NOT use clear() — it sets is_a2dp_active=false, which stops the
//     callback from draining the buffer, causing writes to block on reconnect.
//
// AudioPlayer::stop() / setActive(false):
//   - When auto-fade is enabled (default), stop() writes 2KB+ of silence
//     to the output stream (A2DPStream). After multiple BT disconnect/
//     reconnect cycles, these writes hang due to accumulated BT stack state
//     corruption. Fix: disable auto-fade before stopping.
//
// AppManager Integration:
//   - AppManager::switchToApp() calls endFunc() on the old app automatically.
//     Don't call end() manually in exitApp() — double end() causes double
//     stop()/disconnect() which accelerates the hang.
// ---------------------------------------------------------------------------
#define MUSIC_PLAYER_DEBUG 1

// Disconnect BT on normal app exit? The ESP32-A2DP BT stack accumulates
// internal state corruption over repeated disconnect/reconnect cycles,
// eventually causing pPlayer->copy() to hang when writing to A2DPStream.
// Set to 0 (default) to keep BT connected on exit — stable and fast re-entry.
// Set to 1 to disconnect on exit — cleaner but may hang after ~5 cycles.
// Explicit disconnect via BT submenu always works regardless of this flag.
#define MUSIC_PLAYER_DISCONNECT_ON_EXIT 0

#if MUSIC_PLAYER_DEBUG
  #define MPLAYER_LOG(msg) ESP_LOGD(TAG_MAIN, "[MusicPlayer] " msg)
  #define MPLAYER_LOGF(fmt, ...) ESP_LOGD(TAG_MAIN, "[MusicPlayer] " fmt, ##__VA_ARGS__)
#else
  #define MPLAYER_LOG(msg) ((void)0)
  #define MPLAYER_LOGF(fmt, ...) ((void)0)
#endif

// SD Card SPI Pins
static const int PIN_SD_CLK  = 5;
static const int PIN_SD_MISO = 21;
static const int PIN_SD_MOSI = 19;
static const int PIN_SD_CS   = 8;

static const char* MEDIA_DIR = "/media";
static const char* CACHE_PATH = "/music.idx";

// Bluetooth rune icon (7x10, XBM format) — shown in player header when connected
#define BT_ICON_WIDTH 7
#define BT_ICON_HEIGHT 10
static const uint8_t bt_icon_bits[] PROGMEM = {
    0x08,  // ...X....
    0x0C,  // ..XX....
    0x2A,  // .X.X.X..
    0x1C,  // ..XXX...
    0x08,  // ...X....
    0x08,  // ...X....
    0x1C,  // ..XXX...
    0x2A,  // .X.X.X..
    0x0C,  // ..XX....
    0x08,  // ...X....
};

// Lightning bolt icon (5x8, XBM format) — shown next to battery when charging
#define BOLT_ICON_WIDTH 5
#define BOLT_ICON_HEIGHT 8
static const uint8_t bolt_icon_bits[] PROGMEM = {
    0x10,  // ....X
    0x08,  // ...X.
    0x0C,  // ..XX.
    0x1F,  // XXXXX
    0x1F,  // XXXXX
    0x06,  // .XX..
    0x02,  // .X...
    0x01,  // X....
};
static const int MAX_VISIBLE_ITEMS = 4;
static const int ITEM_HEIGHT = 12;
static const int LIST_Y_START = 16;

MusicPlayerApp* MusicPlayerApp::instance = nullptr;
MusicPlayerApp musicPlayerApp(HAL::buttonManager());
static auto& display = HAL::displayProxy();

// =========================================================================
// Constructor
// =========================================================================
MusicPlayerApp::MusicPlayerApp(ButtonManager& btnMgr)
    : buttonManager(btnMgr), decoder()
{
    instance = this;
}

// =========================================================================
// Lifecycle
// =========================================================================
void MusicPlayerApp::begin() {
    currentState = STATE_DEVICE_MENU;
    menuCursorIndex = 0;
    menuScrollOffset = 0;
    isPlaying = false;
    btConnected = false;
    usingOnboardSpeaker = false;
    // NOTE: audioPipelineReady is NOT reset — the pipeline persists across
    // app enter/exit cycles. Recreating it after active playback crashes.
    nowPlayingTitle = "";
    nowPlayingArtist = "";
    connectingDeviceName = "";
    connectedDeviceName = "";
    connectFailNeedsRestart = false;
    currentTrackIndex = -1;
    marqueeOffset = 0;

    // Clear any LEDs left over from a previous app
    setColorsOff();

    // Register all 6 button callbacks
    buttonManager.registerCallback(button_UpIndex, onButtonUp);
    buttonManager.registerCallback(button_DownIndex, onButtonDown);
    buttonManager.registerCallback(button_LeftIndex, onButtonLeft);
    buttonManager.registerCallback(button_RightIndex, onButtonRight);
    buttonManager.registerCallback(button_EnterIndex, onButtonEnter);
    buttonManager.registerCallback(button_SelectIndex, onButtonBack);

    // Init SD card
    initSD();

    // Load saved BT devices from NVS
    loadSavedDevices();

    // Load resume state (track path + byte position from last session)
    loadPlaybackState();

    // Load persistent settings (shuffle, LED mode, visualizer)
    loadSettings();

    // Reset scrub state
    scrubButtonDir = 0;
    scrubActive = false;
    lastVolumeChangeTime = 0;
    currentTrackBitrate = 0;
    currentTrackFileSize = 0;

    // Always start at device menu — user picks which device to connect to.
    // Auto-reconnect on startup was causing BT stack crashes when the
    // target device wasn't in pairing mode.
    setState(STATE_DEVICE_MENU);
}

void MusicPlayerApp::end() {
    MPLAYER_LOG("end: enter");
    saveSettings();       // Persist shuffle, LED mode, visualizer toggle
    savePlaybackState();  // Remember position for resume on next launch
    setColorsOff();       // Turn off LEDs on app exit
    stopPlayback();
    destroyAudioPipeline();

    if (scannerActive) {
        btScanner.end();
        scannerActive = false;
    }

    // Unregister all button callbacks
    buttonManager.unregisterCallback(button_UpIndex);
    buttonManager.unregisterCallback(button_DownIndex);
    buttonManager.unregisterCallback(button_LeftIndex);
    buttonManager.unregisterCallback(button_RightIndex);
    buttonManager.unregisterCallback(button_EnterIndex);
    buttonManager.unregisterCallback(button_SelectIndex);
}

void MusicPlayerApp::update() {
    // Keep device awake during music playback (AppManager sleeps after 60s inactivity)
    if (isPlaying) {
        millis_APP_LASTINTERACTION = millis_NOW;
    }

    // Detect BT reconnect (heartbeat auto-reconnect recovered the connection)
    if (!usingOnboardSpeaker && pA2dpStream && pA2dpStream->isConnected() && !btConnected) {
        MPLAYER_LOG("update: BT reconnected (heartbeat recovery)");
        btConnected = true;
    }

    // Dispatch AVRCP commands from BT speaker (set from BT task callback)
    if (pendingAvrcCmd != 0) {
        uint8_t cmd = pendingAvrcCmd;
        pendingAvrcCmd = 0;
        MPLAYER_LOGF("AVRCP cmd: 0x%02x", cmd);
        switch (cmd) {
            case ESP_AVRC_PT_CMD_PLAY:
            case ESP_AVRC_PT_CMD_PAUSE:
                togglePlayPause();
                break;
            case ESP_AVRC_PT_CMD_STOP:
                stopPlayback();
                break;
            case ESP_AVRC_PT_CMD_FORWARD:
                nextTrack();
                break;
            case ESP_AVRC_PT_CMD_BACKWARD:
                prevTrack();
                break;
        }
    }

    // Scrub detection: if left/right held past threshold, enter scrub mode
    if (scrubButtonDir != 0 && !scrubActive &&
        millis() - scrubPressTime > SCRUB_HOLD_MS) {
        scrubActive = true;
        lastScrubTime = millis();
        MPLAYER_LOGF("scrub: started, dir=%d", scrubButtonDir);
    }

    // Scrub execution: periodically seek while button is held
    if (scrubActive && pPlayer && pPlayer->getStream() &&
        millis() - lastScrubTime > SCRUB_INTERVAL_MS) {
        File* f = (File*)pPlayer->getStream();
        size_t currentPos = f->position();
        size_t fileSize = f->size();
        size_t skipBytes = (currentTrackBitrate > 0)
            ? (size_t)((long long)currentTrackBitrate / 8 * SCRUB_SECONDS)
            : 50000;  // ~50KB fallback
        size_t newPos;
        if (scrubButtonDir > 0) {
            newPos = (currentPos + skipBytes < fileSize) ? currentPos + skipBytes : fileSize - 1;
        } else {
            newPos = (currentPos > skipBytes) ? currentPos - skipBytes : 0;
        }
        f->seek(newPos);
        lastScrubTime = millis();
        MPLAYER_LOGF("scrub: %s to %u / %u",
            scrubButtonDir > 0 ? "fwd" : "rev", (unsigned)newPos, (unsigned)fileSize);
    }

    // Drive audio pipeline only while playing
    if (isPlaying && audioPipelineReady && pPlayer) {
        // Detect BT disconnect during playback — stop before copy() blocks
        // (not applicable when using onboard speaker)
        if (!usingOnboardSpeaker && pA2dpStream && !pA2dpStream->isConnected()) {
            MPLAYER_LOG("update: BT disconnected during playback, stopping");
            stopPlayback();
            btConnected = false;
        } else {
            updateVolumeFromSlider();
            pPlayer->copy();

            // Sample amplitude + spectrum for LED effects + visualizer
            // Use delayed values to compensate for BT audio latency
            if (pSpectrumAnalyzer) {
                pSpectrumAnalyzer->spectrumEnabled = (visualizerMode == VIZ_SPECTRUM);
                currentAmplitude = pSpectrumAnalyzer->delayedVolumeRatio();

                if (visualizerMode == VIZ_AMPLITUDE && millis() - lastVisualizerSample > 60) {
                    amplitudeHistory[amplitudeHistoryIndex] = currentAmplitude;
                    amplitudeHistoryIndex = (amplitudeHistoryIndex + 1) % 16;
                    lastVisualizerSample = millis();
                    // Store amplitude in delay buffer (FFT not running in this mode)
                    pSpectrumAnalyzer->storeDelayFrame();
                } else if (visualizerMode == VIZ_SPECTRUM) {
                    // Run pending FFT here in main loop (not in audio write path)
                    // processFFT() also stores bands + amplitude into delay buffer
                    if (pSpectrumAnalyzer->processFFT()) {
                        const float* raw = pSpectrumAnalyzer->delayedBands();
                        for (int i = 0; i < 16; i++) {
                            // Fast attack, slow decay (VFD-style)
                            if (raw[i] > spectrumBands[i]) spectrumBands[i] = raw[i];
                            else spectrumBands[i] = spectrumBands[i] * 0.85f + raw[i] * 0.15f;
                        }
                    }
                } else {
                    // VIZ_OFF — still store amplitude for LED reactive mode
                    pSpectrumAnalyzer->storeDelayFrame();
                }
            }

            // Check if track ended
            if (!pPlayer->isActive()) {
                isPlaying = false;
                // Auto-advance
                if (shuffleEnabled) {
                    int total = trackLibrary.size();
                    if (total > 1) {
                        int next = random(0, total);
                        if (next == currentTrackIndex) next = (next + 1) % total;
                        playTrack(next);
                    }
                } else if (currentTrackIndex + 1 < (int)trackLibrary.size()) {
                    playTrack(currentTrackIndex + 1);
                }
            }
        }
    }

    // Update music-reactive LEDs
    updateLEDs();

    // State-specific polling
    switch (currentState) {
        case STATE_BT_CONNECTING:
            if (pA2dpStream && pA2dpStream->isConnected()) {
                btConnected = true;
                connectedDeviceName = connectingDeviceName;
                // Save address for proper source-mode disconnect later
                for (auto& d : savedDevices) {
                    if (d.name == connectingDeviceName) {
                        memcpy(connectedAddress, d.address, ESP_BD_ADDR_LEN);
                        break;
                    }
                }
                setState(STATE_MAIN_MENU);
            } else if (millis() - connectStartTime > CONNECT_TIMEOUT_MS) {
                // Timeout — clean up and show failure
                destroyAudioPipeline();
                connectFailNeedsRestart = false;
                setState(STATE_CONNECT_FAIL);
            }
            break;

        case STATE_BT_SWITCHING:
            // Phase 1: wait for old connection to drop
            if (switchWaitingForDisconnect) {
                if (!pA2dpStream || !pA2dpStream->isConnected()) {
                    MPLAYER_LOG("update: switch — old device disconnected");
                    switchWaitingForDisconnect = false;
                    switchStartTime = millis(); // start settle timer
                } else if (millis() - switchStartTime > 3000) {
                    MPLAYER_LOG("update: switch — disconnect timeout, proceeding anyway");
                    switchWaitingForDisconnect = false;
                    switchStartTime = millis();
                }
            }
            // Phase 2: settle time (let BT stack fully clean up)
            else if (millis() - switchStartTime >= 1000) {
                MPLAYER_LOG("update: switch — settle complete, connecting to new device");
                // Restore blocking read timeout for the new connection
                auto& buf = static_cast<audio_tools::BufferRTOS<uint8_t>&>(pA2dpStream->buffer());
                buf.setReadMaxWait(portMAX_DELAY);
                // Point library at new address and reconnect
                pA2dpStream->source().set_auto_reconnect(
                    const_cast<uint8_t*>(switchTargetDevice.address));
                pA2dpStream->source().reconnect();
                connectingDeviceName = switchTargetDevice.name;
                createAudioPipeline();
                connectStartTime = millis();
                setState(STATE_BT_CONNECTING);
            }
            break;

        default:
            break;
    }

    // Render
    display.clear();
    switch (currentState) {
        case STATE_DEVICE_MENU:      renderDeviceMenu();      break;
        case STATE_BT_SCANNING:      renderBtScanning();      break;
        case STATE_BT_CONNECTING:    renderConnecting();      break;
        case STATE_CONNECT_FAIL:     renderConnectFail();     break;
        case STATE_MAIN_MENU:        renderMainMenu();        break;
        case STATE_BT_SUBMENU:       renderBtSubMenu();       break;
        case STATE_LED_SUBMENU:      renderLedSubMenu();      break;
        case STATE_FILE_BROWSER:     renderFileBrowser();     break;
        case STATE_PLAYER:           renderPlayer();          break;
        case STATE_SCANNING_LIBRARY: renderScanningLibrary(); break;
        case STATE_BT_SWITCHING:     renderSwitching();       break;
    }
    display.display();
}

// =========================================================================
// SD Card Init
// =========================================================================
void MusicPlayerApp::initSD() {
    if (sdAvailable) return;  // Already initialized from a previous session
    SPI.begin(PIN_SD_CLK, PIN_SD_MISO, PIN_SD_MOSI, PIN_SD_CS);
    sdAvailable = SD.begin(PIN_SD_CS);
    if (!sdAvailable) {
        MPLAYER_LOG("SD init failed");
        return;
    }
    // Ensure /media/ directory exists for organized file storage
    if (!SD.exists(MEDIA_DIR)) {
        SD.mkdir(MEDIA_DIR);
        MPLAYER_LOG("Created /media/ directory");
    }
}

// =========================================================================
// Connection Lifecycle
// =========================================================================

void MusicPlayerApp::startConnectingByAddress(const SavedDevice& dev) {
    MPLAYER_LOG("startConnectingByAddress: enter");
    if (WebPortalApp::bluetoothReleasedThisPowerCycle()) {
        showBluetoothNeedsRestart();
        return;
    }

    if (scannerActive) {
        btScanner.end();
        scannerActive = false;
    }

    // If switching from onboard speaker, tear down I2S pipeline first
    if (usingOnboardSpeaker) {
        MPLAYER_LOG("startConnectingByAddress: switching from onboard speaker");
        stopPlayback();
        // Clean up I2S pipeline objects so BT can rebuild fresh
        if (pSpectrumAnalyzer) { delete pSpectrumAnalyzer; pSpectrumAnalyzer = nullptr; }
        if (pPlayer) { delete pPlayer; pPlayer = nullptr; }
        audioPipelineReady = false;
        stopOnboardSpeaker();
    }

    connectingDeviceName = dev.name;
    btConnected = false;

    if (pA2dpStream) {
        // Reuse the existing stream. The library's internal task, timer, and
        // global pointer survive across disconnect/reconnect cycles.
        if (pA2dpStream->isConnected()) {
            // Check if we're already connected to the REQUESTED device
            if (memcmp(connectedAddress, dev.address, ESP_BD_ADDR_LEN) == 0) {
                // Same device — skip straight to menu
                MPLAYER_LOG("startConnectingByAddress: already connected, fast path");
                btConnected = true;
                connectedDeviceName = dev.name;
                createAudioPipeline();
                setState(STATE_MAIN_MENU);
                return;
            }
            // Connected to a DIFFERENT device — show "Switching..." screen
            // while disconnecting and settling. Direct disconnect+reconnect
            // fails (~2-3s connection drop) so we add a 1s settle phase.
            MPLAYER_LOG("startConnectingByAddress: different device, switching");
            stopPlayback();
            disconnectBT();
            switchTargetDevice = dev;
            switchStartTime = millis();
            switchWaitingForDisconnect = true;
            setState(STATE_BT_SWITCHING);
            return;
        }
        // Point the library at the new address and reconnect
        MPLAYER_LOG("startConnectingByAddress: reconnecting existing stream");
        // Restore blocking read timeout — disconnectBT() reduces it so the
        // BTC task doesn't block during disconnect. Restore portMAX_DELAY
        // for seamless audio playback (callback waits for data instead of
        // returning silence on empty buffer).
        auto& buf = static_cast<audio_tools::BufferRTOS<uint8_t>&>(pA2dpStream->buffer());
        buf.setReadMaxWait(portMAX_DELAY);
        pA2dpStream->source().set_auto_reconnect(const_cast<uint8_t*>(dev.address));
        pA2dpStream->source().reconnect();
    } else {
        // First connection ever — create the stream and full BT stack
        pA2dpStream = new A2DPStream();
        pA2dpStream->source().set_auto_reconnect(const_cast<uint8_t*>(dev.address));

        auto cfg = pA2dpStream->defaultConfig(TX_MODE);
        cfg.name = connectingDeviceName.c_str();
        cfg.auto_reconnect = true;
        cfg.wait_for_connection = false;
        // Prevent A2DPStream::write() from blocking forever when BT drops.
        // The write polling loop checks availableForWrite() every 5ms;
        // with timeout=200, it gives up after 200ms instead of looping forever.
        cfg.tx_write_timeout_ms = 200;
        pA2dpStream->begin(cfg);

        // Register AVRCP Target callback for media controls from BT speaker
        pA2dpStream->source().set_avrc_passthru_command_callback(
            [](uint8_t key, bool isReleased) {
                // Only act on key press, not release
                if (isReleased || !instance) return;
                instance->pendingAvrcCmd = key;
            });
    }

    createAudioPipeline();

    connectStartTime = millis();
    setState(STATE_BT_CONNECTING);
}

void MusicPlayerApp::showBluetoothNeedsRestart() {
    MPLAYER_LOG("Bluetooth unavailable until restart");
    connectFailNeedsRestart = true;
    setState(STATE_CONNECT_FAIL);
}

void MusicPlayerApp::startOnboardSpeaker() {
    MPLAYER_LOG("startOnboardSpeaker: enter");

    // If currently connected to BT, disconnect first
    if (pA2dpStream && pA2dpStream->isConnected()) {
        stopPlayback();
        disconnectBT();
    }

    usingOnboardSpeaker = true;
    btConnected = false;
    connectedDeviceName = "Onboard Speaker";

    // Release AudioManager's I2S port 0 so we can use it
    HAL::audioManager().releaseI2S();

    // Create I2S output stream for MAX98357A DAC
    if (!pI2sOut) {
        pI2sOut = new I2SStream();
        auto cfg = pI2sOut->defaultConfig(TX_MODE);
        cfg.port_no         = 0;
        cfg.i2s_format      = I2S_LSB_FORMAT;
        cfg.pin_ws          = 27;   // LRCLK
        cfg.pin_bck         = 26;   // BCLK
        cfg.pin_data        = 14;   // DOUT
        cfg.sample_rate     = 44100;
        cfg.channels        = 2;
        cfg.bits_per_sample = 16;
        cfg.buffer_count    = 8;
        cfg.buffer_size     = 512;
        pI2sOut->begin(cfg);

        // Volume control wrapper for I2S output
        pI2sVolume = new VolumeStream(*pI2sOut);
        auto vcfg = pI2sVolume->defaultConfig();
        vcfg.sample_rate     = 44100;
        vcfg.channels        = 2;
        vcfg.bits_per_sample = 16;
        pI2sVolume->begin(vcfg);
        pI2sVolume->setVolume(0.7f);
    }

    createAudioPipeline();
    setState(STATE_MAIN_MENU);
    MPLAYER_LOG("startOnboardSpeaker: ready");
}

void MusicPlayerApp::stopOnboardSpeaker() {
    if (!usingOnboardSpeaker) return;
    MPLAYER_LOG("stopOnboardSpeaker: enter");

    usingOnboardSpeaker = false;

    if (pI2sVolume) {
        pI2sVolume->end();
        delete pI2sVolume;
        pI2sVolume = nullptr;
    }
    if (pI2sOut) {
        pI2sOut->end();
        delete pI2sOut;
        pI2sOut = nullptr;
    }

    // Give AudioManager its I2S port back
    HAL::audioManager().reclaimI2S();
    MPLAYER_LOG("stopOnboardSpeaker: I2S returned to AudioManager");
}

void MusicPlayerApp::createAudioPipeline() {
    if (audioPipelineReady) return;
    // Need at least one output stream
    if (!pA2dpStream && !pI2sVolume) return;

    if (sdAvailable && !pSourceSD) {
        pSourceSD = new AudioSourceIdxSD(MEDIA_DIR, "mp3", PIN_SD_CS);
    }

    if (pSourceSD) {
        // Insert SpectrumAnalyzer between player and output (BT or I2S)
        if (!pSpectrumAnalyzer) {
            Print& outputStream = usingOnboardSpeaker
                ? static_cast<Print&>(*pI2sVolume)
                : static_cast<Print&>(*pA2dpStream);
            pSpectrumAnalyzer = new SpectrumAnalyzer(outputStream);
            pSpectrumAnalyzer->begin(44100, 2, 16);
        }
        pPlayer = new AudioPlayer(*pSourceSD, *pSpectrumAnalyzer, decoder);
        pPlayer->setVolume(1.0);
        pPlayer->setMetadataCallback(onMetadata);
        // Initialize the decoder pipeline and SD source without selecting a stream.
        // begin(-1) skips initial file selection; playTrack() uses setPath() later.
        pPlayer->begin(-1, false);
        audioPipelineReady = true;
    }
}

void MusicPlayerApp::disconnectBT() {
    if (!pA2dpStream) return;
    MPLAYER_LOG("disconnectBT: disabling auto-reconnect");
    pA2dpStream->source().set_auto_reconnect(false);

    // The A2DPStream data callback runs on the BTC task and calls
    // a2dp_buffer.readArray() with portMAX_DELAY (xStreamBufferReceive).
    // When the AudioPlayer is stopped and the buffer is empty, this blocks
    // the BTC task forever. Since esp_a2d_source_disconnect() dispatches
    // through the same BTC task, it hangs waiting for the blocked task.
    //
    // Fix: temporarily reduce the read timeout so the callback returns
    // quickly on an empty buffer instead of blocking forever. Then write
    // silence to wake the currently-pending read (started with the old
    // portMAX_DELAY). After a brief delay the BTC task is free.
    //
    // We do NOT call pA2dpStream->clear() because that sets is_a2dp_active
    // to false. With is_a2dp_active=false, the callback stops draining
    // the buffer, so writes fill it to 100% and block on reconnect.
    // The read timeout is restored in startConnectingByAddress().
    MPLAYER_LOG("disconnectBT: unblocking A2DP data callback");
    auto& buf = static_cast<audio_tools::BufferRTOS<uint8_t>&>(pA2dpStream->buffer());
    buf.setReadMaxWait(pdMS_TO_TICKS(50));
    uint8_t silence[512] = {0};
    buf.writeArray(silence, sizeof(silence));
    delay(100);  // let BTC task process with the new short timeout

    esp_bd_addr_t empty = {0};
    if (memcmp(connectedAddress, empty, ESP_BD_ADDR_LEN) != 0) {
        MPLAYER_LOG("disconnectBT: calling esp_a2d_source_disconnect");
        esp_a2d_source_disconnect(connectedAddress);
        MPLAYER_LOG("disconnectBT: disconnect sent");
    }
    btConnected = false;
}

void MusicPlayerApp::destroyAudioPipeline() {
    MPLAYER_LOG("destroyAudioPipeline: enter");
    isPlaying = false;
    // NOTE: Don't call pPlayer->stop() here — stopPlayback() handles it.

    if (usingOnboardSpeaker) {
        // Clean up I2S output — safe to fully tear down unlike BT
        stopOnboardSpeaker();
        // SpectrumAnalyzer's output pointer is now invalid — must recreate next time
        if (pSpectrumAnalyzer) {
            delete pSpectrumAnalyzer;
            pSpectrumAnalyzer = nullptr;
        }
        if (pPlayer) {
            delete pPlayer;
            pPlayer = nullptr;
        }
        audioPipelineReady = false;
    } else {
#if MUSIC_PLAYER_DISCONNECT_ON_EXIT
        disconnectBT();
#else
        // Keep BT connected on exit — avoids BT stack degradation from
        // repeated disconnect/reconnect cycles. On re-entry, isConnected()
        // fast path skips straight to main menu.
        MPLAYER_LOG("destroyAudioPipeline: keeping BT connected (DISCONNECT_ON_EXIT=0)");
        btConnected = false;
#endif
    }
    MPLAYER_LOG("destroyAudioPipeline: done");
}

// =========================================================================
// NVS Device Storage
// =========================================================================
void MusicPlayerApp::loadSavedDevices() {
    savedDevices.clear();
    Preferences prefs;
    prefs.begin("btdevs", true);

    int count = prefs.getInt("count", 0);
    for (int i = 0; i < count && i < 8; i++) {
        SavedDevice dev;
        dev.name = prefs.getString(("d" + String(i) + "n").c_str(), "");
        prefs.getBytes(("d" + String(i) + "a").c_str(), dev.address, ESP_BD_ADDR_LEN);
        if (dev.name.length() > 0) {
            savedDevices.push_back(dev);
        }
    }

    prefs.end();
}

void MusicPlayerApp::saveDevice(const String& name, esp_bd_addr_t address) {
    // Check if already saved (by address)
    for (int i = 0; i < (int)savedDevices.size(); i++) {
        if (memcmp(savedDevices[i].address, address, ESP_BD_ADDR_LEN) == 0) {
            // Move to front
            SavedDevice dev = savedDevices[i];
            savedDevices.erase(savedDevices.begin() + i);
            savedDevices.insert(savedDevices.begin(), dev);
            writeSavedDevicesToNVS();
            return;
        }
    }

    // Add new device at front
    SavedDevice dev;
    dev.name = name;
    memcpy(dev.address, address, ESP_BD_ADDR_LEN);
    savedDevices.insert(savedDevices.begin(), dev);

    // Trim to max 8
    while (savedDevices.size() > 8) savedDevices.pop_back();

    writeSavedDevicesToNVS();
}

void MusicPlayerApp::forgetAllDevices() {
    savedDevices.clear();
    Preferences prefs;
    prefs.begin("btdevs", false);
    prefs.clear();
    prefs.end();
}

void MusicPlayerApp::writeSavedDevicesToNVS() {
    Preferences prefs;
    prefs.begin("btdevs", false);
    prefs.putInt("count", savedDevices.size());
    for (int i = 0; i < (int)savedDevices.size(); i++) {
        prefs.putString(("d" + String(i) + "n").c_str(), savedDevices[i].name);
        prefs.putBytes(("d" + String(i) + "a").c_str(), savedDevices[i].address, ESP_BD_ADDR_LEN);
    }
    prefs.end();
}

// =========================================================================
// Library Scanning
// =========================================================================
void MusicPlayerApp::startLibraryScan() {
    if (!sdAvailable) {
        setState(STATE_FILE_BROWSER);
        return;
    }

    // Count MP3 files to check cache
    File root = SD.open("/");
    int fileCount = 0;
    if (root) {
        File entry;
        while ((entry = root.openNextFile())) {
            if (!entry.isDirectory()) {
                String name = entry.name();
                String lower = name;
                lower.toLowerCase();
                if (lower.endsWith(".mp3")) fileCount++;
            }
            entry.close();
        }
        root.close();
    }

    // Try loading cache
    if (fileCount > 0 && ID3Scanner::isCacheValid(CACHE_PATH, fileCount)) {
        if (ID3Scanner::loadCache(CACHE_PATH, trackLibrary)) {
            libraryScanned = true;
            setState(STATE_FILE_BROWSER);
            return;
        }
    }

    // Need full scan
    if (fileCount == 0) {
        trackLibrary.clear();
        libraryScanned = true;
        setState(STATE_FILE_BROWSER);
        return;
    }

    libraryScanTotal = fileCount;
    libraryScanCurrent = 0;
    setState(STATE_SCANNING_LIBRARY);

    // Do the scan (blocking for now — shows progress via update/render cycle)
    ID3Scanner::scanAllFiles(MEDIA_DIR, "mp3", trackLibrary,
        [](int current, int total) {
            if (MusicPlayerApp::instance) {
                MusicPlayerApp::instance->libraryScanCurrent = current;
                MusicPlayerApp::instance->libraryScanTotal = total;
                // Force a display update during scan
                display.clear();
                MusicPlayerApp::instance->renderScanningLibrary();
                display.display();
            }
        });

    // Save cache
    ID3Scanner::saveCache(CACHE_PATH, trackLibrary);
    libraryScanned = true;
    setState(STATE_FILE_BROWSER);
}

// =========================================================================
// Playback
// =========================================================================
void MusicPlayerApp::playTrack(int index) {
    if (!pPlayer || index < 0 || index >= (int)trackLibrary.size()) return;

    currentTrackIndex = index;
    nowPlayingTitle = "";
    nowPlayingArtist = "";
    marqueeOffset = 0;

    // Open file directly by path (bypasses SDIndex which may not be populated)
    const char* path = trackLibrary[index].path.c_str();
    MPLAYER_LOGF("playTrack: %d -> %s", index, path);

    // Read bitrate for time estimation and get file size
    currentTrackBitrate = ID3Scanner::readMP3Bitrate(path);
    File tmpF = SD.open(path, FILE_READ);
    currentTrackFileSize = tmpF ? tmpF.size() : 0;
    if (tmpF) tmpF.close();
    MPLAYER_LOGF("playTrack: bitrate=%d, size=%u", currentTrackBitrate, (unsigned)currentTrackFileSize);

    pPlayer->setPath(path);
    pPlayer->setActive(true);
    isPlaying = true;
    clearPlaybackState();  // New track starts fresh
    setState(STATE_PLAYER);
}

void MusicPlayerApp::stopPlayback() {
    MPLAYER_LOG("stopPlayback: enter");
    isPlaying = false;
    if (pPlayer) {
        if (!usingOnboardSpeaker) {
            // Disable auto-fade before stopping. Auto-fade writes silence (2KB+)
            // to the A2DPStream output, which hangs after multiple BT disconnect/
            // reconnect cycles due to accumulated BT stack state corruption.
            // Safe to leave auto-fade on for I2S output.
            pPlayer->setAutoFade(false);
        }
        pPlayer->stop();
        if (!usingOnboardSpeaker) {
            pPlayer->setAutoFade(true);
        }
    }
    MPLAYER_LOG("stopPlayback: done");
}

void MusicPlayerApp::nextTrack() {
    if (trackLibrary.empty()) return;
    int next;
    if (shuffleEnabled) {
        next = random(0, trackLibrary.size());
        if (next == currentTrackIndex && trackLibrary.size() > 1)
            next = (next + 1) % trackLibrary.size();
    } else {
        next = (currentTrackIndex + 1) % trackLibrary.size();
    }
    playTrack(next);
}

void MusicPlayerApp::prevTrack() {
    if (trackLibrary.empty()) return;
    int prev = currentTrackIndex - 1;
    if (prev < 0) prev = trackLibrary.size() - 1;
    playTrack(prev);
}

void MusicPlayerApp::togglePlayPause() {
    if (!pPlayer) return;

    // If no track selected, auto-pick a random one from the library
    if (currentTrackIndex < 0) {
        if (!trackLibrary.empty()) {
            playTrack(random(0, trackLibrary.size()));
        }
        return;
    }

    isPlaying = !isPlaying;
    pPlayer->setActive(isPlaying);

    // Save position on pause so we can resume later
    if (!isPlaying) {
        savePlaybackState();
    }
}

void MusicPlayerApp::updateVolumeFromSlider() {
    // Invert slider direction so "up" = louder
    float vol = (100.0f - sliderPosition_Percentage_Filtered) / 100.0f;
    static float lastVol = -1.0;
    if (abs(vol - lastVol) > 0.03) {
        if (usingOnboardSpeaker && pI2sVolume) {
            pI2sVolume->setVolume(vol);
        } else if (pA2dpStream && pA2dpStream->isConnected()) {
            pA2dpStream->setVolume(vol);
        } else {
            return;  // No output available
        }
        lastVol = vol;
        lastVolumeChangeTime = millis();  // Trigger volume overlay in UI
    }
}

// =========================================================================
// Resume Playback State (NVS)
// =========================================================================
void MusicPlayerApp::savePlaybackState() {
    if (currentTrackIndex < 0 || currentTrackIndex >= (int)trackLibrary.size()) return;
    if (!pPlayer) return;

    Preferences prefs;
    prefs.begin("mpstate", false);
    prefs.putString("path", trackLibrary[currentTrackIndex].path);

    // Get current byte position from the underlying file stream
    size_t pos = 0;
    Stream* s = pPlayer->getStream();
    if (s) pos = ((File*)s)->position();
    prefs.putUInt("pos", (uint32_t)pos);
    prefs.end();

    MPLAYER_LOGF("savePlaybackState: %s @ %u bytes",
        trackLibrary[currentTrackIndex].path.c_str(), (unsigned)pos);
}

void MusicPlayerApp::loadPlaybackState() {
    Preferences prefs;
    prefs.begin("mpstate", true);
    resumeTrackPath = prefs.getString("path", "");
    resumeBytePosition = prefs.getUInt("pos", 0);
    hasResumeState = resumeTrackPath.length() > 0;
    prefs.end();

    if (hasResumeState) {
        MPLAYER_LOGF("loadPlaybackState: %s @ %u bytes",
            resumeTrackPath.c_str(), (unsigned)resumeBytePosition);
    }
}

void MusicPlayerApp::clearPlaybackState() {
    if (!hasResumeState) return;
    hasResumeState = false;
    resumeTrackPath = "";
    resumeBytePosition = 0;

    Preferences prefs;
    prefs.begin("mpstate", false);
    prefs.clear();
    prefs.end();
}

// =========================================================================
// Metadata Callback (static)
// =========================================================================
void MusicPlayerApp::onMetadata(MetaDataType type, const char* str, int len) {
    if (!instance) return;
    String val = String(str).substring(0, len);
    val.trim();
    if (type == Title && val.length()) {
        instance->nowPlayingTitle = val;
        instance->marqueeOffset = 0;
    }
    if (type == Artist && val.length()) {
        instance->nowPlayingArtist = val;
    }
}

// =========================================================================
// State Management
// =========================================================================
void MusicPlayerApp::setState(MusicAppState newState) {
#if MUSIC_PLAYER_DEBUG
    static const char* stateNames[] = {
        "DEVICE_MENU", "BT_SCANNING", "BT_CONNECTING", "CONNECT_FAIL",
        "MAIN_MENU", "BT_SUBMENU", "LED_SUBMENU", "FILE_BROWSER", "PLAYER",
        "SCANNING_LIBRARY", "BT_SWITCHING"
    };
    MPLAYER_LOGF("setState: %s -> %s",
        stateNames[currentState], stateNames[newState]);
#endif
    currentState = newState;
    resetCursor();
}

void MusicPlayerApp::resetCursor() {
    menuCursorIndex = 0;
    menuScrollOffset = 0;
}

int MusicPlayerApp::getListSize() const {
    switch (currentState) {
        case STATE_DEVICE_MENU:
            return savedDevices.size() + 2; // +1 for "Onboard Speaker", +1 for "Scan for new..."
        case STATE_BT_SCANNING:
            return btScanner.getResultCount();
        case STATE_MAIN_MENU:
            return MAIN_MENU_COUNT;
        case STATE_BT_SUBMENU:
            return getBtSubMenuCount();
        case STATE_LED_SUBMENU:
            return getLedSubMenuCount();
        case STATE_FILE_BROWSER:
            return trackLibrary.size();
        default:
            return 0;
    }
}

void MusicPlayerApp::exitApp() {
    // Don't call end() here — AppManager::switchToApp() calls it via endFunc().
    // Double end() was causing pPlayer->stop() to hang after multiple cycles.
    MenuManager::instance().returnToMenu();
}

// =========================================================================
// Main Menu Items
// =========================================================================
String MusicPlayerApp::getMainMenuItem(int index) const {
    switch (index) {
        case 0: return "Now Playing";
        case 1: return "Browse Songs";
        case 2: return shuffleEnabled ? "Shuffle: On" : "Shuffle: Off";
        case 3: return usingOnboardSpeaker ? "Output" : "Bluetooth";
        case 4: return "LEDs";
        case 5: return "Exit";
        default: return "";
    }
}

int MusicPlayerApp::getBtSubMenuCount() const {
    if (usingOnboardSpeaker) return 2; // Status, Switch Output
    return 3; // Status, Disconnect, Forget All
}

String MusicPlayerApp::getBtSubMenuItem(int index) const {
    if (usingOnboardSpeaker) {
        switch (index) {
            case 0: return "Output: Onboard Speaker";
            case 1: return "Switch Output...";
            default: return "";
        }
    }
    switch (index) {
        case 0: return btConnected ? ("Connected: " + connectedDeviceName) : "Not Connected";
        case 1: return "Disconnect";
        case 2: return "Forget All Devices";
        default: return "";
    }
}

int MusicPlayerApp::getLedSubMenuCount() const {
    return 6; // Mode, Back, Front Top, Front Mid, Front Bottom, Sync Delay
}

String MusicPlayerApp::getLedSubMenuItem(int index) const {
    static const char* modes[] = {"Off", "Reactive", "Pulse"};
    switch (index) {
        case 0: return String("Mode: ") + modes[ledEffectMode];
        case 1: return (ledEnableMask & 0x01) ? "Back LED: On" : "Back LED: Off";
        case 2: return (ledEnableMask & 0x02) ? "Front Top: On" : "Front Top: Off";
        case 3: return (ledEnableMask & 0x04) ? "Front Mid: On" : "Front Mid: Off";
        case 4: return (ledEnableMask & 0x08) ? "Front Bot: On" : "Front Bot: Off";
        case 5: {
            int delayMs = pSpectrumAnalyzer ? pSpectrumAnalyzer->getDelayMs() : 150;
            return "Sync: " + String(delayMs) + "ms";
        }
        default: return "";
    }
}

// =========================================================================
// Button Callbacks (static)
// =========================================================================
void MusicPlayerApp::onButtonUp(const ButtonEvent& event) {
    if (event.eventType == ButtonEvent_Pressed) instance->handleUp();
}
void MusicPlayerApp::onButtonDown(const ButtonEvent& event) {
    if (event.eventType == ButtonEvent_Pressed) instance->handleDown();
}
void MusicPlayerApp::onButtonLeft(const ButtonEvent& event) {
    // In player: defer to Released to distinguish tap vs hold-to-scrub
    if (instance->currentState == STATE_PLAYER) {
        if (event.eventType == ButtonEvent_Pressed) {
            instance->scrubButtonDir = -1;
            instance->scrubPressTime = millis();
        } else if (event.eventType == ButtonEvent_Released) {
            if (instance->scrubActive) {
                instance->scrubActive = false;
            } else {
                instance->prevTrack();
            }
            instance->scrubButtonDir = 0;
        }
    } else {
        if (event.eventType == ButtonEvent_Pressed) instance->handleLeft();
    }
}
void MusicPlayerApp::onButtonRight(const ButtonEvent& event) {
    if (instance->currentState == STATE_PLAYER) {
        if (event.eventType == ButtonEvent_Pressed) {
            instance->scrubButtonDir = 1;
            instance->scrubPressTime = millis();
        } else if (event.eventType == ButtonEvent_Released) {
            if (instance->scrubActive) {
                instance->scrubActive = false;
            } else {
                instance->nextTrack();
            }
            instance->scrubButtonDir = 0;
        }
    } else {
        if (event.eventType == ButtonEvent_Pressed) instance->handleRight();
    }
}
void MusicPlayerApp::onButtonEnter(const ButtonEvent& event) {
    if (event.eventType == ButtonEvent_Released) instance->handleEnter();
}
void MusicPlayerApp::onButtonBack(const ButtonEvent& event) {
    if (event.eventType == ButtonEvent_Released) instance->handleBack();
}

// =========================================================================
// Navigation Logic
// =========================================================================
void MusicPlayerApp::handleUp() {
    if (currentState == STATE_PLAYER) {
        visualizerMode = (VisualizerMode)((visualizerMode + 1) % 3);
        return;
    }
    if (currentState == STATE_BT_CONNECTING ||
        currentState == STATE_BT_SWITCHING || currentState == STATE_SCANNING_LIBRARY) return;

    if (menuCursorIndex > 0) {
        menuCursorIndex--;
        if (menuCursorIndex < menuScrollOffset)
            menuScrollOffset = menuCursorIndex;
    }
}

void MusicPlayerApp::handleDown() {
    if (currentState == STATE_PLAYER) {
        // Cycle backwards: Off → Spectrum → Amplitude → Off
        visualizerMode = (VisualizerMode)((visualizerMode + 2) % 3);
        return;
    }
    if (currentState == STATE_BT_CONNECTING ||
        currentState == STATE_BT_SWITCHING || currentState == STATE_SCANNING_LIBRARY) return;

    int listSize = getListSize();
    if (menuCursorIndex < listSize - 1) {
        menuCursorIndex++;
        if (menuCursorIndex >= menuScrollOffset + MAX_VISIBLE_ITEMS)
            menuScrollOffset = menuCursorIndex - MAX_VISIBLE_ITEMS + 1;
    }
}

void MusicPlayerApp::handleLeft() {
    if (currentState == STATE_PLAYER) {
        prevTrack();
    }
}

void MusicPlayerApp::handleRight() {
    if (currentState == STATE_PLAYER) {
        nextTrack();
    }
}

void MusicPlayerApp::handleEnter() {
    switch (currentState) {
        case STATE_DEVICE_MENU: {
            if (menuCursorIndex == 0) {
                // "Onboard Speaker" selected
                startOnboardSpeaker();
            } else if (menuCursorIndex <= (int)savedDevices.size()) {
                // Saved BT device selected (index shifted by 1 for Onboard Speaker)
                int devIdx = menuCursorIndex - 1;
                SavedDevice dev = savedDevices[devIdx];
                saveDevice(dev.name, dev.address); // Move to front
                startConnectingByAddress(dev);
            } else {
                // "Scan for new..." selected
                if (!scannerActive) {
                    if (WebPortalApp::bluetoothReleasedThisPowerCycle()) {
                        showBluetoothNeedsRestart();
                    } else if (btScanner.begin()) {
                        scannerActive = true;
                        btScanner.startScan(12);
                        setState(STATE_BT_SCANNING);
                    } else {
                        showBluetoothNeedsRestart();
                    }
                }
            }
            break;
        }

        case STATE_BT_SCANNING: {
            int count = btScanner.getResultCount();
            if (menuCursorIndex < count) {
                BTScanResult result = btScanner.getResult(menuCursorIndex);
                saveDevice(String(result.name), result.address);
                // Build a SavedDevice and use address-based connection
                SavedDevice dev;
                dev.name = String(result.name);
                memcpy(dev.address, result.address, ESP_BD_ADDR_LEN);
                startConnectingByAddress(dev);
            }
            break;
        }

        case STATE_CONNECT_FAIL:
            setState(STATE_DEVICE_MENU);
            break;

        case STATE_MAIN_MENU:
            switch (menuCursorIndex) {
                case 0: // Now Playing — resume from saved position if available
                    if (currentTrackIndex < 0 && hasResumeState && libraryScanned) {
                        // Find the saved track in the library
                        for (int i = 0; i < (int)trackLibrary.size(); i++) {
                            if (trackLibrary[i].path == resumeTrackPath) {
                                MPLAYER_LOGF("resume: found track %d, seeking to %u",
                                    i, (unsigned)resumeBytePosition);
                                playTrack(i);
                                // Seek to saved position after playTrack opens the file
                                if (pPlayer && pPlayer->getStream() && resumeBytePosition > 0) {
                                    ((File*)pPlayer->getStream())->seek(resumeBytePosition);
                                }
                                hasResumeState = false;
                                break;
                            }
                        }
                        if (currentTrackIndex < 0) {
                            // Track not found in library, just go to player screen
                            hasResumeState = false;
                        }
                    }
                    setState(STATE_PLAYER);
                    break;
                case 1: // Browse Songs
                    if (!libraryScanned) {
                        startLibraryScan();
                    } else {
                        setState(STATE_FILE_BROWSER);
                    }
                    break;
                case 2: // Shuffle toggle
                    shuffleEnabled = !shuffleEnabled;
                    break;
                case 3: // Bluetooth
                    setState(STATE_BT_SUBMENU);
                    break;
                case 4: // LEDs submenu
                    setState(STATE_LED_SUBMENU);
                    break;
                case 5: // Exit
                    exitApp();
                    break;
            }
            break;

        case STATE_BT_SUBMENU:
            if (usingOnboardSpeaker) {
                switch (menuCursorIndex) {
                    case 0: break; // Status (info only)
                    case 1: // Switch Output — tear down I2S and go to device menu
                        stopPlayback();
                        destroyAudioPipeline();
                        setState(STATE_DEVICE_MENU);
                        break;
                }
            } else {
                switch (menuCursorIndex) {
                    case 0: // Status (info only)
                        break;
                    case 1: { // Disconnect — always disconnects (explicit user action)
                        stopPlayback();
                        disconnectBT();
                        setState(STATE_DEVICE_MENU);
                        break;
                    }
                    case 2: { // Forget All
                        forgetAllDevices();
                        stopPlayback();
                        disconnectBT();
                        setState(STATE_DEVICE_MENU);
                        break;
                    }
                }
            }
            break;

        case STATE_LED_SUBMENU:
            switch (menuCursorIndex) {
                case 0: // Cycle LED mode
                    ledEffectMode = (LEDEffectMode)((ledEffectMode + 1) % 3);
                    if (ledEffectMode == LED_OFF) setColorsOff();
                    break;
                case 1: // Back LED toggle
                    ledEnableMask ^= 0x01;
                    if (!(ledEnableMask & 0x01)) HAL::setRgbLed(pixel_Back, 0, 0, 0, 0);
                    break;
                case 2: // Front Top toggle
                    ledEnableMask ^= 0x02;
                    if (!(ledEnableMask & 0x02)) HAL::setRgbLed(pixel_Front_Top, 0, 0, 0, 0);
                    break;
                case 3: // Front Mid toggle
                    ledEnableMask ^= 0x04;
                    if (!(ledEnableMask & 0x04)) HAL::setRgbLed(pixel_Front_Middle, 0, 0, 0, 0);
                    break;
                case 4: // Front Bottom toggle
                    ledEnableMask ^= 0x08;
                    if (!(ledEnableMask & 0x08)) HAL::setRgbLed(pixel_Front_Bottom, 0, 0, 0, 0);
                    break;
                case 5: // Sync Delay — cycle 0, 25, 50, ..., 300, back to 0
                    if (pSpectrumAnalyzer) {
                        int d = pSpectrumAnalyzer->getDelayMs() + 25;
                        if (d > 300) d = 0;
                        pSpectrumAnalyzer->setDelayMs(d);
                    }
                    break;
            }
            break;

        case STATE_FILE_BROWSER:
            if (!trackLibrary.empty()) {
                playTrack(menuCursorIndex);
            }
            break;

        case STATE_PLAYER:
            togglePlayPause();
            break;

        default:
            break;
    }
}

void MusicPlayerApp::handleBack() {
    switch (currentState) {
        case STATE_DEVICE_MENU:
            exitApp();
            break;

        case STATE_BT_SCANNING:
            if (scannerActive) {
                btScanner.end();
                scannerActive = false;
            }
            setState(STATE_DEVICE_MENU);
            break;

        case STATE_CONNECT_FAIL:
            setState(STATE_DEVICE_MENU);
            break;

        case STATE_MAIN_MENU:
            exitApp();
            break;

        case STATE_BT_SUBMENU:
        case STATE_LED_SUBMENU:
            setState(STATE_MAIN_MENU);
            break;

        case STATE_FILE_BROWSER:
            setState(STATE_MAIN_MENU);
            break;

        case STATE_PLAYER:
            // Back to main menu, playback continues
            setState(STATE_MAIN_MENU);
            break;

        default:
            break;
    }
}

// =========================================================================
// LED Effects
// =========================================================================
void MusicPlayerApp::updateLEDs() {
    if (ledEffectMode == LED_OFF) return;
    if (millis() - lastLEDUpdate < 33) return;  // ~30fps, matches RGBController throttle
    lastLEDUpdate = millis();

    uint8_t r = 0, g = 0, b = 0, w = 0;

    if (ledEffectMode == LED_REACTIVE && isPlaying) {
        // EMA smoothing (α=0.25) + gamma (0.6) — follows Booper pattern
        float raw = currentAmplitude;
        if (raw > 1.0f) raw = 1.0f;
        float curved = powf(raw, 0.6f);
        ledSmoothedAmplitude = 0.25f * curved + 0.75f * ledSmoothedAmplitude;
        float amp = ledSmoothedAmplitude;

        if (amp < 0.15f) {
            // Quiet: dim blue pulse (breathing)
            float pulse = (sinf(millis() / 1000.0f * 3.14159f) + 1.0f) * 0.5f;
            b = (uint8_t)(3.0f + pulse * 5.0f);
        } else if (amp < 0.5f) {
            // Medium: cyan → green gradient
            float t = (amp - 0.15f) / 0.35f;
            g = (uint8_t)(5.0f + t * 15.0f);
            b = (uint8_t)(5.0f + (1.0f - t) * 10.0f);
        } else {
            // Loud: red + white flash
            float t = (amp - 0.5f) / 0.5f;
            if (t > 1.0f) t = 1.0f;
            r = (uint8_t)(15.0f + t * 10.0f);
            w = (uint8_t)(t * 30.0f);
        }

    } else if (ledEffectMode == LED_PULSE) {
        // Gentle breathing regardless of audio
        float pulse = (sinf(millis() / 2000.0f * 3.14159f) + 1.0f) * 0.5f;
        b = (uint8_t)(2.0f + pulse * 6.0f);

    } else if (ledEffectMode == LED_REACTIVE && !isPlaying) {
        // Reactive but not playing: dim idle pulse
        float pulse = (sinf(millis() / 3000.0f * 3.14159f) + 1.0f) * 0.5f;
        b = (uint8_t)(1.0f + pulse * 3.0f);
    }

    // Apply per-LED mask: enabled LEDs get color, disabled ones get zeroed
    // Front pixels get full color, back pixel gets dimmed (÷3)
    HAL::setRgbLed(pixel_Front_Top,    (ledEnableMask & 0x02) ? r     : 0, (ledEnableMask & 0x02) ? g     : 0, (ledEnableMask & 0x02) ? b     : 0, (ledEnableMask & 0x02) ? w     : 0);
    HAL::setRgbLed(pixel_Front_Middle, (ledEnableMask & 0x04) ? r     : 0, (ledEnableMask & 0x04) ? g     : 0, (ledEnableMask & 0x04) ? b     : 0, (ledEnableMask & 0x04) ? w     : 0);
    HAL::setRgbLed(pixel_Front_Bottom, (ledEnableMask & 0x08) ? r     : 0, (ledEnableMask & 0x08) ? g     : 0, (ledEnableMask & 0x08) ? b     : 0, (ledEnableMask & 0x08) ? w     : 0);
    HAL::setRgbLed(pixel_Back,         (ledEnableMask & 0x01) ? r / 3 : 0, (ledEnableMask & 0x01) ? g / 3 : 0, (ledEnableMask & 0x01) ? b / 3 : 0, (ledEnableMask & 0x01) ? w / 3 : 0);
}

// =========================================================================
// Settings Persistence (NVS)
// =========================================================================
void MusicPlayerApp::saveSettings() {
    Preferences prefs;
    prefs.begin("mpsettings", false);
    prefs.putBool("shuffle", shuffleEnabled);
    prefs.putUChar("ledmode", (uint8_t)ledEffectMode);
    prefs.putUChar("vizmode", (uint8_t)visualizerMode);
    prefs.putUChar("ledmask", ledEnableMask);
    prefs.putUShort("leddelay", pSpectrumAnalyzer ? pSpectrumAnalyzer->getDelayMs() : 150);
    prefs.end();
    MPLAYER_LOGF("saveSettings: shuffle=%d led=%d vizmode=%d ledmask=0x%02X delay=%d",
        shuffleEnabled, ledEffectMode, visualizerMode, ledEnableMask,
        pSpectrumAnalyzer ? pSpectrumAnalyzer->getDelayMs() : 150);
}

void MusicPlayerApp::loadSettings() {
    Preferences prefs;
    prefs.begin("mpsettings", true);  // read-only
    shuffleEnabled = prefs.getBool("shuffle", false);
    ledEffectMode = (LEDEffectMode)prefs.getUChar("ledmode", LED_OFF);
    visualizerMode = (VisualizerMode)prefs.getUChar("vizmode", VIZ_OFF);
    ledEnableMask = prefs.getUChar("ledmask", 0x0F);
    int ledDelay = prefs.getUShort("leddelay", 150);
    prefs.end();
    if (pSpectrumAnalyzer) pSpectrumAnalyzer->setDelayMs(ledDelay);
    MPLAYER_LOGF("loadSettings: shuffle=%d led=%d vizmode=%d ledmask=0x%02X delay=%d",
        shuffleEnabled, ledEffectMode, visualizerMode, ledEnableMask, ledDelay);
}

// =========================================================================
// UI Rendering
// =========================================================================
void MusicPlayerApp::drawHeader(const String& title) {
    display.setColor(WHITE);
    display.fillRect(0, 0, 128, 14);
    display.setColor(BLACK);
    display.setFont(ArialMT_Plain_10);
    display.setTextAlignment(TEXT_ALIGN_CENTER);
    display.drawString(64, 1, title);
    display.setColor(WHITE);
}

void MusicPlayerApp::drawList(const std::vector<String>& items, int cursor, int scrollOffset) {
    display.setTextAlignment(TEXT_ALIGN_LEFT);
    display.setFont(ArialMT_Plain_10);

    for (int i = 0; i < MAX_VISIBLE_ITEMS; i++) {
        int idx = i + scrollOffset;
        if (idx >= (int)items.size()) break;

        int y = LIST_Y_START + (i * ITEM_HEIGHT);
        if (idx == cursor) {
            display.fillRect(0, y, 128, ITEM_HEIGHT);
            display.setColor(BLACK);
            display.drawString(4, y - 1, items[idx]);
            display.setColor(WHITE);
        } else {
            display.drawString(4, y - 1, items[idx]);
        }
    }
}

void MusicPlayerApp::drawScrollBar(int totalItems, int visibleItems, int scrollOffset) {
    if (totalItems <= visibleItems) return;

    int barHeight = 48; // Available height for scrollbar
    int thumbHeight = max(4, (barHeight * visibleItems) / totalItems);
    int thumbY = LIST_Y_START + (barHeight * scrollOffset) / totalItems;

    display.drawRect(126, LIST_Y_START, 2, barHeight);
    display.fillRect(126, thumbY, 2, thumbHeight);
}

void MusicPlayerApp::renderDeviceMenu() {
    drawHeader("Audio Output");

    std::vector<String> items;
    items.push_back("Onboard Speaker");
    for (const auto& dev : savedDevices) {
        items.push_back(dev.name);
    }
    items.push_back("Scan for new...");

    drawList(items, menuCursorIndex, menuScrollOffset);
    drawScrollBar(items.size(), MAX_VISIBLE_ITEMS, menuScrollOffset);
}

void MusicPlayerApp::renderBtScanning() {
    drawHeader("Scanning");

    int count = btScanner.getResultCount();

    if (count == 0) {
        // Animated "Searching..."
        int dots = (millis() / 500) % 4;
        String msg = "Searching";
        for (int i = 0; i < dots; i++) msg += ".";

        display.setFont(ArialMT_Plain_10);
        display.setTextAlignment(TEXT_ALIGN_CENTER);
        display.drawString(64, 30, msg);
    } else {
        std::vector<String> items;
        for (int i = 0; i < count; i++) {
            BTScanResult r = btScanner.getResult(i);
            // Show signal strength indicator
            String entry = r.name;
            int bars = 0;
            if (r.rssi > -60) bars = 3;
            else if (r.rssi > -75) bars = 2;
            else bars = 1;
            String sig = " (";
            for (int b = 0; b < bars; b++) sig += "*";
            sig += ")";
            entry += sig;
            items.push_back(entry);
        }
        drawList(items, menuCursorIndex, menuScrollOffset);
    }

    // Show scanning indicator at bottom
    if (btScanner.isScanning()) {
        display.setFont(ArialMT_Plain_10);
        display.setTextAlignment(TEXT_ALIGN_RIGHT);
        display.drawString(124, 54, "scanning...");
    }
}

void MusicPlayerApp::renderConnecting() {
    drawHeader("Bluetooth");

    int dots = (millis() / 400) % 4;
    String msg = "Connecting";
    for (int i = 0; i < dots; i++) msg += ".";

    display.setFont(ArialMT_Plain_10);
    display.setTextAlignment(TEXT_ALIGN_CENTER);
    display.drawString(64, 24, msg);
    display.drawString(64, 38, connectingDeviceName);
}

void MusicPlayerApp::renderConnectFail() {
    drawHeader("Bluetooth");

    display.setFont(ArialMT_Plain_10);
    display.setTextAlignment(TEXT_ALIGN_CENTER);
    if (connectFailNeedsRestart) {
        display.drawString(64, 20, "Bluetooth needs");
        display.drawString(64, 33, "a restart");
        display.drawString(64, 48, "Press any button");
    } else {
        display.drawString(64, 24, "Connection Failed");
        display.drawString(64, 40, "Press any button");
    }
}

void MusicPlayerApp::renderMainMenu() {
    drawHeader("Music Player");

    std::vector<String> items;
    for (int i = 0; i < MAIN_MENU_COUNT; i++) {
        items.push_back(getMainMenuItem(i));
    }
    drawList(items, menuCursorIndex, menuScrollOffset);

    // Connection status in bottom-right
    if (btConnected) {
        display.setFont(ArialMT_Plain_10);
        display.setTextAlignment(TEXT_ALIGN_RIGHT);
        display.drawString(124, 54, "BT");
    }
}

void MusicPlayerApp::renderBtSubMenu() {
    drawHeader(usingOnboardSpeaker ? "Output" : "Bluetooth");

    std::vector<String> items;
    int count = getBtSubMenuCount();
    for (int i = 0; i < count; i++) {
        items.push_back(getBtSubMenuItem(i));
    }
    drawList(items, menuCursorIndex, menuScrollOffset);
}

void MusicPlayerApp::renderLedSubMenu() {
    drawHeader("LEDs");

    std::vector<String> items;
    int count = getLedSubMenuCount();
    for (int i = 0; i < count; i++) {
        items.push_back(getLedSubMenuItem(i));
    }
    drawList(items, menuCursorIndex, menuScrollOffset);
}

void MusicPlayerApp::renderFileBrowser() {
    drawHeader("Library");

    if (trackLibrary.empty()) {
        display.setFont(ArialMT_Plain_10);
        display.setTextAlignment(TEXT_ALIGN_CENTER);
        display.drawString(64, 30, sdAvailable ? "No MP3 files found" : "No SD card");
        return;
    }

    std::vector<String> items;
    for (const auto& t : trackLibrary) {
        items.push_back(t.display);
    }
    drawList(items, menuCursorIndex, menuScrollOffset);
    drawScrollBar(items.size(), MAX_VISIBLE_ITEMS, menuScrollOffset);
}

void MusicPlayerApp::renderPlayer() {
    drawHeader("Now Playing");

    // Output icon top-left (drawn over white header in black) — shifted right 3px
    if (usingOnboardSpeaker) {
        // Speaker icon: simple 5-char glyph drawn as text
        display.setColor(BLACK);
        display.setFont(ArialMT_Plain_10);
        display.setTextAlignment(TEXT_ALIGN_LEFT);
        display.drawString(2, 1, "spk");
        display.setColor(WHITE);
    } else if (btConnected) {
        display.setColor(BLACK);
        display.drawXbm(5, 2, BT_ICON_WIDTH, BT_ICON_HEIGHT, bt_icon_bits);
        display.setColor(WHITE);
    }

    // Battery in header: outline with % text inside, bolt if charging — shifted left 3px
    display.setColor(BLACK);
    display.setFont(ArialMT_Plain_10);
    int battPct = (int)batteryVoltagePercentage;
    if (battPct > 100) battPct = 100;
    if (battPct < 0) battPct = 0;
    String battStr = String(battPct);
    int textW = display.getStringWidth(battStr);
    int bodyW = textW + 6;              // 3px padding each side
    int bodyH = 10;
    int bodyX = 121 - bodyW;            // shifted left 3px (was 124 - bodyW)
    int bodyY = 2;
    display.drawRect(bodyX, bodyY, bodyW, bodyH);
    display.fillRect(bodyX + bodyW, bodyY + 3, 2, 4);       // terminal nub
    display.setTextAlignment(TEXT_ALIGN_CENTER);
    display.drawString(bodyX + bodyW / 2, bodyY - 1, battStr);
    if (batteryChangeRate > 0.5f) {
        display.drawXbm(bodyX - BOLT_ICON_WIDTH - 1, bodyY + 1, BOLT_ICON_WIDTH, BOLT_ICON_HEIGHT, bolt_icon_bits);
    }
    display.setColor(WHITE);

    display.setFont(ArialMT_Plain_10);
    display.setTextAlignment(TEXT_ALIGN_CENTER);

    // Artist — shifted up 7px (was y=15)
    if (nowPlayingArtist.length()) {
        display.drawString(64, 8, nowPlayingArtist);
    }

    // Title with marquee — shifted up 7px (was y=26)
    String titleText = nowPlayingTitle.length() ? nowPlayingTitle :
                       (currentTrackIndex >= 0 && currentTrackIndex < (int)trackLibrary.size()) ?
                       trackLibrary[currentTrackIndex].display : "No track";

    int titleWidth = display.getStringWidth(titleText);
    int maxWidth = 120;
    if (titleWidth > maxWidth) {
        if (millis() - lastMarqueeUpdate > 300) {
            lastMarqueeUpdate = millis();
            marqueeOffset += 6;
            if (marqueeOffset > titleWidth - maxWidth + 30) {
                marqueeOffset = -20;
            }
        }
        display.setTextAlignment(TEXT_ALIGN_LEFT);
        display.drawString(4 - marqueeOffset, 19, titleText);
        display.setTextAlignment(TEXT_ALIGN_CENTER);
    } else {
        display.drawString(64, 19, titleText);
    }

    // Play/Pause + shuffle — shifted up 7px (was y=38)
    String status = isPlaying ? "> Playing" : "|| Paused";
    if (shuffleEnabled) status += "  [S]";
    display.drawString(64, 31, status);

    // Bottom bar: volume overlay > visualizer > playhead (priority order)
    bool showVolume = (lastVolumeChangeTime > 0 && millis() - lastVolumeChangeTime < 2000);
    if (showVolume) {
        // Volume overlay — shows briefly when slider is touched
        int volumePct = (int)(100.0f - sliderPosition_Percentage_Filtered);
        if (volumePct < 0) volumePct = 0;
        if (volumePct > 100) volumePct = 100;
        display.drawProgressBar(14, 50, 100, 6, volumePct);
        display.setFont(ArialMT_Plain_10);
        display.setTextAlignment(TEXT_ALIGN_LEFT);
        display.drawString(0, 47, "V");
        display.setTextAlignment(TEXT_ALIGN_RIGHT);
        display.drawString(128, 47, String(volumePct));
    } else if (visualizerMode == VIZ_AMPLITUDE && isPlaying) {
        // 16-bar amplitude visualizer (time-domain), y=48-63
        for (int i = 0; i < 16; i++) {
            int idx = (amplitudeHistoryIndex + i) % 16;
            int barH = (int)(amplitudeHistory[idx] * 14.0f);
            if (barH < 1 && amplitudeHistory[idx] > 0.01f) barH = 1;
            if (barH > 0) {
                display.fillRect(i * 8, 63 - barH, 7, barH);
            }
        }
    } else if (visualizerMode == VIZ_SPECTRUM && isPlaying) {
        // 16-bar spectrum visualizer (frequency-domain, VFD-style), y=48-63
        for (int i = 0; i < 16; i++) {
            int barH = (int)(spectrumBands[i] * 14.0f);
            if (barH > 14) barH = 14;
            if (barH < 1 && spectrumBands[i] > 0.01f) barH = 1;
            if (barH > 0) {
                display.fillRect(i * 8, 63 - barH, 7, barH);
            }
        }
    } else {
        // Playhead with elapsed / remaining time
        size_t filePos = 0, fileSize = currentTrackFileSize;
        Stream* s = (pPlayer && currentTrackIndex >= 0) ? pPlayer->getStream() : nullptr;
        if (s) {
            filePos = ((File*)s)->position();
            if (fileSize == 0) fileSize = ((File*)s)->size();
        }

        // Progress bar (byte-based, works even without bitrate)
        int progressPct = (fileSize > 0) ? (int)((uint64_t)filePos * 100 / fileSize) : 0;
        if (progressPct > 100) progressPct = 100;

        // Time estimation from bitrate
        int elapsedSec = 0, remainSec = 0;
        bool hasTime = (currentTrackBitrate > 0 && fileSize > 0);
        if (hasTime) {
            elapsedSec = (int)((uint64_t)filePos * 8 / currentTrackBitrate);
            int totalSec = (int)((uint64_t)fileSize * 8 / currentTrackBitrate);
            remainSec = totalSec - elapsedSec;
            if (remainSec < 0) remainSec = 0;
        }

        // Format times as M:SS
        char elapsedStr[8], remainStr[8];
        snprintf(elapsedStr, sizeof(elapsedStr), "%d:%02d", elapsedSec / 60, elapsedSec % 60);
        snprintf(remainStr, sizeof(remainStr), "-%d:%02d", remainSec / 60, remainSec % 60);

        display.setFont(ArialMT_Plain_10);
        if (hasTime) {
            // Draw time labels
            display.setTextAlignment(TEXT_ALIGN_LEFT);
            display.drawString(0, 47, elapsedStr);
            display.setTextAlignment(TEXT_ALIGN_RIGHT);
            display.drawString(128, 47, remainStr);
            // Progress bar between the time labels
            int leftW = display.getStringWidth(elapsedStr) + 2;
            int rightW = display.getStringWidth(remainStr) + 2;
            int barX = leftW;
            int barW = 128 - leftW - rightW;
            if (barW > 20) {
                display.drawProgressBar(barX, 50, barW, 6, progressPct);
            }
        } else {
            // No bitrate info — just show progress bar full-width
            display.drawProgressBar(4, 50, 120, 6, progressPct);
        }
    }
}

void MusicPlayerApp::renderScanningLibrary() {
    drawHeader("Library");

    display.setFont(ArialMT_Plain_10);
    display.setTextAlignment(TEXT_ALIGN_CENTER);
    display.drawString(64, 24, "Scanning library...");

    if (libraryScanTotal > 0) {
        int pct = (libraryScanCurrent * 100) / libraryScanTotal;
        display.drawProgressBar(10, 40, 108, 10, pct);

        String progress = String(libraryScanCurrent) + " / " + String(libraryScanTotal);
        display.drawString(64, 54, progress);
    }
}

void MusicPlayerApp::renderSwitching() {
    drawHeader("Bluetooth");

    int dots = (millis() / 400) % 4;
    String msg = "Switching";
    for (int i = 0; i < dots; i++) msg += ".";

    display.setFont(ArialMT_Plain_10);
    display.setTextAlignment(TEXT_ALIGN_CENTER);
    display.drawString(64, 24, msg);
    display.drawString(64, 38, switchTargetDevice.name);
}
