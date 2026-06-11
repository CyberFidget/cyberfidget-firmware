// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

// lib/VoiceRecorderApp/VoiceRecorderApp.cpp
//
// "Voice Notes" — record-first voice memo capture.
//
// Pipeline: I2S port 1 (16-bit/mono, owned by this app for its whole
// session) -> capture task on core 0 -> PSRAM SPSC ring -> drained in
// update() through a WAV encoder to /recordings/REC_NNNN.wav.
//
// Two recording qualities, toggled from HOME and persisted in NVS:
// Standard (16kHz, 32KB/s) for memos/transcription, High (48kHz, 96KB/s)
// for production-usable voice. The sample/byte rate flows from recQuality
// into the I2S clock, the WAV AudioInfo, and every UX time estimate.
//
// audio-tools v1.2.0's WAVEncoder streams its header once with placeholder
// sizes and never patches them, so finalizeWav() seek-patches the RIFF and
// data chunk lengths after every stop. That patch rewrites existing bytes —
// it needs no free clusters, which is what makes the card-full auto-stop
// still produce a valid file.
//
// AppManager integration: button callbacks are dispatched from the main
// loop (same context as update()), so handlers mutate state directly.
// Exit goes through MenuManager::instance().returnToMenu() — never call
// end() from inside the app (see MusicPlayerApp double-end note).

#include "VoiceRecorderApp.h"

#include <SPI.h>
#include <esp_heap_caps.h>
#include <sys/time.h>

#include "AudioManager.h"
#include "RGBController.h"
#include "globals.h"

static const char* TAG_VREC = "VoiceRec";

// External fonts (thingpulse OLED lib, same pattern as ClockDisplay)
extern const uint8_t ArialMT_Plain_10[];
extern const uint8_t ArialMT_Plain_16[];
extern const uint8_t ArialMT_Plain_24[];

// --- SD wiring (same SPI map MusicPlayerApp uses) ---
static const int PIN_SD_CLK  = 5;
static const int PIN_SD_MISO = 21;
static const int PIN_SD_MOSI = 19;
static const int PIN_SD_CS   = 8;

// --- Recording constants ---
static const uint64_t      REC_RESERVE_BYTES    = 262144;     // auto-stop floor; sized to swallow a full ring drain
static const uint32_t      LOW_SPACE_WARN_SEC   = 60;
static const unsigned long HOLD_STOP_MS         = 400;        // latch-vs-hold gesture threshold
static const unsigned long NOTE_DURATION_MS     = 2500;
static const unsigned long MIN_VALID_EPOCH      = 1600000000UL; // clock-is-set heuristic (ClockDisplay pattern)
static const unsigned long RECONFIG_WAIT_MS     = 250;       // startRecording waits this long for a pending re-clock

// Digital gain, applied (saturating) to every sample in the capture task.
// The ICS-43434 is quiet by design (94dB SPL = -26dBFS), so normal speech
// lands far down the 16-bit range; x8 (+18dB) brings voice memos up to a
// comfortable playback level. This is the tuning knob if recordings still
// play back quiet — each +1 shift adds 6dB but eats 6dB of clip headroom.
static const int REC_GAIN_SHIFT = 3;

// Audio to discard at the start of every recording so the mechanical click
// of the record press doesn't open the file. Time-based so it holds at
// either byte rate (bytes = recByteRate/1000 * ms; both rates are exact
// multiples of 1000, so the division never truncates).
static const uint32_t REC_START_SKIP_MS = 100;

// Audio to drop from the END of a recording when the stop was triggered
// by a button PRESS (tap-mode second press, or Select) — the press click
// lands right at the tail. Applied only to the declared data length in
// the WAV header (the bytes stay in the file past the chunk; players
// ignore them), so it can't corrupt anything. Hold-mode release stops and
// auto-stops keep the full tail: a release is quiet, and trimming real
// speech is worse than a soft click. Time-based (see REC_START_SKIP_MS).
static const uint32_t REC_TAIL_TRIM_MS = 150;

// --- Tape-deck UI layout ---
//
// Enclosure window bleed: the case clips roughly 12px 45-degree triangles
// off all four display corners (playtest: the old bottom-right "ENTER =
// REC" hint lost the tail of its C to the corner). Keep ink out of:
//   top-left      x + y           < 12
//   top-right     (127 - x) + y   < 12
//   bottom-left   x + (63 - y)    < 12
//   bottom-right  (127-x)+(63-y)  < 12
// Layout principle: the label strip lives INSIDE the cassette above the
// reels, so text and wheels are separated vertically and can never
// collide regardless of text width. The bottom-right sub-area under the
// VU bar only ever shows short numeric text (~44px max at its lowest row).
static const int16_t CAS_X = 14, CAS_Y = 2, CAS_W = 100, CAS_H = 36;
static const int16_t REEL_LEFT_X  = 44;
static const int16_t REEL_RIGHT_X = 84;
static const int16_t REEL_Y       = 25;  // rim r=10: reel tops at y15
static const int16_t REEL_RIM_R   = 10;
static const int16_t STRIP_TEXT_Y = 3;   // Plain_10 ink ~y5-13, above the reels
static const int16_t TIMER_X = 12, TIMER_Y = 38;
// VU right edge (VU_X + VU_W - 1 = 113) aligns with the cassette's right
// edge, mirroring the timer's alignment to its left edge.
static const int16_t VU_X = 74, VU_Y = 43, VU_W = 40, VU_H = 8;
static const uint8_t VU_FILL_MAX   = 36;  // inner fill width (VU_W - 4)
static const uint8_t VU_GOOD_LO_PX = 18;  // ~ -20 dBFS post-gain
static const uint8_t VU_GOOD_HI_PX = 32;  // ~ -6 dBFS post-gain
static const int16_t SUB_CENTER_X = 94, SUB_TEXT_Y = 52;

