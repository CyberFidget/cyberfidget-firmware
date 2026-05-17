// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

// lib/AudioManager/AudioManager.cpp
#include "AudioManager.h"
#include "globals.h"
#include <math.h>

AudioManager::AudioManager()
    : currentFrequency(440.0f),
      isPlaying(false),
      stopAtMillis(0),
      in(generator),
      volume(in),
      copier(i2s, volume),
      micCopy(micMeter, i2sIn)
{
}

void AudioManager::init() {
    // --- TX: MAX98357A path ---
    auto cfg = i2s.defaultConfig(TX_MODE);
    cfg.port_no         = 0;
    cfg.i2s_format      = I2S_LSB_FORMAT;   // 
    cfg.pin_ws          = 27;               // LRCLK
    cfg.pin_bck         = 26;               // BCLK
    cfg.pin_data        = 14;               // DOUT
    cfg.channels        = 2;
    cfg.bits_per_sample = 16;
    // cfg.sample_rate     = 44100;
    cfg.buffer_count = 12;    // default is usually smaller
    cfg.buffer_size  = 256;  // bytes per buffer

    // Keep I2S port default here (usually 0). We’ll put mic on the other port.
    i2s.begin(cfg);

    generator.setFrequency(currentFrequency);

    auto vcfg = volume.defaultConfig();
    vcfg.copyFrom(cfg);
    volume.begin(vcfg);
    volume.setVolume(0.7f);

    isPlaying = false;

    // --- Prepare (do NOT start) RX: ICS-43434 mic ---
    // Put mic on the *other* I2S peripheral to avoid any cross-talk.
    // ESP32 has I2S0/I2S1; if TX is using default (0), use 1 here.
    micCfg = i2sIn.defaultConfig(RX_MODE);
    micCfg.port_no         = 1;              // << important: separate port
    micCfg.i2s_format      = I2S_STD_FORMAT; // ICS-43434 standard I2S
    micCfg.sample_rate     = 44100;          // or your preferred rate
    micCfg.bits_per_sample = 16;             // PDM->I2S mics often packed as 24-in-32, except here
    micCfg.channels        = 1;              // mono mic
    micCfg.pin_ws          = 25;             // LRCLK
    micCfg.pin_bck         = 32;             // BCLK
    micCfg.pin_data_rx     = 33;             // DATA IN
    micCfg.pin_data        = -1;             // not used for RX
    micCfg.is_master       = true;
    micCfg.buffer_count    = 6;
    micCfg.buffer_size     = 512;

    // Make mic opt-in:
    micRunRequested = false;   // opt-in
    micRunning      = false;
    micVolumeAtomic  = 0.0f;

    
    // Create the mic pump task ONCE, pinned to core 0
    if (micTaskHandle == nullptr) {
    xTaskCreatePinnedToCore(
        &AudioManager::micTaskThunk, // task entry
        "micPump",      // name
        4096,           // stack size
        this,           // arg = this
        1,              // low priority
        &micTaskHandle, // task handle
        0               // Core 0
    );
}
}

void AudioManager::loop() {
    // --- Tone path ---
    if (isPlaying) {
      copier.copy(); // this one already pulls fast/non-blocking
    if (stopAtMillis > 0 && millis() >= stopAtMillis) {
      stopTone();
      stopAtMillis = 0;
      }
    }

    // --- Sequence advance ---
    if (currentSequence != nullptr && millis() >= nextStepAtMs) {
        if (currentSequenceIdx >= currentSequenceLen) {
            // Sequence finished.
            currentSequence    = nullptr;
            currentSequenceLen = 0;
            currentSequenceIdx = 0;
        } else {
            const ToneStep& s = currentSequence[currentSequenceIdx];
            if (s.freq > 0.0f) {
                playTone(s.freq, s.durationMs);
            } else {
                // Rest: ensure any in-flight tone is silenced for the rest interval.
                stopTone();
            }
            nextStepAtMs = millis() + s.durationMs + s.gapAfterMs;
            currentSequenceIdx++;
        }
    }
}

