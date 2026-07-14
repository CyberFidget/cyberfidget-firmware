// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

// lib/MicCapture/MicCapture.cpp — see MicCapture.h for the ownership and
// threading model. The capture loop is a verbatim extraction of the voice
// recorder's: read I2S, apply gain + track the post-gain peak, push to the
// ring only while a streaming session is active.

#include "MicCapture.h"

#include <esp_heap_caps.h>

#include "AudioManager.h"
#include "HAL.h"
#include "globals.h"

static const char* TAG_MIC = "MicCapture";

MicCapture& MicCapture::instance() {
    static MicCapture singleton;
    return singleton;
}

bool MicCapture::acquire(const char* ownerTag, uint32_t sampleRate,
                         const char** errOut, uint8_t dmaBufCount) {
    const char* err = nullptr;
    if (owner != nullptr) {
        if (errOut) *errOut = "IN USE";
        ESP_LOGE(TAG_MIC, "acquire(%s) refused: held by %s", ownerTag, owner);
        return false;
    }

    // AudioManager's metering task only holds I2S port 1 while its run flag
    // is set; clear it and give the task (<=5ms poll) time to close the port.
    HAL::audioManager().enableMic(false);
    delay(15);

    exitRequested.store(false, std::memory_order_relaxed);
    streamingActive.store(false, std::memory_order_relaxed);
    reconfigRequested.store(false, std::memory_order_relaxed);
    vuPeak.store(0, std::memory_order_relaxed);
    pendingSampleRate.store(sampleRate, std::memory_order_relaxed);

    if (ringStorage == nullptr) {
        ringCapacity = REC_RING_SIZE_BYTES;
        ringStorage  = (uint8_t*)ps_malloc(ringCapacity);
        if (ringStorage == nullptr) {
            // No PSRAM: a 64KB internal-RAM ring still buffers 2s of audio.
            ringCapacity = 65536;
            ringStorage  = (uint8_t*)heap_caps_malloc(ringCapacity, MALLOC_CAP_8BIT);
        }
    }
    if (ringStorage == nullptr || !ringBuf.init(ringStorage, ringCapacity)) {
        ESP_LOGE(TAG_MIC, "ring alloc failed (%u bytes)", (unsigned)ringCapacity);
        err = "MEMORY ERROR";
        goto fail;
    }

    // Mic input — AudioManager.cpp's pin map, at the session sample rate.
    micCfg = i2sIn.defaultConfig(audio_tools::RX_MODE);
    micCfg.port_no         = 1;
    micCfg.i2s_format      = audio_tools::I2S_STD_FORMAT;
    micCfg.sample_rate     = sampleRate;
    micCfg.bits_per_sample = 16;
    micCfg.channels        = 1;
    micCfg.pin_ws          = 25;
    micCfg.pin_bck         = 32;
    micCfg.pin_data_rx     = 33;
    micCfg.pin_data        = -1;
    micCfg.is_master       = true;
    // x512B DMA buffers: at the recorder's default 8 that's ~128ms cushion
    // at 16kHz / ~43ms at 48kHz; even the live path's 4 stays well past
    // the capture task's 5ms poll.
    micCfg.buffer_count    = (dmaBufCount >= 2) ? dmaBufCount : 2;
    micCfg.buffer_size     = 512;
    i2sOpened = i2sIn.begin(micCfg);
    if (!i2sOpened) {
        ESP_LOGE(TAG_MIC, "I2S port 1 open failed");
        err = "MIC ERROR";
        goto fail;
    }
    i2sIn.setTimeout(0);

    captureTaskExited.store(false, std::memory_order_relaxed);
    if (xTaskCreatePinnedToCore(
            &MicCapture::captureTaskThunk,
            "micPump",
            4096,
            this,
            2,              // above the prio-1 background pumps; mic reads are time-sensitive
            &captureTaskHandle,
            0               // core 0, same as AudioManager's micPump
        ) != pdPASS) {
        captureTaskExited.store(true, std::memory_order_relaxed);
        captureTaskHandle = nullptr;
        i2sIn.end();
        i2sOpened = false;
        err = "TASK ERROR";
        goto fail;
    }

    owner = ownerTag;
    return true;

fail:
    if (ringStorage != nullptr) {
        free(ringStorage);
        ringStorage = nullptr;
        ringCapacity = 0;
    }
    if (errOut) *errOut = err;
    return false;
}