// sin(3°·k) scaled to 127, k = 0..119. cos(x) = sin(x + 90°) = +30 entries.
static const int8_t kSin3[120] = {
       0,   7,  13,  20,  26,  33,  39,  46,  52,  58,  63,  69,
      75,  80,  85,  90,  94,  99, 103, 107, 110, 113, 116, 119,
     121, 123, 124, 125, 126, 127, 127, 127, 126, 125, 124, 123,
     121, 119, 116, 113, 110, 107, 103,  99,  94,  90,  85,  80,
      75,  69,  63,  58,  52,  46,  39,  33,  26,  20,  13,   7,
       0,  -7, -13, -20, -26, -33, -39, -46, -52, -58, -64, -69,
     -75, -80, -85, -90, -94, -99,-103,-107,-110,-113,-116,-119,
    -121,-123,-124,-125,-126,-127,-127,-127,-126,-125,-124,-123,
    -121,-119,-116,-113,-110,-107,-103, -99, -94, -90, -85, -80,
     -75, -69, -64, -58, -52, -46, -39, -33, -26, -20, -13,  -7,
};
static inline int8_t sin3(int idx)  { return kSin3[((idx % 120) + 120) % 120]; }
static inline int8_t cos3(int idx)  { return kSin3[((idx + 30) % 120)]; }

// Log-ish VU mapping: 0..VU_FILL_MAX from a 0..32767 peak. Integer-only:
// 8 px per octave above the -42 dBFS floor + a 3-bit fraction, scaled to
// the bar's fill width.
static uint8_t vuPixels(uint16_t peak) {
    if (peak < 256) return 0;
    uint8_t msb  = 31 - __builtin_clz((uint32_t)peak);  // 8..14
    uint8_t frac = (peak >> (msb - 3)) & 7;
    uint16_t px  = (uint16_t)(msb - 8) * 8 + frac;      // 0..55
    px = px * VU_FILL_MAX / 55;
    return (px > VU_FILL_MAX) ? VU_FILL_MAX : (uint8_t)px;
}

static bool sdExistsAdapter(const char* path, void* /*ctx*/) {
    return SD.exists(path);
}

VoiceRecorderApp* VoiceRecorderApp::instance = nullptr;
VoiceRecorderApp voiceRecorderApp(HAL::buttonManager());
static auto& display = HAL::displayProxy();

// =========================================================================
// Constructor / static callbacks
// =========================================================================
VoiceRecorderApp::VoiceRecorderApp(ButtonManager& btnMgr)
    : buttonManager(btnMgr)
{
    instance = this;
    recPath[0] = '\0';
}

void VoiceRecorderApp::onButtonEnter(const ButtonEvent& event) {
    if (instance) instance->handleEnterEvent(event);
}

void VoiceRecorderApp::onButtonBack(const ButtonEvent& event) {
    if (instance) instance->handleBackEvent(event);
}

void VoiceRecorderApp::onButtonQuality(const ButtonEvent& event) {
    if (instance) instance->handleQualityEvent(event);
}

// =========================================================================
// Lifecycle
// =========================================================================
void VoiceRecorderApp::begin() {
    state = REC_STATE_HOME;
    stopReason = REC_STOP_USER;
    faultMsg = nullptr;
    homeNoteMsg = nullptr;
    armedHoldStop = false;
    writeErrorSeen = false;
    bytesWritten = 0;
    savedCounter = 0;
    savedDroppedBytes = 0;
    uiVuLevel = 0;
    frameCounter = 0;
    reelAngle = 0;
    lastLedBright = 0xFF;
    sdMounted = false;
    exitRequested.store(false, std::memory_order_relaxed);
    recordingActive.store(false, std::memory_order_relaxed);
    reconfigRequested.store(false, std::memory_order_relaxed);
    vuPeak.store(0, std::memory_order_relaxed);

    // Restore the persisted quality before the port is clocked, so the mic
    // opens at the saved rate (no reconfigure needed on a clean entry).
    {
        Preferences prefs;
        uint8_t q = 0;
        if (prefs.begin("voicerec", true)) {
            q = prefs.getUChar("quality", 0);
            prefs.end();
        }
        applyQuality(q);
    }
    pendingSampleRate.store(recSampleRate, std::memory_order_relaxed);

    setColorsOff();

    buttonManager.registerCallback(button_EnterIndex, onButtonEnter);
    buttonManager.registerCallback(button_SelectIndex, onButtonBack);
    buttonManager.registerCallback(button_UpIndex, onButtonQuality);

    // AudioManager's metering task only holds I2S port 1 while its run flag
    // is set; clear it and give the task (<=5ms poll) time to close the port.
    HAL::audioManager().enableMic(false);
    delay(15);

    if (ringStorage == nullptr) {
        ringCapacity = REC_RING_SIZE_BYTES;
        ringStorage  = (uint8_t*)ps_malloc(ringCapacity);
        if (ringStorage == nullptr) {
            // No PSRAM: a 64KB internal-RAM ring still buffers 2s of audio.
            ringCapacity = 65536;
            ringStorage  = (uint8_t*)heap_caps_malloc(ringCapacity, MALLOC_CAP_8BIT);
        }
    }
    if (ringStorage == nullptr || !ring.init(ringStorage, ringCapacity)) {
        ESP_LOGE(TAG_VREC, "ring alloc failed (%u bytes)", (unsigned)ringCapacity);
        faultMsg = "MEMORY ERROR";
        state = REC_STATE_FAULT;
        return;
    }

    // Mic input — AudioManager.cpp's pin map, at the recording sample rate.
    micCfg = i2sIn.defaultConfig(audio_tools::RX_MODE);
    micCfg.port_no         = 1;
    micCfg.i2s_format      = audio_tools::I2S_STD_FORMAT;
    micCfg.sample_rate     = recSampleRate;
    micCfg.bits_per_sample = 16;
    micCfg.channels        = 1;
    micCfg.pin_ws          = 25;
    micCfg.pin_bck         = 32;
    micCfg.pin_data_rx     = 33;
    micCfg.pin_data        = -1;
    micCfg.is_master       = true;
    micCfg.buffer_count    = 8;    // 8 x 512B DMA: ~128ms cushion at Standard,
    micCfg.buffer_size     = 512;  // ~43ms at High — both comfortably > the 5ms poll
    i2sOpened = i2sIn.begin(micCfg);
    if (!i2sOpened) {
        ESP_LOGE(TAG_VREC, "I2S port 1 open failed");
        faultMsg = "MIC ERROR";
        state = REC_STATE_FAULT;
        return;
    }
    i2sIn.setTimeout(0);

    captureTaskExited.store(false, std::memory_order_relaxed);
    BaseType_t created = xTaskCreatePinnedToCore(
        &VoiceRecorderApp::captureTaskThunk,
        "recPump",
        4096,
        this,
        2,              // above the prio-1 background pumps; mic reads are time-sensitive
        &captureTaskHandle,
        0               // core 0, same as AudioManager's micPump
    );
    if (created != pdPASS) {
        captureTaskExited.store(true, std::memory_order_relaxed);
        captureTaskHandle = nullptr;
        faultMsg = "TASK ERROR";
        state = REC_STATE_FAULT;
        return;
    }

    enterHomeOrNoSd();
}