void AudioManager::setVolume(float volumeLevel) {
    float vol = constrain(volumeLevel, 0.0f, 1.0f);
    volume.setVolume(vol);
}

void AudioManager::playTone(float frequency, int durationMs) {
    currentFrequency = frequency;
    generator.setFrequency(currentFrequency);

    if (!isPlaying) {
        generator.begin();
        isPlaying = true;
    }

    stopAtMillis = (durationMs > 0) ? (millis() + durationMs) : 0;
}

void AudioManager::stopTone() {
    if (isPlaying) {
        generator.end();
        isPlaying = false;
        // volume.setVolume(0.0f); // optional instant silence
    }
}

void AudioManager::playSequence(const ToneStep* steps, int count) {
    if (steps == nullptr || count <= 0) {
        stopSequence();
        return;
    }
    currentSequence    = steps;
    currentSequenceLen = count;
    currentSequenceIdx = 0;
    nextStepAtMs       = millis(); // play first step immediately on next loop()
}

void AudioManager::stopSequence() {
    currentSequence    = nullptr;
    currentSequenceLen = 0;
    currentSequenceIdx = 0;
    stopTone();
}
void AudioManager::releaseI2S() {
    stopTone();
    i2s.end();
}

void AudioManager::reclaimI2S() {
    auto cfg = i2s.defaultConfig(TX_MODE);
    cfg.port_no         = 0;
    cfg.i2s_format      = I2S_LSB_FORMAT;
    cfg.pin_ws          = 27;
    cfg.pin_bck         = 26;
    cfg.pin_data        = 14;
    cfg.channels        = 2;
    cfg.bits_per_sample = 16;
    cfg.buffer_count    = 12;
    cfg.buffer_size     = 256;
    i2s.begin(cfg);
}

void AudioManager::enableMic(bool on) {
    micRunRequested = on; // task will do the rest
}

float AudioManager::getMicVolumeDb() const {
    float lin = getMicVolumeLinear();      // 0..1
    if (lin < 1e-6f) lin = 1e-6f;          // avoid log(0)
    return 20.0f * log10f(lin);            // dBFS (negative up to 0)
}

void AudioManager::micTaskThunk(void *arg) {
    reinterpret_cast<AudioManager*>(arg)->micTaskLoop();
}

void AudioManager::micTaskLoop() {
    static uint8_t buf[512];
    const TickType_t idleDelay = pdMS_TO_TICKS(5);
    uint32_t lastLevelMs = millis();

    for (;;) {
        // State transitions
        if (micRunRequested && !micRunning) {
            i2sIn.begin(micCfg);
            i2sIn.setTimeout(0);

            micMeter.begin(AudioInfo(micCfg.sample_rate, 1, 16));
            micMeter.setTimeout(0);

            micRunning = true;
        } else if (!micRunRequested && micRunning) {
            micMeter.end();
            i2sIn.end();
            micRunning = false;
            micVolumeAtomic = 0.0f;
        }

        if (!micRunning) {
            vTaskDelay(idleDelay);
            continue;
        }

        // Non-blocking pump
        int avail = i2sIn.available();
        if (avail > 0) {
            size_t toRead = (size_t)avail;
            if (toRead > sizeof(buf)) toRead = sizeof(buf);
            int n = i2sIn.readBytes(buf, toRead); // timeout(0) => non-blocking
            if (n > 0) {
                micMeter.write(buf, (size_t)n);
            }
        } else {
            vTaskDelay(1);
        }

        // Publish level ~every 20ms
        uint32_t now = millis();
        if (now - lastLevelMs >= 20) {
            float raw = micMeter.volume();        // ~0..32767
            micVolumeAtomic = (raw <= 0.0f) ? 0.0f : (raw / 32768.0f); // 0..1
            // optional: micMeter.clear();
            lastLevelMs = now;
        }
    }
}
