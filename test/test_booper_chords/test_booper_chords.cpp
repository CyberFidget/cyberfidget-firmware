// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#include <unity.h>
#include <math.h>
#include <string.h>
#include "BooperChords.h"

void setUp() {}
void tearDown() {}

static void test_every_set_has_name_and_four_usable_notes() {
    TEST_ASSERT_EQUAL_INT(5, kBooperChordSetCount);
    for (int s = 0; s < kBooperChordSetCount; ++s) {
        TEST_ASSERT_NOT_NULL(kBooperChordSets[s].name);
        TEST_ASSERT_TRUE(strlen(kBooperChordSets[s].name) > 0);
        for (int n = 0; n < 4; ++n) {
            TEST_ASSERT_TRUE(kBooperChordSets[s].freq[n] >= 260.0f);
            TEST_ASSERT_TRUE(kBooperChordSets[s].freq[n] <= 1000.0f);
        }
    }
}

static void test_set_frequencies() {
    TEST_ASSERT_EQUAL_STRING("Major", kBooperChordSets[0].name);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 329.63f, kBooperChordSets[0].freq[1]);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 311.13f, kBooperChordSets[1].freq[1]); // Eb4
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 783.99f, kBooperChordSets[2].freq[3]); // G5
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 349.23f, kBooperChordSets[3].freq[1]); // F4
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 293.66f, kBooperChordSets[4].freq[1]); // D4
}

static void test_notes_ascend_within_each_set() {
    for (int s = 0; s < kBooperChordSetCount; ++s)
        for (int n = 1; n < 4; ++n)
            TEST_ASSERT_TRUE(kBooperChordSets[s].freq[n] > kBooperChordSets[s].freq[n - 1]);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_every_set_has_name_and_four_usable_notes);
    RUN_TEST(test_set_frequencies);
    RUN_TEST(test_notes_ascend_within_each_set);
    return UNITY_END();
}