void VoiceRecorderApp::update() {
    frameCounter++;

    // A recording in progress must never be truncated by auto-sleep.
    if (state == REC_STATE_RECORDING || state == REC_STATE_SAVING) {
        millis_APP_LASTINTERACTION = millis_NOW;
    }

    if (state == REC_STATE_RECORDING) {
        drainToFile(8192);
        if (writeErrorSeen) {
            requestStop(REC_STOP_WRITE_ERROR);
        } else if (remainingSeconds() == 0) {
            requestStop(REC_STOP_CARD_FULL);
        }
    } else if (state == REC_STATE_SAVING) {
        // UI is static here; drain harder than during recording.
        if (!writeErrorSeen) drainToFile(16384);
        if (ring.available() == 0 || writeErrorSeen) {
            finishSave();
        }
    } else if (state == REC_STATE_SAVED_NOTE) {
        if (millis_NOW >= noteUntilMs) dismissNote();
    }

    updateLed();
    render();
}

void VoiceRecorderApp::end() {
    // Defensive finalize: the normal stop path runs through SAVING in
    // update(), but a forced switchToApp() can land here mid-recording.
    if (state == REC_STATE_RECORDING || state == REC_STATE_SAVING) {
        recordingActive.store(false, std::memory_order_release);
        unsigned long t0 = millis();
        while (ring.available() > 0 && !writeErrorSeen && (millis() - t0) < 2000) {
            drainToFile(sizeof(drainBuf));
        }
        closeRecordingFile();
    }

    exitRequested.store(true, std::memory_order_release);
    unsigned long t0 = millis();
    while (!captureTaskExited.load(std::memory_order_acquire) &&
           (millis() - t0) < 500) {
        vTaskDelay(1);
    }
    captureTaskHandle = nullptr;

    if (i2sOpened) {
        i2sIn.end();   // release I2S port 1 — leave shared hardware clean on exit
        i2sOpened = false;
    }

    buttonManager.unregisterCallback(button_EnterIndex);
    buttonManager.unregisterCallback(button_SelectIndex);
    buttonManager.unregisterCallback(button_UpIndex);

    if (ringStorage != nullptr) {
        free(ringStorage);
        ringStorage = nullptr;
    }

    setColorsOff();
}

// =========================================================================
// Button handling (main-loop context — same as update())
// =========================================================================
void VoiceRecorderApp::handleEnterEvent(const ButtonEvent& event) {
    if (event.eventType == ButtonEvent_Held) return;  // we do our own timing

    switch (state) {
        case REC_STATE_HOME:
            if (event.eventType == ButtonEvent_Pressed) {
                startRecording();
                // Hold-to-record: the press that started the recording is
                // armed; a >=400ms release stops it again (record-while-held).
                armedHoldStop = (state == REC_STATE_RECORDING);
            }
            break;

        case REC_STATE_RECORDING:
            if (event.eventType == ButtonEvent_Pressed && !armedHoldStop) {
                // Latched mode: press stops — trim its click off the tail
                requestStop(REC_STOP_USER, /*trimPressClick=*/true);
            } else if (event.eventType == ButtonEvent_Released && armedHoldStop) {
                if (event.duration >= HOLD_STOP_MS) {
                    // Hold mode: release stops — releases are quiet, keep tail
                    requestStop(REC_STOP_USER, /*trimPressClick=*/false);
                } else {
                    armedHoldStop = false;           // short press: latched
                }
            }
            break;

        case REC_STATE_SAVED_NOTE:
            if (event.eventType == ButtonEvent_Pressed) dismissNote();
            break;

        case REC_STATE_NO_SD:
            if (event.eventType == ButtonEvent_Pressed) enterHomeOrNoSd();
            break;

        default:
            break;
    }
}