void MicCapture::release() {
    if (owner == nullptr) return;

    streamingActive.store(false, std::memory_order_release);
    exitRequested.store(true, std::memory_order_release);
    unsigned long t0 = millis();
    while (!captureTaskExited.load(std::memory_order_acquire) &&
           (millis() - t0) < 500) {
        vTaskDelay(1);
    }
    captureTaskHandle = nullptr;

    if (i2sOpened) {
        i2sIn.end();   // release I2S port 1 — leave shared hardware clean
        i2sOpened = false;
    }

    if (ringStorage != nullptr) {
        free(ringStorage);
        ringStorage = nullptr;
        ringCapacity = 0;
    }

    owner = nullptr;
}

void MicCapture::requestSampleRate(uint32_t rate) {
    // Hand the new rate to the capture task, which owns the port and does
    // the re-clock between reads — the main loop never touches the port
    // mid-session, so this is the only safe way to re-clock it.
    pendingSampleRate.store(rate, std::memory_order_relaxed);
    reconfigRequested.store(true, std::memory_order_release);
}

void MicCapture::startStreaming(uint32_t skipBytes) {
    ringBuf.reset();
    ringBuf.clearDropped();
    // Must be set before streamingActive: the task only reads it after
    // seeing the gate open.
    skipBytesRemaining.store(skipBytes, std::memory_order_relaxed);
    streamingActive.store(true, std::memory_order_release);
}

void MicCapture::stopStreaming() {
    streamingActive.store(false, std::memory_order_release);
}

// =========================================================================
// Capture task (core 0) — reads I2S, publishes VU peak, feeds the ring
// =========================================================================
void MicCapture::captureTaskThunk(void* arg) {
    static_cast<MicCapture*>(arg)->captureTaskLoop();
}

void MicCapture::captureTaskLoop() {
    const TickType_t idleDelay = pdMS_TO_TICKS(5);
    while (!exitRequested.load(std::memory_order_acquire)) {
        // Sample-rate re-clock — only this task ever calls i2sIn.end()/begin()
        // during a session, so re-opening the port here can't race a read.
        // Guarded on !streamingActive so a live capture is never re-clocked
        // underneath itself.
        if (reconfigRequested.load(std::memory_order_acquire) &&
            !streamingActive.load(std::memory_order_acquire)) {
            uint32_t newRate = pendingSampleRate.load(std::memory_order_relaxed);
            if (newRate != micCfg.sample_rate || !i2sOpened) {
                i2sIn.end();
                micCfg.sample_rate = newRate;
                i2sOpened = i2sIn.begin(micCfg);
                if (i2sOpened) {
                    i2sIn.setTimeout(0);
                } else {
                    ESP_LOGE(TAG_MIC, "I2S re-clock to %u Hz failed",
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
                // post-gain peak so VU displays reflect what actually lands
                // in the ring.
                int16_t* samples = (int16_t*)captureBuf;
                int count = n / 2;
                uint16_t peak = vuPeak.load(std::memory_order_relaxed);
                for (int i = 0; i < count; ++i) {
                    int32_t v = (int32_t)samples[i] << kGainShift;
                    if (v > 32767) v = 32767;
                    else if (v < -32768) v = -32768;
                    samples[i] = (int16_t)v;
                    uint16_t mag = (uint16_t)(v < 0 ? -v : v);
                    if (mag > peak) peak = mag;
                }
                vuPeak.store(peak, std::memory_order_relaxed);
                if (streamingActive.load(std::memory_order_acquire)) {
                    // Discard the session's first skipBytes (e.g. the
                    // record-press click) before anything reaches the ring.
                    uint32_t skip = skipBytesRemaining.load(std::memory_order_relaxed);
                    uint32_t offset = 0;
                    if (skip > 0) {
                        offset = (skip < (uint32_t)n) ? skip : (uint32_t)n;
                        skipBytesRemaining.store(skip - offset,
                                                 std::memory_order_relaxed);
                    }
                    if ((uint32_t)n > offset) {
                        ringBuf.push(captureBuf + offset,
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
