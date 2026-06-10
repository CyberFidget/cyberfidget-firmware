// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

// lib/VoiceRecorderApp/RecRingBuffer.h
//
// Single-producer / single-consumer byte ring buffer for the voice recorder
// capture pipeline. Producer is the I2S capture task (core 0); consumer is
// the app's update() on the main loop. Pure C++ (no Arduino/ESP types) so it
// compiles host-side for the native test env (test_voicerecorder).
//
// Capacity must be a power of two. Head/tail are free-running 32-bit
// counters; (head - tail) is wrap-correct in unsigned math, so the full
// capacity is usable (no wasted slot). Overflow policy is drop-newest: a
// push that doesn't fit accepts only what fits and counts the rest in
// droppedBytes(), so already-captured audio is never overwritten.

#ifndef REC_RING_BUFFER_H
#define REC_RING_BUFFER_H

#include <atomic>
#include <cstdint>
#include <cstring>

#ifndef REC_RING_SIZE_BYTES
#define REC_RING_SIZE_BYTES 262144u  // 256KB = 8s headroom at 32KB/s
#endif

class RecRingBuffer {
public:
    // Storage is caller-owned (ps_malloc on target, plain array in tests).
    // capacityPow2 must be a non-zero power of two.
    bool init(uint8_t* storage, uint32_t capacityPow2) {
        if (storage == nullptr || capacityPow2 == 0 ||
            (capacityPow2 & (capacityPow2 - 1)) != 0) {
            return false;
        }
        buf  = storage;
        cap  = capacityPow2;
        mask = capacityPow2 - 1;
        reset();
        return true;
    }

    // Only safe while the producer is idle (recordingActive false).
    void reset() {
        head.store(0, std::memory_order_relaxed);
        tail.store(0, std::memory_order_relaxed);
        dropped.store(0, std::memory_order_relaxed);
    }

    // Producer side. Returns bytes accepted (drop-newest on overflow).
    uint32_t push(const uint8_t* data, uint32_t len) {
        const uint32_t h = head.load(std::memory_order_relaxed);
        const uint32_t t = tail.load(std::memory_order_acquire);
        const uint32_t freeBytes = cap - (h - t);
        const uint32_t accepted  = (len < freeBytes) ? len : freeBytes;
        if (accepted > 0) {
            const uint32_t idx   = h & mask;
            const uint32_t first = (accepted < cap - idx) ? accepted : cap - idx;
            std::memcpy(buf + idx, data, first);
            if (accepted > first) {
                std::memcpy(buf, data + first, accepted - first);
            }
            head.store(h + accepted, std::memory_order_release);
        }
        if (accepted < len) {
            dropped.fetch_add(len - accepted, std::memory_order_relaxed);
        }
        return accepted;
    }

    // Consumer side. Returns bytes copied out.
    uint32_t pop(uint8_t* out, uint32_t maxLen) {
        const uint32_t t = tail.load(std::memory_order_relaxed);
        const uint32_t h = head.load(std::memory_order_acquire);
        const uint32_t avail = h - t;
        const uint32_t n = (maxLen < avail) ? maxLen : avail;
        if (n > 0) {
            const uint32_t idx   = t & mask;
            const uint32_t first = (n < cap - idx) ? n : cap - idx;
            std::memcpy(out, buf + idx, first);
            if (n > first) {
                std::memcpy(out + first, buf, n - first);
            }
            tail.store(t + n, std::memory_order_release);
        }
        return n;
    }

    uint32_t available() const {
        return head.load(std::memory_order_acquire) -
               tail.load(std::memory_order_acquire);
    }

    uint32_t freeSpace() const { return cap - available(); }
    uint32_t capacity()  const { return cap; }

    uint32_t droppedBytes() const {
        return dropped.load(std::memory_order_relaxed);
    }
    void clearDropped() { dropped.store(0, std::memory_order_relaxed); }

private:
    uint8_t* buf  = nullptr;
    uint32_t cap  = 0;
    uint32_t mask = 0;
    std::atomic<uint32_t> head{0};
    std::atomic<uint32_t> tail{0};
    std::atomic<uint32_t> dropped{0};
};

#endif // REC_RING_BUFFER_H
