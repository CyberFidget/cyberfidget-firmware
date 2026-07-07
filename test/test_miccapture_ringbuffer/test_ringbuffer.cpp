// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

// test/test_miccapture_ringbuffer/test_ringbuffer.cpp
//
// Pure-logic tests for RecRingBuffer (SPSC byte ring used by the shared
// lib/MicCapture pipeline). No HAL, no FreeRTOS — indices, wraparound,
// and drop-newest overflow accounting.

#include <unity.h>
#include <cstdint>
#include <cstring>

#include "RecRingBuffer.h"

static uint8_t storage[1024];
static RecRingBuffer ring;

void setUp(void) {
    std::memset(storage, 0, sizeof(storage));
    TEST_ASSERT_TRUE(ring.init(storage, sizeof(storage)));
}

void tearDown(void) {}

void test_init_rejects_non_power_of_two_capacity(void) {
    RecRingBuffer r;
    TEST_ASSERT_FALSE(r.init(storage, 1000));
    TEST_ASSERT_FALSE(r.init(storage, 0));
    TEST_ASSERT_FALSE(r.init(nullptr, 1024));
}

void test_init_accepts_power_of_two_and_reports_capacity(void) {
    RecRingBuffer r;
    TEST_ASSERT_TRUE(r.init(storage, 512));
    TEST_ASSERT_EQUAL_UINT32(512, r.capacity());
    TEST_ASSERT_EQUAL_UINT32(0, r.available());
    TEST_ASSERT_EQUAL_UINT32(512, r.freeSpace());
}

void test_pop_on_empty_returns_zero(void) {
    uint8_t out[16];
    TEST_ASSERT_EQUAL_UINT32(0, ring.pop(out, sizeof(out)));
}

void test_push_pop_roundtrip_bytes_match(void) {
    uint8_t in[100], out[100];
    for (int i = 0; i < 100; ++i) in[i] = (uint8_t)(i * 7 + 3);
    TEST_ASSERT_EQUAL_UINT32(100, ring.push(in, 100));
    TEST_ASSERT_EQUAL_UINT32(100, ring.available());
    TEST_ASSERT_EQUAL_UINT32(100, ring.pop(out, sizeof(out)));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(in, out, 100);
    TEST_ASSERT_EQUAL_UINT32(0, ring.available());
}

void test_wraparound_preserves_data_integrity(void) {
    uint8_t in[300], out[300];
    uint8_t seq = 0;
    // 10 rounds of 300 bytes through a 1024-byte ring forces several wraps.
    for (int round = 0; round < 10; ++round) {
        for (int i = 0; i < 300; ++i) in[i] = seq++;
        TEST_ASSERT_EQUAL_UINT32(300, ring.push(in, 300));
        TEST_ASSERT_EQUAL_UINT32(300, ring.pop(out, 300));
        TEST_ASSERT_EQUAL_UINT8_ARRAY(in, out, 300);
    }
}

void test_fill_to_exact_capacity(void) {
    uint8_t in[1024];
    for (int i = 0; i < 1024; ++i) in[i] = (uint8_t)i;
    TEST_ASSERT_EQUAL_UINT32(1024, ring.push(in, 1024));
    TEST_ASSERT_EQUAL_UINT32(1024, ring.available());
    TEST_ASSERT_EQUAL_UINT32(0, ring.freeSpace());
    TEST_ASSERT_EQUAL_UINT32(0, ring.droppedBytes());
}

void test_overflow_drops_newest_and_counts(void) {
    uint8_t in[1100], out[1024];
    for (int i = 0; i < 1100; ++i) in[i] = (uint8_t)(i & 0xFF);
    // 1100 into a 1024 ring: accepts 1024, drops the newest 76.
    TEST_ASSERT_EQUAL_UINT32(1024, ring.push(in, 1100));
    TEST_ASSERT_EQUAL_UINT32(76, ring.droppedBytes());
    // Oldest data is intact — what we read is the FIRST 1024 bytes pushed.
    TEST_ASSERT_EQUAL_UINT32(1024, ring.pop(out, 1024));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(in, out, 1024);
}

