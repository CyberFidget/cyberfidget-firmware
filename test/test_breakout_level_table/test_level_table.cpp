// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

// lib/BreakoutGame/test/test_level_table.cpp
//
// Tests for BreakoutMath::isLayoutRowValid — the validator used in debug
// builds to catch typos in level-layout strings before flashing.

#include <unity.h>
#include "BreakoutMath.h"

void test_valid_rows_accepted(void) {
    TEST_ASSERT_TRUE(BreakoutMath::isLayoutRowValid("BBBBBBBB", 8));
    TEST_ASSERT_TRUE(BreakoutMath::isLayoutRowValid("BB.UU.BB", 8));
    TEST_ASSERT_TRUE(BreakoutMath::isLayoutRowValid("CUCUCUCU", 8));
    TEST_ASSERT_TRUE(BreakoutMath::isLayoutRowValid("........", 8));
    // Spike cell chars added in T-012 round 3.
    TEST_ASSERT_TRUE(BreakoutMath::isLayoutRowValid("BSBSBSBS", 8));    // one-shot spike
    TEST_ASSERT_TRUE(BreakoutMath::isLayoutRowValid("PUPUPUPU", 8));    // persistent spike + unbreakable
    TEST_ASSERT_TRUE(BreakoutMath::isLayoutRowValid("BSPCUSPB", 8));    // all cell-type chars mixed
}

void test_short_row_rejected(void) {
    TEST_ASSERT_FALSE(BreakoutMath::isLayoutRowValid("BBBBBBB", 8));   // 7 chars
}

void test_long_row_rejected(void) {
    TEST_ASSERT_FALSE(BreakoutMath::isLayoutRowValid("BBBBBBBBB", 8)); // 9 chars
}

void test_invalid_char_rejected(void) {
    TEST_ASSERT_FALSE(BreakoutMath::isLayoutRowValid("BBBBxBBB", 8));  // 'x' not allowed
    TEST_ASSERT_FALSE(BreakoutMath::isLayoutRowValid("BBBB BBB", 8));  // space not allowed
    TEST_ASSERT_FALSE(BreakoutMath::isLayoutRowValid("bbbbbbbb", 8));  // lowercase not allowed
}

void test_null_row_rejected(void) {
    TEST_ASSERT_FALSE(BreakoutMath::isLayoutRowValid(nullptr, 8));
}

void setUp(void)    {}
void tearDown(void) {}

int main(int /*argc*/, char** /*argv*/) {
    UNITY_BEGIN();
    RUN_TEST(test_valid_rows_accepted);
    RUN_TEST(test_short_row_rejected);
    RUN_TEST(test_long_row_rejected);
    RUN_TEST(test_invalid_char_rejected);
    RUN_TEST(test_null_row_rejected);
    return UNITY_END();
}
