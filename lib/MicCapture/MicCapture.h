// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

// lib/MicCapture/MicCapture.h
//
// Shared microphone capture service: I2S port 1 (ICS-43434) -> capture task
// on core 0 -> SPSC byte ring. Extracted from VoiceRecorderApp once a second
// consumer existed (WebPortalApp's live caption stream). The pipeline is
// byte-for-byte the recorder's: same pin map, DMA sizing, digital gain, VU
// peak publishing, safe between-reads re-clock, and drop-newest ring.
//
// Ownership model: one owner at a time, taken with acquire() and returned
// with release(). The second acquire() fails with a reason string instead of
// corrupting the running session — live streaming and SD recording are
// mutually exclusive by construction (single ring, single consumer).
//
// Threading: acquire/release/startStreaming/stopStreaming/requestSampleRate
// are main-loop only. The capture task is the sole I2S reader and the only
// code that re-opens the port mid-session.

#ifndef MIC_CAPTURE_H
#define MIC_CAPTURE_H

#include <Arduino.h>
#include <atomic>

#include "AudioTools/CoreAudio/AudioI2S/I2SStream.h"

#include "RecRingBuffer.h"

class MicCapture {
public:
    static MicCapture& instance();

    // Digital gain applied (saturating) to every sample in the capture task.
    // The ICS-43434 is quiet by design (94dB SPL = -26dBFS); x8 (+18dB)
    // brings speech up to a comfortable level. Each +1 shift adds 6dB but
    // eats 6dB of clip headroom.
    static constexpr int kGainShift = 3;

    // Take exclusive ownership of the mic: allocate the ring (PSRAM first,
    // 64KB internal fallback), open I2S port 1 at sampleRate, start the
    // capture task. Returns false with *errOut set to a short display-safe
    // reason ("IN USE", "MEMORY ERROR", "MIC ERROR", "TASK ERROR") on
    // failure. Also parks AudioManager's mic metering task, which shares
    // the port.
    //
    // dmaBufCount sizes the I2S DMA cushion (x512B buffers) — and with it
    // the acquire's INTERNAL-ram footprint, the scarce resource when WiFi
    // AP+STA is up. The cushion only has to cover capture-task scheduling
    // jitter (<=5ms poll), so callers in heap-tight contexts (the portal's
    // live stream) pass a smaller count; the recorder keeps its original 8.
    bool acquire(const char* ownerTag, uint32_t sampleRate,
                 const char** errOut = nullptr, uint8_t dmaBufCount = 8);
    void release();

    bool acquired() const { return owner != nullptr; }
    const char* ownerTag() const { return owner; }

    // Re-clock the port between reads (capture-task-side, never racing a
    // read). Only honored while not streaming; reconfigPending() lets the
    // caller wait for the swap before starting a time-sensitive session.
    void requestSampleRate(uint32_t rate);
    bool reconfigPending() const {
        return reconfigRequested.load(std::memory_order_acquire);
    }
    uint32_t sampleRate() const {
        return pendingSampleRate.load(std::memory_order_relaxed);
    }

    // Gate ring pushes on/off. skipBytes discards the first N captured bytes
    // of the session (the recorder uses ~100ms to drop the record-press
    // click; a live stream passes 0). Resets the ring and its drop counter.
    void startStreaming(uint32_t skipBytes);
    void stopStreaming();
    bool streaming() const {
        return streamingActive.load(std::memory_order_acquire);
    }

    RecRingBuffer& ring() { return ringBuf; }

    // Post-gain peak magnitude since the last call (drives VU bars).
    uint16_t vuPeakExchange() {
        return vuPeak.exchange(0, std::memory_order_relaxed);
    }
    void clearVuPeak() { vuPeak.store(0, std::memory_order_relaxed); }

private:
    MicCapture() = default;
    MicCapture(const MicCapture&) = delete;
    MicCapture& operator=(const MicCapture&) = delete;

    static void captureTaskThunk(void* arg);
    void captureTaskLoop();

    const char* owner = nullptr;

    RecRingBuffer ringBuf;
    uint8_t* ringStorage = nullptr;
    uint32_t ringCapacity = 0;

    audio_tools::I2SStream i2sIn;
    audio_tools::I2SConfig micCfg;
    bool i2sOpened = false;

    TaskHandle_t captureTaskHandle = nullptr;
    std::atomic<bool> exitRequested{false};
    std::atomic<bool> streamingActive{false};
    std::atomic<bool> captureTaskExited{true};
    std::atomic<bool> reconfigRequested{false};
    std::atomic<uint32_t> pendingSampleRate{16000};
    std::atomic<uint16_t> vuPeak{0};
    std::atomic<uint32_t> skipBytesRemaining{0};

    uint8_t captureBuf[1024];   // capture task only
};

#endif // MIC_CAPTURE_H