void VoiceRecorderApp::handleBackEvent(const ButtonEvent& event) {
    if (event.eventType != ButtonEvent_Pressed) return;

    switch (state) {
        case REC_STATE_RECORDING:
            // Never silently discard: back stops and saves, stays in app.
            requestStop(REC_STOP_USER, /*trimPressClick=*/true);
            break;

        case REC_STATE_SAVING:
            break;  // sub-second; ignore

        case REC_STATE_SAVED_NOTE:
            dismissNote();
            break;

        case REC_STATE_HOME:
        case REC_STATE_NO_SD:
        case REC_STATE_FAULT:
        default:
            // AppManager calls end() for us via switchToApp.
            MenuManager::instance().returnToMenu();
            break;
    }
}

// Up button: flip Standard <-> High, but only from HOME — never mid-recording
// (re-clocking the port underneath a live capture would corrupt the file).
void VoiceRecorderApp::handleQualityEvent(const ButtonEvent& event) {
    if (event.eventType != ButtonEvent_Pressed) return;
    if (state == REC_STATE_HOME) toggleQuality();
}

// =========================================================================
// Recording quality
// =========================================================================
void VoiceRecorderApp::applyQuality(uint8_t q) {
    recQuality    = (q == 1) ? 1 : 0;        // tolerate stale/garbage NVS values
    recSampleRate = recQuality ? 48000 : 16000;
    recByteRate   = recSampleRate * 2;       // 16-bit mono = 2 bytes/sample
}

void VoiceRecorderApp::toggleQuality() {
    applyQuality(recQuality ^ 1);

    Preferences prefs;
    if (prefs.begin("voicerec", false)) {
        prefs.putUChar("quality", recQuality);  // alongside "counter"
        prefs.end();
    }

    // Hand the new rate to the capture task. It owns I2S port 1 and applies
    // the re-clock between reads (see captureTaskLoop), so the main loop
    // never calls i2sIn.end()/begin() concurrently with a read.
    pendingSampleRate.store(recSampleRate, std::memory_order_relaxed);
    reconfigRequested.store(true, std::memory_order_release);
}

// =========================================================================
// SD lifecycle
// =========================================================================
bool VoiceRecorderApp::mountSD() {
    if (SD.cardType() != CARD_NONE) {
        // Someone (MusicPlayerApp keeps its mount) already mounted it;
        // probe in case the card was yanked while mounted.
        File root = SD.open("/");
        if (root) {
            root.close();
            sdMounted = true;
            return true;
        }
        SD.end();
    }
    SPI.begin(PIN_SD_CLK, PIN_SD_MISO, PIN_SD_MOSI, PIN_SD_CS);
    sdMounted = SD.begin(PIN_SD_CS);
    return sdMounted;
}

void VoiceRecorderApp::enterHomeOrNoSd() {
    if (!mountSD()) {
        state = REC_STATE_NO_SD;
        return;
    }
    if (!SD.exists(RecNaming::kRecordingsDir)) {
        SD.mkdir(RecNaming::kRecordingsDir);
    }
    refreshFreeBytes();
    state = REC_STATE_HOME;
}

void VoiceRecorderApp::refreshFreeBytes() {
    if (!sdMounted) {
        cachedFreeBytes = 0;
        return;
    }
    // usedBytes() scans FAT free clusters on first call — done only at app
    // entry and after each save, never per-tick.
    uint64_t total = SD.totalBytes();
    uint64_t used  = SD.usedBytes();
    cachedFreeBytes = (total > used) ? (total - used) : 0;
}

uint32_t VoiceRecorderApp::remainingSeconds() const {
    uint64_t committed = bytesWritten + REC_RESERVE_BYTES;
    if (cachedFreeBytes <= committed) return 0;
    return (uint32_t)((cachedFreeBytes - committed) / recByteRate);
}

// =========================================================================
// Recording session
// =========================================================================
void VoiceRecorderApp::startRecording() {
    if (!sdMounted) {
        state = REC_STATE_NO_SD;
        return;
    }

    // If the user toggled quality and immediately pressed record, the capture
    // task may not have re-clocked the port yet. Wait it out so the I2S rate
    // matches the AudioInfo we're about to write into the header — otherwise
    // the file would declare the new rate but carry samples at the old one.
    unsigned long reconfigT0 = millis();
    while (reconfigRequested.load(std::memory_order_acquire) &&
           (millis() - reconfigT0) < RECONFIG_WAIT_MS) {
        delay(2);
    }

    ring.reset();
    ring.clearDropped();
    writeErrorSeen = false;

    uint32_t startCounter = 1;
    Preferences prefs;
    if (prefs.begin("voicerec", true)) {
        startCounter = prefs.getUInt("counter", 0) + 1;
        prefs.end();
    }
    recCounter = RecNaming::nextFreeCounter(startCounter, sdExistsAdapter, nullptr);
    if (recCounter == RecNaming::kCounterExhausted ||
        RecNaming::formatRecPath(recPath, sizeof(recPath), recCounter) < 0) {
        showNote("SD ERROR");
        return;
    }

    if (!SD.exists(RecNaming::kRecordingsDir)) {
        SD.mkdir(RecNaming::kRecordingsDir);  // card may have been swapped
    }
    recFile = SD.open(recPath, FILE_WRITE);
    if (!recFile) {
        ESP_LOGW(TAG_VREC, "open failed: %s", recPath);
        if (!mountSD()) {
            state = REC_STATE_NO_SD;
        } else {
            showNote("SD ERROR");
        }
        return;
    }

    pEncOut = new audio_tools::EncodedAudioOutput(&recFile, &wavEncoder);
    audio_tools::AudioInfo info(recSampleRate, 1, 16);
    if (!pEncOut->begin(info)) {
        delete pEncOut;
        pEncOut = nullptr;
        recFile.close();
        SD.remove(recPath);
        showNote("SD ERROR");
        return;
    }

    recStartEpoch = time(nullptr);
    recStartMs = millis_NOW;
    bytesWritten = 0;

    tailTrimBytes = 0;
    finalDataBytes = 0;
    // Must be set before recordingActive: the task only reads it after
    // acquiring the recording flag. recByteRate is an exact multiple of 1000.
    skipBytesRemaining.store((recByteRate / 1000) * REC_START_SKIP_MS,
                             std::memory_order_relaxed);
    recordingActive.store(true, std::memory_order_release);
    state = REC_STATE_RECORDING;
    ESP_LOGI(TAG_VREC, "recording -> %s @ %u Hz (q=%u)", recPath,
             (unsigned)recSampleRate, (unsigned)recQuality);
}

