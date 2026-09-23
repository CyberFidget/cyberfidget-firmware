// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

// Pure update manifest revision parsing and inclusive hardware gate.

#include <unity.h>

#include "BoardInfo.h"
#include "OtaManifest.h"

using namespace OtaManifest;

static BoardInfo::Info board(uint8_t major, uint8_t minor) {
    BoardInfo::Info info = BoardInfo::defaults();
    info.source = BoardInfo::Source::Efuse;
    info.major = major;
    info.minor = minor;
    return info;
}

// Shared boundary examples for the server gate:
// board | min  | max  | result
// 1.2   | 1.2  | 1.2  | Compatible (equal bounds)
// 1.2   | 1.2  | 2.0  | Compatible (lower boundary)
// 2.0   | 1.2  | 2.0  | Compatible (upper boundary)
// 1.1   | 1.2  | 2.0  | Incompatible (below)
// 2.1   | 1.2  | 2.0  | Incompatible (above)
// 1.10  | 1.9  | 1.10 | Compatible (numeric ordering)
// 1.9   | 1.10 | 2.0  | Incompatible (numeric ordering)
void test_inclusive_boundaries_and_structured_order(void) {
    struct Example {
        uint8_t major;
        uint8_t minor;
        const char* min;
        const char* max;
        HwResult result;
    };
    const Example examples[] = {
        {1, 2, "1.2", "1.2", HwResult::Compatible},
        {1, 2, "1.2", "2.0", HwResult::Compatible},
        {2, 0, "1.2", "2.0", HwResult::Compatible},
        {1, 1, "1.2", "2.0", HwResult::Incompatible},
        {2, 1, "1.2", "2.0", HwResult::Incompatible},
        {1, 10, "1.9", "1.10", HwResult::Compatible},
        {1, 9, "1.10", "2.0", HwResult::Incompatible},
    };
    for (const Example& example : examples) {
        TEST_ASSERT_TRUE(hwCompatible(board(example.major, example.minor),
                                      example.min, example.max) == example.result);
    }
}

void test_parse_rev(void) {
    const Rev rev = parseRev("1.10");
    TEST_ASSERT_TRUE(rev.ok);
    TEST_ASSERT_EQUAL_UINT32(1, rev.major);
    TEST_ASSERT_EQUAL_UINT32(10, rev.minor);
    TEST_ASSERT_TRUE(parseRev("0.0").ok);
    TEST_ASSERT_TRUE(parseRev("4294967295.2").ok);
}

void test_reject_malformed_revisions(void) {
    const char* invalid[] = {
        "", "1", ".2", "1.", "1.2.3", "1,2", "1.-2", "+1.2",
        " 1.2", "1.2 ", "1.a", "4294967296.2", "1.4294967296",
    };
    TEST_ASSERT_FALSE(parseRev(nullptr).ok);
    for (const char* value : invalid) {
        TEST_ASSERT_FALSE(parseRev(value).ok);
    }
}

void test_missing_garbage_and_inverted_bounds_are_malformed(void) {
    const BoardInfo::Info info = board(1, 2);
    TEST_ASSERT_TRUE(hwCompatible(info, nullptr, "1.2") == HwResult::Malformed);
    TEST_ASSERT_TRUE(hwCompatible(info, "1.2", nullptr) == HwResult::Malformed);
    TEST_ASSERT_TRUE(hwCompatible(info, "", "1.2") == HwResult::Malformed);
    TEST_ASSERT_TRUE(hwCompatible(info, "1.2", "") == HwResult::Malformed);
    TEST_ASSERT_TRUE(hwCompatible(info, "garbage", "1.2") == HwResult::Malformed);
    TEST_ASSERT_TRUE(hwCompatible(info, "1.2", "garbage") == HwResult::Malformed);
    TEST_ASSERT_TRUE(hwCompatible(info, "1.10", "1.9") == HwResult::Malformed);
    TEST_ASSERT_TRUE(hwCompatible(info, "2.0", "1.9") == HwResult::Malformed);
}

void test_default_and_error_sources_use_reported_1_2(void) {
    BoardInfo::Info info = BoardInfo::defaults();
    TEST_ASSERT_TRUE(hwCompatible(info, "1.2", "1.2") == HwResult::Compatible);
    info.source = BoardInfo::Source::ReadError;
    info.major = 9;
    info.minor = 9;
    TEST_ASSERT_TRUE(hwCompatible(info, "1.2", "1.2") == HwResult::Compatible);
    info.source = BoardInfo::Source::UnknownLayout;
    TEST_ASSERT_TRUE(hwCompatible(info, "1.2", "1.2") == HwResult::Compatible);
    TEST_ASSERT_TRUE(hwCompatible(info, "1.3", "2.0") == HwResult::Incompatible);
}

void test_refusal_copy(void) {
    TEST_ASSERT_EQUAL_STRING("This update isn't made for this Fidget.", kHardwareRefusal);
}

void setUp(void)    {}
void tearDown(void) {}

int main(int /*argc*/, char** /*argv*/) {
    UNITY_BEGIN();
    RUN_TEST(test_inclusive_boundaries_and_structured_order);
    RUN_TEST(test_parse_rev);
    RUN_TEST(test_reject_malformed_revisions);
    RUN_TEST(test_missing_garbage_and_inverted_bounds_are_malformed);
    RUN_TEST(test_default_and_error_sources_use_reported_1_2);
    RUN_TEST(test_refusal_copy);
    return UNITY_END();
}