void test_partial_push_accepts_exactly_free_bytes(void) {
    uint8_t in[600], out[600];
    for (int i = 0; i < 600; ++i) in[i] = (uint8_t)(i & 0xFF);
    TEST_ASSERT_EQUAL_UINT32(600, ring.push(in, 600));
    TEST_ASSERT_EQUAL_UINT32(424, ring.freeSpace());
    // Second 600-byte push only fits 424.
    TEST_ASSERT_EQUAL_UINT32(424, ring.push(in, 600));
    TEST_ASSERT_EQUAL_UINT32(176, ring.droppedBytes());
    TEST_ASSERT_EQUAL_UINT32(1024, ring.available());
    TEST_ASSERT_EQUAL_UINT32(600, ring.pop(out, 600));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(in, out, 600);
}

void test_interleaved_push_pop_preserves_fifo_order(void) {
    // Heavy interleave with varying chunk sizes; payload is a global byte
    // sequence so any reorder/loss/duplication shows up immediately.
    uint8_t in[97], out[256];
    uint32_t lcg = 12345;
    uint8_t pushSeq = 0, popSeq = 0;
    uint64_t pushed = 0, popped = 0;
    for (int step = 0; step < 5000; ++step) {
        lcg = lcg * 1664525u + 1013904223u;
        uint32_t pushLen = 1 + (lcg >> 16) % 97;
        if (pushLen > ring.freeSpace()) pushLen = ring.freeSpace();
        for (uint32_t i = 0; i < pushLen; ++i) in[i] = pushSeq++;
        TEST_ASSERT_EQUAL_UINT32(pushLen, ring.push(in, pushLen));
        pushed += pushLen;

        lcg = lcg * 1664525u + 1013904223u;
        uint32_t popLen = (lcg >> 16) % 128;
        uint32_t got = ring.pop(out, popLen);
        popped += got;
        for (uint32_t i = 0; i < got; ++i) {
            if (out[i] != popSeq++) {
                TEST_FAIL_MESSAGE("FIFO order violated");
            }
        }
    }
    TEST_ASSERT_EQUAL_UINT32(0, ring.droppedBytes());
    TEST_ASSERT_EQUAL_UINT32((uint32_t)(pushed - popped), ring.available());
}

void test_available_plus_free_equals_capacity_invariant(void) {
    uint8_t in[333];
    std::memset(in, 0xAB, sizeof(in));
    uint8_t out[100];
    for (int step = 0; step < 50; ++step) {
        ring.push(in, 333);
        ring.pop(out, 100);
        TEST_ASSERT_EQUAL_UINT32(ring.capacity(),
                                 ring.available() + ring.freeSpace());
    }
}

void test_dropped_counter_accumulates_and_clears(void) {
    uint8_t in[1024];
    std::memset(in, 0x55, sizeof(in));
    ring.push(in, 1024);          // full
    ring.push(in, 100);           // +100 dropped
    ring.push(in, 50);            // +50 dropped
    TEST_ASSERT_EQUAL_UINT32(150, ring.droppedBytes());
    ring.clearDropped();
    TEST_ASSERT_EQUAL_UINT32(0, ring.droppedBytes());
    // Data unaffected by clearing the counter.
    TEST_ASSERT_EQUAL_UINT32(1024, ring.available());
}

void test_reset_empties_buffer(void) {
    uint8_t in[200];
    std::memset(in, 0x77, sizeof(in));
    ring.push(in, 200);
    ring.reset();
    TEST_ASSERT_EQUAL_UINT32(0, ring.available());
    TEST_ASSERT_EQUAL_UINT32(ring.capacity(), ring.freeSpace());
    TEST_ASSERT_EQUAL_UINT32(0, ring.droppedBytes());
}

int main(int /*argc*/, char** /*argv*/) {
    UNITY_BEGIN();
    RUN_TEST(test_init_rejects_non_power_of_two_capacity);
    RUN_TEST(test_init_accepts_power_of_two_and_reports_capacity);
    RUN_TEST(test_pop_on_empty_returns_zero);
    RUN_TEST(test_push_pop_roundtrip_bytes_match);
    RUN_TEST(test_wraparound_preserves_data_integrity);
    RUN_TEST(test_fill_to_exact_capacity);
    RUN_TEST(test_overflow_drops_newest_and_counts);
    RUN_TEST(test_partial_push_accepts_exactly_free_bytes);
    RUN_TEST(test_interleaved_push_pop_preserves_fifo_order);
    RUN_TEST(test_available_plus_free_equals_capacity_invariant);
    RUN_TEST(test_dropped_counter_accumulates_and_clears);
    RUN_TEST(test_reset_empties_buffer);
    return UNITY_END();
}