void VoiceRecorderApp::requestStop(VoiceRecStopReason reason, bool trimPressClick) {
    recordingActive.store(false, std::memory_order_release);
    stopReason = reason;
    tailTrimBytes = trimPressClick ? (recByteRate / 1000) * REC_TAIL_TRIM_MS : 0;
    armedHoldStop = false;
    state = REC_STATE_SAVING;
}

// Pop from the ring into the WAV encoder, up to maxBytes. Tracks short
// writes (card yanked / FS error) in writeErrorSeen.
uint32_t VoiceRecorderApp::drainToFile(uint32_t maxBytes) {
    if (pEncOut == nullptr) return 0;
    uint32_t total = 0;
    while (total < maxBytes) {
        uint32_t want = maxBytes - total;
        if (want > sizeof(drainBuf)) want = sizeof(drainBuf);
        uint32_t chunk = ring.pop(drainBuf, want);
        if (chunk == 0) break;
        size_t written = pEncOut->write(drainBuf, chunk);
        bytesWritten += written;
        total += written;
        if (written < chunk) {
            ESP_LOGE(TAG_VREC, "short write (%u of %u)", (unsigned)written,
                     (unsigned)chunk);
            writeErrorSeen = true;
            break;
        }
    }
    return total;
}

// Shared by the normal SAVING path and the defensive end() path.
// Returns true if the WAV on card is finalized (header patched).
bool VoiceRecorderApp::closeRecordingFile() {
    if (pEncOut != nullptr) {
        pEncOut->end();
        delete pEncOut;
        pEncOut = nullptr;
    }
    bool wavOk = finalizeWav();
    if (wavOk) {
        appendIndexRow();
        Preferences prefs;
        if (prefs.begin("voicerec", false)) {
            // Best-effort: a lost write self-heals via collision skip.
            prefs.putUInt("counter", recCounter);
            prefs.end();
        }
    }
    return wavOk;
}

void VoiceRecorderApp::finishSave() {
    savedDroppedBytes = ring.droppedBytes();
    savedCounter = recCounter;

    bool wavOk = closeRecordingFile();
    if (!wavOk && stopReason == REC_STOP_USER) {
        stopReason = REC_STOP_WRITE_ERROR;
    }

    if (stopReason == REC_STOP_WRITE_ERROR) {
        // Card may be gone entirely; SAVED_NOTE falls through to NO_SD
        // on dismiss when the re-probe fails.
        mountSD();
    }
    refreshFreeBytes();

    homeNoteMsg = nullptr;
    state = REC_STATE_SAVED_NOTE;
    noteUntilMs = millis_NOW + NOTE_DURATION_MS;
    if (savedDroppedBytes > 0) {
        ESP_LOGW(TAG_VREC, "ring overflow: %u bytes dropped",
                 (unsigned)savedDroppedBytes);
    }
}

// Patch the RIFF (offset 4) and data (offset 40) chunk sizes the streaming
// WAVEncoder left as placeholders. Header layout verified against
// audio-tools v1.2.0 WAVHeader::writeHeader: 44 bytes, PCM, no junk chunk.
bool VoiceRecorderApp::finalizeWav() {
    if (!recFile) return false;
    recFile.flush();
    uint32_t sz = recFile.size();
    if (sz < 44) {
        // Nothing usable was written (stopped instantly / first write failed).
        recFile.close();
        SD.remove(recPath);
        return false;
    }
    uint32_t dataLen = (sz - 44) & ~1u;  // even-trim a torn trailing byte
    uint32_t trim = tailTrimBytes & ~1u; // press-stop click trim (sample-aligned)
    if (dataLen > trim) dataLen -= trim; // tiny recordings keep what they have
    finalDataBytes = dataLen;            // what index.csv should report

    auto writeLE32At = [&](uint32_t offset, uint32_t value) -> bool {
        uint8_t b[4] = {
            (uint8_t)(value & 0xFF),
            (uint8_t)((value >> 8) & 0xFF),
            (uint8_t)((value >> 16) & 0xFF),
            (uint8_t)((value >> 24) & 0xFF),
        };
        return recFile.seek(offset) && recFile.write(b, 4) == 4;
    };

    // Also stamp the sample/byte rate (offsets 24/28) from the selected
    // quality. The streaming encoder writes these from its AudioInfo, but we
    // own the authoritative rate here — patching them guarantees the header
    // can never disagree with the I2S clock (a High file must declare 48kHz,
    // or it plays back slow on a PC). block_align (32) and bits (34) are
    // rate-independent (mono/16-bit), so they're left as the encoder wrote.
    bool ok = writeLE32At(4, 36 + dataLen) &&
              writeLE32At(24, recSampleRate) &&
              writeLE32At(28, recByteRate) &&
              writeLE32At(40, dataLen);
    recFile.flush();
    recFile.close();
    if (!ok) {
        ESP_LOGE(TAG_VREC, "header patch failed: %s", recPath);
    }
    return ok;
}

