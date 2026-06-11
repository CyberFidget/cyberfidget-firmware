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

// High quality = 48kHz/16-bit/mono = 96000 B/s. index.csv duration must read
// from the active byte rate, so the same math has to hold at the High rate.
void test_duration_from_bytes_high_quality(void) {
    TEST_ASSERT_EQUAL_UINT32(1, RecNaming::durationSecondsFromPcmBytes(96000, 96000));
    TEST_ASSERT_EQUAL_UINT32(0, RecNaming::durationSecondsFromPcmBytes(95999, 96000));
    // 5 minutes at High (96000 * 300).
    TEST_ASSERT_EQUAL_UINT32(300,
        RecNaming::durationSecondsFromPcmBytes(28800000ull, 96000));
    // The same byte count is a 3x shorter clip when recorded at High: a
    // fixed file size must not be reported with a hardcoded Standard rate.
    TEST_ASSERT_EQUAL_UINT32(30, RecNaming::durationSecondsFromPcmBytes(960000ull, 32000));
    TEST_ASSERT_EQUAL_UINT32(10, RecNaming::durationSecondsFromPcmBytes(960000ull, 96000));
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

// --- parseIndexRow (read-back for the on-device recordings list) --------

void test_parse_index_row_valid(void) {
    char name[16], ts[20];
    uint32_t dur;
    uint64_t bytes;
    TEST_ASSERT_TRUE(RecNaming::parseIndexRow(
        "REC_0042.wav,2026-06-09T14:23:11,123,3936044\n",
        name, sizeof(name), ts, sizeof(ts), &dur, &bytes));
    TEST_ASSERT_EQUAL_STRING("REC_0042.wav", name);
    TEST_ASSERT_EQUAL_STRING("2026-06-09T14:23:11", ts);
    TEST_ASSERT_EQUAL_UINT32(123, dur);
    TEST_ASSERT_EQUAL_UINT64(3936044ull, bytes);
}

void test_parse_index_row_blank_timestamp(void) {
    char name[16], ts[20];
    uint32_t dur;
    uint64_t bytes;
    TEST_ASSERT_TRUE(RecNaming::parseIndexRow(
        "REC_0001.wav,,7,224000\n",
        name, sizeof(name), ts, sizeof(ts), &dur, &bytes));
    TEST_ASSERT_EQUAL_STRING("REC_0001.wav", name);
    TEST_ASSERT_EQUAL_STRING("", ts);
    TEST_ASSERT_EQUAL_UINT32(7, dur);
    TEST_ASSERT_EQUAL_UINT64(224000ull, bytes);
}

// The final row of a file that didn't end in '\n' must still parse.
void test_parse_index_row_no_trailing_newline(void) {
    char name[16], ts[20];
    uint32_t dur;
    uint64_t bytes;
    TEST_ASSERT_TRUE(RecNaming::parseIndexRow(
        "REC_0009.wav,,1,32000",
        name, sizeof(name), ts, sizeof(ts), &dur, &bytes));
    TEST_ASSERT_EQUAL_STRING("REC_0009.wav", name);
    TEST_ASSERT_EQUAL_UINT64(32000ull, bytes);
}

void test_parse_index_row_rejects_header(void) {
    char name[16], ts[20];
    uint32_t dur;
    uint64_t bytes;
    TEST_ASSERT_FALSE(RecNaming::parseIndexRow(
        RecNaming::indexHeader(),
        name, sizeof(name), ts, sizeof(ts), &dur, &bytes));
}

void test_parse_index_row_rejects_short_row(void) {
    char name[16], ts[20];
    uint32_t dur;
    uint64_t bytes;
    TEST_ASSERT_FALSE(RecNaming::parseIndexRow(
        "REC_0001.wav,,7\n",   // only three fields
        name, sizeof(name), ts, sizeof(ts), &dur, &bytes));
}

void test_parse_index_row_rejects_nonnumeric(void) {
    char name[16], ts[20];
    uint32_t dur;
    uint64_t bytes;
    TEST_ASSERT_FALSE(RecNaming::parseIndexRow(
        "REC_0001.wav,,x,224000\n",   // non-numeric duration
        name, sizeof(name), ts, sizeof(ts), &dur, &bytes));
}

// A name that overflows the caller's buffer is rejected, not truncated.
void test_parse_index_row_rejects_overflow_name(void) {
    char name[8], ts[20];   // too small for "REC_0042.wav"
    uint32_t dur;
    uint64_t bytes;
    TEST_ASSERT_FALSE(RecNaming::parseIndexRow(
        "REC_0042.wav,,1,32000\n",
        name, sizeof(name), ts, sizeof(ts), &dur, &bytes));
}

// --- indexRowMatchesFilename / delete-row behavior ----------------------

void test_index_row_matches_exact(void) {
    TEST_ASSERT_TRUE(RecNaming::indexRowMatchesFilename(
        "REC_0042.wav,2026-06-09T14:23:11,123,3936044\n", "REC_0042.wav"));
}

void test_index_row_matches_prefix_collision_safe(void) {
    // Deleting REC_0001.wav must NOT match the row for REC_0010.wav, in either
    // direction, and a target that is a prefix of the field must not match.
    TEST_ASSERT_FALSE(RecNaming::indexRowMatchesFilename(
        "REC_0010.wav,,1,32000\n", "REC_0001.wav"));
    TEST_ASSERT_FALSE(RecNaming::indexRowMatchesFilename(
        "REC_0001.wav,,1,32000\n", "REC_0010.wav"));
    TEST_ASSERT_FALSE(RecNaming::indexRowMatchesFilename(
        "REC_0001.wav,,1,32000\n", "REC_0001"));
}

void test_index_row_matches_rejects_header(void) {
    TEST_ASSERT_FALSE(RecNaming::indexRowMatchesFilename(
        RecNaming::indexHeader(), "REC_0001.wav"));
}

// A stray line with no comma still compares the whole token (up to newline).
void test_index_row_matches_no_comma_line(void) {
    TEST_ASSERT_TRUE(RecNaming::indexRowMatchesFilename(
        "REC_0007.wav\n", "REC_0007.wav"));
}

// Mirrors the app's index.csv rewrite-on-delete: keep the header and every row
// whose filename != the deleted one; drop exactly the matching row.
void test_delete_row_filters_only_matching(void) {
    const char* lines[] = {
        "filename,timestamp,duration_s,bytes",
        "REC_0001.wav,,1,32000",
        "REC_0002.wav,2026-06-09T14:23:11,123,3936044",
        "REC_0010.wav,,5,160000",
    };
    const int n = sizeof(lines) / sizeof(lines[0]);
    const char* target = "REC_0002.wav";

    int kept = 0;
    bool keptHeader = false, kept1 = false, kept10 = false, keptTarget = false;
    for (int i = 0; i < n; ++i) {
        if (RecNaming::indexRowMatchesFilename(lines[i], target)) continue;
        kept++;
        if (i == 0) keptHeader = true;
        if (RecNaming::indexRowMatchesFilename(lines[i], "REC_0001.wav")) kept1 = true;
        if (RecNaming::indexRowMatchesFilename(lines[i], "REC_0010.wav")) kept10 = true;
        if (RecNaming::indexRowMatchesFilename(lines[i], target)) keptTarget = true;
    }
    TEST_ASSERT_EQUAL_INT(3, kept);   // header + the two non-deleted rows
    TEST_ASSERT_TRUE(keptHeader);
    TEST_ASSERT_TRUE(kept1);
    TEST_ASSERT_TRUE(kept10);
    TEST_ASSERT_FALSE(keptTarget);
}

void test_delete_row_absent_is_noop(void) {
    const char* lines[] = {
        "filename,timestamp,duration_s,bytes",
        "REC_0001.wav,,1,32000",
    };
    const int n = sizeof(lines) / sizeof(lines[0]);
    int kept = 0;
    for (int i = 0; i < n; ++i) {
        if (RecNaming::indexRowMatchesFilename(lines[i], "REC_9999.wav")) continue;
        kept++;
    }
    TEST_ASSERT_EQUAL_INT(2, kept);   // nothing matched, nothing removed
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
    RUN_TEST(test_duration_from_bytes_high_quality);
    RUN_TEST(test_index_header_format);
    RUN_TEST(test_index_row_with_valid_local_time);
    RUN_TEST(test_index_row_blank_timestamp_when_clock_unset);
    RUN_TEST(test_index_row_small_buffer_fails_cleanly);
    RUN_TEST(test_parse_index_row_valid);
    RUN_TEST(test_parse_index_row_blank_timestamp);
    RUN_TEST(test_parse_index_row_no_trailing_newline);
    RUN_TEST(test_parse_index_row_rejects_header);
    RUN_TEST(test_parse_index_row_rejects_short_row);
    RUN_TEST(test_parse_index_row_rejects_nonnumeric);
    RUN_TEST(test_parse_index_row_rejects_overflow_name);
    RUN_TEST(test_index_row_matches_exact);
    RUN_TEST(test_index_row_matches_prefix_collision_safe);
    RUN_TEST(test_index_row_matches_rejects_header);
    RUN_TEST(test_index_row_matches_no_comma_line);
    RUN_TEST(test_delete_row_filters_only_matching);
    RUN_TEST(test_delete_row_absent_is_noop);
    return UNITY_END();
}
