// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

// test/test_voicerecorder_naming/test_naming.cpp
//
// Pure-logic tests for RecNaming: sequential file naming with collision
// skip, PCM duration math, and index.csv row formatting (including the
// blank-timestamp-when-the-clock-is-unset rule).

#include <unity.h>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "RecNaming.h"

void setUp(void) {}
void tearDown(void) {}

// --- formatRecPath / formatRecBasename -------------------------------

void test_format_rec_path_basic(void) {
    char out[40];
    TEST_ASSERT_GREATER_THAN(0, RecNaming::formatRecPath(out, sizeof(out), 1));
    TEST_ASSERT_EQUAL_STRING("/recordings/REC_0001.wav", out);
}

void test_format_rec_path_pads_four_digits(void) {
    char out[40];
    RecNaming::formatRecPath(out, sizeof(out), 42);
    TEST_ASSERT_EQUAL_STRING("/recordings/REC_0042.wav", out);
}

void test_format_rec_path_grows_past_9999(void) {
    char out[40];
    RecNaming::formatRecPath(out, sizeof(out), 12345);
    TEST_ASSERT_EQUAL_STRING("/recordings/REC_12345.wav", out);
}

void test_format_rec_path_small_buffer_fails_cleanly(void) {
    char out[10];
    TEST_ASSERT_EQUAL_INT(-1, RecNaming::formatRecPath(out, sizeof(out), 1));
}

void test_format_rec_basename(void) {
    char out[20];
    TEST_ASSERT_GREATER_THAN(0,
        RecNaming::formatRecBasename(out, sizeof(out), 7));
    TEST_ASSERT_EQUAL_STRING("REC_0007.wav", out);
}

// --- nextFreeCounter --------------------------------------------------

// Exists-predicate fixture: paths with counter < existBelow "exist".
struct ExistsFixture {
    uint32_t existBelow;
};

static bool fixtureExists(const char* path, void* ctx) {
    ExistsFixture* f = (ExistsFixture*)ctx;
    // Parse counter back out of "/recordings/REC_NNNN.wav".
    const char* p = std::strstr(path, "REC_");
    TEST_ASSERT_NOT_NULL(p);
    unsigned long counter = std::strtoul(p + 4, nullptr, 10);
    return counter < f->existBelow;
}

void test_next_free_counter_returns_start_when_no_collision(void) {
    ExistsFixture f{0};  // nothing exists
    TEST_ASSERT_EQUAL_UINT32(5,
        RecNaming::nextFreeCounter(5, fixtureExists, &f));
}

void test_next_free_counter_skips_existing_files(void) {
    ExistsFixture f{10};  // REC_0001..REC_0009 exist
    TEST_ASSERT_EQUAL_UINT32(10,
        RecNaming::nextFreeCounter(3, fixtureExists, &f));
}

void test_next_free_counter_exhausts_to_sentinel(void) {
    ExistsFixture f{0xFFFFFFFFu};  // everything exists
    TEST_ASSERT_EQUAL_UINT32(RecNaming::kCounterExhausted,
        RecNaming::nextFreeCounter(1, fixtureExists, &f, /*maxProbes=*/100));
}

// --- durationSecondsFromPcmBytes ---------------------------------------

void test_duration_from_bytes_floor_semantics(void) {
    TEST_ASSERT_EQUAL_UINT32(1, RecNaming::durationSecondsFromPcmBytes(32000, 32000));
    TEST_ASSERT_EQUAL_UINT32(0, RecNaming::durationSecondsFromPcmBytes(31999, 32000));
    TEST_ASSERT_EQUAL_UINT32(0, RecNaming::durationSecondsFromPcmBytes(0, 32000));
    // 5 minutes at 16kHz/16-bit/mono.
    TEST_ASSERT_EQUAL_UINT32(300,
        RecNaming::durationSecondsFromPcmBytes(9600000ull, 32000));
    // Zero byte rate guarded.
    TEST_ASSERT_EQUAL_UINT32(0, RecNaming::durationSecondsFromPcmBytes(1000, 0));
}

// --- index.csv ----------------------------------------------------------

void test_index_header_format(void) {
    TEST_ASSERT_EQUAL_STRING("filename,timestamp,duration_s,bytes\n",
                             RecNaming::indexHeader());
}

void test_index_row_with_valid_local_time(void) {
    struct tm t = {};
    t.tm_year = 2026 - 1900;
    t.tm_mon  = 5;   // June
    t.tm_mday = 9;
    t.tm_hour = 14;
    t.tm_min  = 23;
    t.tm_sec  = 11;
    char out[80];
    TEST_ASSERT_GREATER_THAN(0,
        RecNaming::formatIndexRow(out, sizeof(out), "REC_0042.wav", &t,
                                  123, 3936044ull));
    TEST_ASSERT_EQUAL_STRING("REC_0042.wav,2026-06-09T14:23:11,123,3936044\n",
                             out);
}

void test_index_row_blank_timestamp_when_clock_unset(void) {
    char out[80];
    TEST_ASSERT_GREATER_THAN(0,
        RecNaming::formatIndexRow(out, sizeof(out), "REC_0001.wav", nullptr,
                                  7, 224000ull));
    TEST_ASSERT_EQUAL_STRING("REC_0001.wav,,7,224000\n", out);
}

void test_index_row_small_buffer_fails_cleanly(void) {
    char out[10];
    TEST_ASSERT_EQUAL_INT(-1,
        RecNaming::formatIndexRow(out, sizeof(out), "REC_0001.wav", nullptr,
                                  7, 224000ull));
}

int main(int /*argc*/, char** /*argv*/) {
    UNITY_BEGIN();
    RUN_TEST(test_format_rec_path_basic);
    RUN_TEST(test_format_rec_path_pads_four_digits);
    RUN_TEST(test_format_rec_path_grows_past_9999);
    RUN_TEST(test_format_rec_path_small_buffer_fails_cleanly);
    RUN_TEST(test_format_rec_basename);
    RUN_TEST(test_next_free_counter_returns_start_when_no_collision);
    RUN_TEST(test_next_free_counter_skips_existing_files);
    RUN_TEST(test_next_free_counter_exhausts_to_sentinel);
    RUN_TEST(test_duration_from_bytes_floor_semantics);
    RUN_TEST(test_index_header_format);
    RUN_TEST(test_index_row_with_valid_local_time);
    RUN_TEST(test_index_row_blank_timestamp_when_clock_unset);
    RUN_TEST(test_index_row_small_buffer_fails_cleanly);
    return UNITY_END();
}