void VoiceRecorderApp::appendIndexRow() {
    char name[20];
    if (RecNaming::formatRecBasename(name, sizeof(name), recCounter) < 0) return;

    struct tm tmBuf;
    struct tm* tmPtr = nullptr;
    if (recStartEpoch >= (time_t)MIN_VALID_EPOCH) {
        if (localtime_r(&recStartEpoch, &tmBuf) != nullptr) tmPtr = &tmBuf;
    }

    char row[96];
    uint64_t dataBytes = (finalDataBytes > 0) ? finalDataBytes : bytesWritten;
    uint32_t durationS =
        RecNaming::durationSecondsFromPcmBytes(dataBytes, recByteRate);
    if (RecNaming::formatIndexRow(row, sizeof(row), name, tmPtr, durationS,
                                  dataBytes) < 0) {
        return;
    }

    char indexPath[40];
    snprintf(indexPath, sizeof(indexPath), "%s/index.csv",
             RecNaming::kRecordingsDir);
    bool isNew = !SD.exists(indexPath);
    File idx = SD.open(indexPath, FILE_APPEND);
    if (!idx) return;  // non-fatal: the WAV itself is already safe
    if (isNew) idx.print(RecNaming::indexHeader());
    idx.print(row);
    idx.close();
}

void VoiceRecorderApp::showNote(const char* msg) {
    savedCounter = 0;
    savedDroppedBytes = 0;
    homeNoteMsg = msg;
    state = REC_STATE_SAVED_NOTE;
    noteUntilMs = millis_NOW + NOTE_DURATION_MS;
}

void VoiceRecorderApp::dismissNote() {
    homeNoteMsg = nullptr;
    state = sdMounted ? REC_STATE_HOME : REC_STATE_NO_SD;
}

// =========================================================================
// Capture task (core 0) — reads I2S, publishes VU peak, feeds the ring
// =========================================================================
void VoiceRecorderApp::captureTaskThunk(void* arg) {
    static_cast<VoiceRecorderApp*>(arg)->captureTaskLoop();
}

void VoiceRecorderApp::captureTaskLoop() {
    const TickType_t idleDelay = pdMS_TO_TICKS(5);
    while (!exitRequested.load(std::memory_order_acquire)) {
        // Quality re-clock — only this task ever calls i2sIn.end()/begin()
        // during a session, so re-opening the port here can't race a read.
        // Guarded on !recordingActive: a toggle is HOME-only, but check
        // anyway so a live capture is never re-clocked underneath itself.
        if (reconfigRequested.load(std::memory_order_acquire) &&
            !recordingActive.load(std::memory_order_acquire)) {
            uint32_t newRate = pendingSampleRate.load(std::memory_order_relaxed);
            if (newRate != micCfg.sample_rate || !i2sOpened) {
                i2sIn.end();
                micCfg.sample_rate = newRate;
                i2sOpened = i2sIn.begin(micCfg);
                if (i2sOpened) {
                    i2sIn.setTimeout(0);
                } else {
                    ESP_LOGE(TAG_VREC, "I2S re-clock to %u Hz failed",
                             (unsigned)newRate);
                }
            }
            reconfigRequested.store(false, std::memory_order_release);
            vuPeak.store(0, std::memory_order_relaxed);  // drop the stale peak
            continue;
        }
        if (!i2sOpened) {                 // re-clock failed; idle until retried
            vTaskDelay(idleDelay);
            continue;
        }

        int avail = i2sIn.available();
        if (avail > 0) {
            size_t toRead = (size_t)avail;
            if (toRead > sizeof(captureBuf)) toRead = sizeof(captureBuf);
            int n = i2sIn.readBytes(captureBuf, toRead);
            if (n > 0) {
                // Apply digital gain in place (saturating) and track the
                // post-gain peak so the VU's good-level band reflects what
                // actually lands in the file.
                int16_t* samples = (int16_t*)captureBuf;
                int count = n / 2;
                uint16_t peak = vuPeak.load(std::memory_order_relaxed);
                for (int i = 0; i < count; ++i) {
                    int32_t v = (int32_t)samples[i] << REC_GAIN_SHIFT;
                    if (v > 32767) v = 32767;
                    else if (v < -32768) v = -32768;
                    samples[i] = (int16_t)v;
                    uint16_t mag = (uint16_t)(v < 0 ? -v : v);
                    if (mag > peak) peak = mag;
                }
                vuPeak.store(peak, std::memory_order_relaxed);
                if (recordingActive.load(std::memory_order_acquire)) {
                    // Discard the first 100ms so the record-press click
                    // doesn't open the file.
                    uint32_t skip = skipBytesRemaining.load(std::memory_order_relaxed);
                    uint32_t offset = 0;
                    if (skip > 0) {
                        offset = (skip < (uint32_t)n) ? skip : (uint32_t)n;
                        skipBytesRemaining.store(skip - offset,
                                                 std::memory_order_relaxed);
                    }
                    if ((uint32_t)n > offset) {
                        ring.push(captureBuf + offset,
                                  (uint32_t)n - offset);  // drop-newest inside
                    }
                }
            }
        } else {
            vTaskDelay(idleDelay);
        }
    }
    captureTaskExited.store(true, std::memory_order_release);
    vTaskDelete(nullptr);
}

