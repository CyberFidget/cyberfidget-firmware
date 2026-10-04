// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2023-2026 Dismo Industries LLC

#include <unity.h>
#include <limits>
#include "Arduino.h"

namespace {
template<typename T>
void assertNumeric(T value, const char* expected) {
    String appended;
    TEST_ASSERT_TRUE(&(appended += value) == &appended);
    TEST_ASSERT_EQUAL_STRING(expected, appended.c_str());
    String concatenated;
    TEST_ASSERT_TRUE(concatenated.concat(value));
    TEST_ASSERT_EQUAL_STRING(expected, concatenated.c_str());
    TEST_ASSERT_EQUAL_STRING(expected, String(value).c_str());
    TEST_ASSERT_EQUAL_STRING((std::string("=") + expected).c_str(), (String("=") + value).c_str());
    TEST_ASSERT_EQUAL_STRING((std::string(expected) + "!").c_str(), (value + String("!")).c_str());
}
void test_int_append() { assertNumeric(3, "3"); }
void test_negative_numbers() {
    assertNumeric(-42, "-42");
    assertNumeric(-123L, "-123");
    assertNumeric(std::numeric_limits<long long>::min(), "-9223372036854775808");
}
void test_unsigned_numbers() {
    assertNumeric(42U, "42");
    assertNumeric(4294967295UL, "4294967295");
    assertNumeric(std::numeric_limits<unsigned long long>::max(), "18446744073709551615");
    assertNumeric(static_cast<unsigned char>(255), "255");
}
void test_integer_aliases() {
    assertNumeric(int8_t(-8), "-8");
    assertNumeric(int16_t(-16), "-16");
    assertNumeric(uint8_t(8), "8");
    assertNumeric(uint16_t(16), "16");
    assertNumeric(size_t(64), "64");
    assertNumeric(int32_t(-32), "-32");
    assertNumeric(uint32_t(32), "32");
    assertNumeric(int64_t(-64), "-64");
    assertNumeric(uint64_t(64), "64");
}
void test_character_append() {
    String s("A");
    s += 'B';
    TEST_ASSERT_TRUE(s.concat('C'));
    TEST_ASSERT_EQUAL_STRING("ABC", s.c_str());
    TEST_ASSERT_EQUAL_STRING("ABCD", (s + 'D').c_str());
    TEST_ASSERT_EQUAL_STRING("ZABC", ('Z' + s).c_str());
    TEST_ASSERT_EQUAL_STRING("Q", String('Q').c_str());
    s += char(0);
    TEST_ASSERT_EQUAL_UINT(4, s.length());
    TEST_ASSERT_EQUAL_STRING("", String(char(0)).c_str());
}
void test_floating_append() {
    assertNumeric(1.5f, "1.50");
    assertNumeric(-1.5, "-1.50");
    // The core dtostrf digit extraction yields 1.12 for this binary tie.
    assertNumeric(1.125, "1.12");
    assertNumeric(-0.0, "0.00");
    assertNumeric(std::numeric_limits<double>::infinity(), "inf");
    assertNumeric(-std::numeric_limits<double>::infinity(), "inf");
    assertNumeric(std::numeric_limits<double>::quiet_NaN(), "nan");
}
void test_float_decimal_places() {
    TEST_ASSERT_EQUAL_STRING("1.5", String(1.5f, 1).c_str());
    TEST_ASSERT_EQUAL_STRING("1.500", String(1.5, 3).c_str());
    TEST_ASSERT_EQUAL_STRING(" 2", String(1.5, 0).c_str());
    TEST_ASSERT_EQUAL_STRING("2.00", String(1.999, 2).c_str());
    TEST_ASSERT_EQUAL_STRING("1.5000000000000000000000000000000000000000", String(1.5, 40).c_str());
}
void test_integer_bases() {
    TEST_ASSERT_EQUAL_STRING("ff", String(255, 16).c_str());
    TEST_ASSERT_EQUAL_STRING("-ff", String(-255L, 16).c_str());
    TEST_ASSERT_EQUAL_STRING("101", String(5U, 2).c_str());
    TEST_ASSERT_EQUAL_STRING("377", String(static_cast<unsigned char>(255), 8).c_str());
    TEST_ASSERT_EQUAL_STRING("ffffffffffffffff", String(std::numeric_limits<unsigned long long>::max(), 16).c_str());
    TEST_ASSERT_EQUAL_STRING("-ff", String(-255LL, 16).c_str());
    TEST_ASSERT_EQUAL_STRING("", String(3, 1).c_str());
    TEST_ASSERT_EQUAL_STRING("", String(3, 17).c_str());
}
void test_string_chains() {
    TEST_ASSERT_EQUAL_STRING("L1 Lv:3 D:0", ("L" + String(1) + " Lv:" + 3 + " D:" + 0).c_str());
    TEST_ASSERT_EQUAL_STRING("3!", (3 + String("!")).c_str());
}
void test_breakout_hud() {
    String hud = "L";
    hud += 1;
    hud += " Lv:";
    hud += 3;
    hud += " D:";
    hud += 0;
    TEST_ASSERT_EQUAL_STRING("L1 Lv:3 D:0", hud.c_str());
}
void test_concat_text_and_null() {
    String s("a");
    TEST_ASSERT_TRUE(s.concat(String("b")));
    TEST_ASSERT_TRUE(s.concat("c"));
    TEST_ASSERT_FALSE(s.concat(static_cast<const char*>(nullptr)));
    TEST_ASSERT_EQUAL_STRING("abc", s.c_str());
    TEST_ASSERT_TRUE(s.concat(s));
    TEST_ASSERT_EQUAL_STRING("abcabc", s.c_str());
}
} // namespace

void setUp() {}
void tearDown() {}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_int_append);
    RUN_TEST(test_negative_numbers);
    RUN_TEST(test_unsigned_numbers);
    RUN_TEST(test_integer_aliases);
    RUN_TEST(test_character_append);
    RUN_TEST(test_floating_append);
    RUN_TEST(test_float_decimal_places);
    RUN_TEST(test_integer_bases);
    RUN_TEST(test_string_chains);
    RUN_TEST(test_breakout_hud);
    RUN_TEST(test_concat_text_and_null);
    return UNITY_END();
}