// =========================================================================
// LED policy — faint red on the front-bottom pixel (index 3) while
// recording (playtest moved it down from front-top: 2026-06-10)
// =========================================================================
void VoiceRecorderApp::updateLed() {
    uint8_t bright = 0;
    if (state == REC_STATE_RECORDING) {
        bright = 8;
        if (remainingSeconds() < LOW_SPACE_WARN_SEC) {
            bright = ((millis_NOW / 250) & 1) ? 48 : 8;  // brief warning flashes
        }
    } else if (state == REC_STATE_SAVING) {
        bright = 8;  // stays on until the file is safe
    }

    if (bright != lastLedBright) {
        if (bright == 0) {
            setColorsOff();
        } else {
            HAL::setRgbLed(pixel_Front_Bottom, bright, 0, 0, 0);
        }
        lastLedBright = bright;
    }
}

// =========================================================================
// Rendering — 1-bit wireframe tape deck on 128x64
// =========================================================================
void VoiceRecorderApp::render() {
    display.clear();
    switch (state) {
        case REC_STATE_HOME:
        case REC_STATE_RECORDING:
        case REC_STATE_SAVING:
            drawHomeOrRecording();
            break;
        case REC_STATE_SAVED_NOTE:
            drawSavedNote();
            break;
        case REC_STATE_NO_SD:
            drawNoSd();
            break;
        case REC_STATE_FAULT:
            drawFault();
            break;
    }
    display.display();
}

void VoiceRecorderApp::drawCassette(bool spinning, bool xOut) {
    display.drawRect(CAS_X, CAS_Y, CAS_W, CAS_H);     // cassette body
    display.drawLine(54, 31, 74, 31);                 // tape run between reels

    // Time-based spin (one 3° step per 20ms = 150°/s). Driving it off the
    // clock instead of frame count keeps the reels smooth even when update()
    // hitches under the heavier SD write load at High quality — the old
    // per-frame advance ebbed and flowed with the frame rate.
    if (spinning) reelAngle = (uint8_t)((millis_NOW / 20) % 40);

    const int16_t centers[2] = { REEL_LEFT_X, REEL_RIGHT_X };
    for (int r = 0; r < 2; ++r) {
        int16_t cx = centers[r];
        display.drawCircle(cx, REEL_Y, REEL_RIM_R);   // reel rim
        display.drawCircle(cx, REEL_Y, 3);            // hub
        for (int s = 0; s < 3; ++s) {
            int a = reelAngle + s * 40;               // 3 spokes, 120° apart
            int8_t sn = sin3(a);
            int8_t cs = cos3(a);
            int16_t ix = cx + (4 * sn) / 127, iy = REEL_Y - (4 * cs) / 127;
            int16_t ox = cx + (8 * sn) / 127, oy = REEL_Y - (8 * cs) / 127;
            display.drawLine(ix, iy, ox, oy);
        }
        if (xOut) {
            display.drawLine(cx - 8, REEL_Y - 8, cx + 8, REEL_Y + 8);
            display.drawLine(cx - 8, REEL_Y + 8, cx + 8, REEL_Y - 8);
        }
    }
}

void VoiceRecorderApp::drawHomeOrRecording() {
    bool recording = (state == REC_STATE_RECORDING);
    drawCassette(recording, false);

    // Label strip inside the cassette, above the reels — vertically clear
    // of the wheels, so any label width is safe
    display.setFont(ArialMT_Plain_10);
    display.setTextAlignment(TEXT_ALIGN_CENTER);
    if (recording) {
        if ((millis_NOW / 500) & 1) display.fillCircle(47, 9, 3);  // blinking dot
        display.drawString(64, STRIP_TEXT_Y, "REC");
    } else if (state == REC_STATE_SAVING) {
        display.drawString(64, STRIP_TEXT_Y, "STOP");
    } else {
        // The record hint lives here (full strip width) — its old
        // bottom-right spot is clipped by the enclosure corner
        display.drawString(64, STRIP_TEXT_Y,
                           ((millis_NOW / 2000) & 1) ? "ENTER = REC" : "READY");
    }

    // High-quality tag tucked into the cassette's lower-right corner (right
    // of the right reel, clear of the label strip above); Standard shows
    // nothing (it's the default).
    if (recQuality) {
        display.setTextAlignment(TEXT_ALIGN_RIGHT);
        display.drawString(111, 26, "HQ");
    }

    // Elapsed timer, large type, bottom-left
    unsigned long elapsedS =
        (recording || state == REC_STATE_SAVING)
            ? (millis_NOW - recStartMs) / 1000UL
            : 0;
    if (elapsedS > 5999) elapsedS = 5999;  // display clamp at 99:59
    char timer[8];
    snprintf(timer, sizeof(timer), "%02lu:%02lu", elapsedS / 60, elapsedS % 60);
    display.setFont(ArialMT_Plain_24);
    display.setTextAlignment(TEXT_ALIGN_LEFT);
    display.drawString(TIMER_X, TIMER_Y, timer);

    // VU bar; good-level band tick placement gets a manual tuning pass
    // against real speech before this is considered final
    uint16_t peakNow = vuPeak.exchange(0, std::memory_order_relaxed);
    uint16_t decayed = (uiVuLevel > 8) ? uiVuLevel - uiVuLevel / 8 : 0;
    uiVuLevel = (peakNow > decayed) ? peakNow : decayed;
    display.drawRect(VU_X, VU_Y, VU_W, VU_H);
    uint8_t px = vuPixels(uiVuLevel);
    if (px > 0) display.fillRect(VU_X + 2, VU_Y + 2, px, VU_H - 4);
    display.drawLine(VU_X + 2 + VU_GOOD_LO_PX, VU_Y - 3, VU_X + 2 + VU_GOOD_LO_PX, VU_Y - 1);
    display.drawLine(VU_X + 2 + VU_GOOD_HI_PX, VU_Y - 3, VU_X + 2 + VU_GOOD_HI_PX, VU_Y - 1);

    // Short numeric sub-line under the VU bar. The bottom-right corner
    // keep-out leaves only ~44px of safe width at this height — keep it
    // terse; anything verbose belongs in the label strip.
    display.setFont(ArialMT_Plain_10);
    display.setTextAlignment(TEXT_ALIGN_CENTER);
    char sub[16];
    if (state == REC_STATE_SAVING) {
        display.drawString(SUB_CENTER_X, SUB_TEXT_Y, "SAVING");
    } else if (recording) {
        uint32_t remainS = remainingSeconds();
        if (remainS < LOW_SPACE_WARN_SEC) {
            // Low-space warning: fill bar + blinking countdown, alternating
            if ((millis_NOW / 1000) & 1) {
                display.drawRect(VU_X, 54, VU_W, 6);
                uint16_t fill = (uint16_t)((LOW_SPACE_WARN_SEC - remainS) * VU_FILL_MAX / LOW_SPACE_WARN_SEC);
                if (fill > 0) display.fillRect(VU_X + 2, 56, fill, 2);
            } else {
                snprintf(sub, sizeof(sub), "-0:%02lu", (unsigned long)remainS);
                display.drawString(SUB_CENTER_X, SUB_TEXT_Y, sub);
            }
        } else if (remainS < 6000) {
            snprintf(sub, sizeof(sub), "-%lu:%02lu",
                     (unsigned long)(remainS / 60), (unsigned long)(remainS % 60));
            display.drawString(SUB_CENTER_X, SUB_TEXT_Y, sub);
        } else {
            snprintf(sub, sizeof(sub), "-%luh", (unsigned long)(remainS / 3600));
            display.drawString(SUB_CENTER_X, SUB_TEXT_Y, sub);
        }
    } else {
        // HOME: remaining tape, short form ("84m" / "27h")
        uint64_t freeB = (cachedFreeBytes > REC_RESERVE_BYTES)
                             ? cachedFreeBytes - REC_RESERVE_BYTES : 0;
        unsigned long freeMin = (unsigned long)(freeB / recByteRate / 60);
        if (freeMin >= 600) {
            snprintf(sub, sizeof(sub), "%luh", freeMin / 60);
        } else {
            snprintf(sub, sizeof(sub), "%lum", freeMin);
        }
        display.drawString(SUB_CENTER_X, SUB_TEXT_Y, sub);
    }
}

void VoiceRecorderApp::drawSavedNote() {
    drawCassette(false, false);
    display.setFont(ArialMT_Plain_10);
    display.setTextAlignment(TEXT_ALIGN_CENTER);

    char line1[28];
    char line2[28];
    line2[0] = '\0';

    if (homeNoteMsg != nullptr) {
        snprintf(line1, sizeof(line1), "%s", homeNoteMsg);
    } else {
        switch (stopReason) {
            case REC_STOP_CARD_FULL:   snprintf(line1, sizeof(line1), "TAPE FULL"); break;
            case REC_STOP_WRITE_ERROR: snprintf(line1, sizeof(line1), "SAVE ERROR"); break;
            case REC_STOP_USER:
            default:                   snprintf(line1, sizeof(line1), "Saved"); break;
        }
        if (savedDroppedBytes > 0) {
            // Rarer and more important than the filename: surface the gap.
            unsigned long tenths = savedDroppedBytes / (recByteRate / 10);
            snprintf(line2, sizeof(line2), "gap: %lu.%lus lost",
                     tenths / 10, tenths % 10);
        } else if (savedCounter > 0) {
            char name[20];
            if (RecNaming::formatRecBasename(name, sizeof(name), savedCounter) > 0) {
                snprintf(line2, sizeof(line2), "%s", name);
            }
        }
    }

    display.drawString(64, 39, line1);
    if (line2[0] != '\0') display.drawString(64, 50, line2);
}

void VoiceRecorderApp::drawNoSd() {
    drawCassette(false, true);
    display.setFont(ArialMT_Plain_10);
    display.setTextAlignment(TEXT_ALIGN_CENTER);
    display.drawString(64, STRIP_TEXT_Y, "NO TAPE");
    display.drawString(64, 39, "insert memory card");
    display.drawString(64, 50, "ENTER = retry");
}

void VoiceRecorderApp::drawFault() {
    display.setFont(ArialMT_Plain_16);
    display.setTextAlignment(TEXT_ALIGN_CENTER);
    display.drawString(64, 8, "ERROR");
    display.setFont(ArialMT_Plain_10);
    display.drawString(64, 30, faultMsg != nullptr ? faultMsg : "UNKNOWN");
    display.drawString(64, 47, "SELECT = exit");
}
