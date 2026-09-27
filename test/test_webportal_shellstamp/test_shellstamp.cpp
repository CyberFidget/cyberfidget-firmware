// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

// Pure companion-shell stamp extraction and ordering. No device runtime or
// storage dependency; the web portal consumes the same header directly.

#include <unity.h>
#include <cstring>
#include "ShellStamp.h"
#include "PortalPassword.h"
#include "PortalSsid.h"

using namespace ShellStamp;

void test_absent_empty_and_malformed_card_stamps_are_older(void) {
    TEST_ASSERT_TRUE(cardShellIsOlder(nullptr, "1.3.3+12345678"));
    TEST_ASSERT_TRUE(cardShellIsOlder("", "1.3.3+12345678"));
    TEST_ASSERT_TRUE(cardShellIsOlder("abc", "1.3.3+12345678"));
    TEST_ASSERT_TRUE(cardShellIsOlder("1.2", "1.3.3+12345678"));
    TEST_ASSERT_TRUE(cardShellIsOlder("1.2.x", "1.3.3+12345678"));
    TEST_ASSERT_TRUE(cardShellIsOlder("01.2.3", "1.3.3+12345678"));
}

void test_unparseable_built_in_stamp_fails_open(void) {
    TEST_ASSERT_FALSE(cardShellIsOlder("1.0.0+12345678", nullptr));
    TEST_ASSERT_FALSE(cardShellIsOlder("1.0.0+12345678", "broken"));
}

void test_identical_stamp_is_not_older(void) {
    TEST_ASSERT_FALSE(cardShellIsOlder("1.3.3+12345678", "1.3.3+12345678"));
}

void test_card_older_by_each_numeric_component(void) {
    TEST_ASSERT_TRUE(cardShellIsOlder("1.9.9+aaaaaaaa", "2.0.0+bbbbbbbb"));
    TEST_ASSERT_TRUE(cardShellIsOlder("2.3.9+aaaaaaaa", "2.4.0+bbbbbbbb"));
    TEST_ASSERT_TRUE(cardShellIsOlder("2.4.8+aaaaaaaa", "2.4.9+bbbbbbbb"));
}

void test_card_newer_by_each_numeric_component(void) {
    TEST_ASSERT_FALSE(cardShellIsOlder("3.0.0+aaaaaaaa", "2.9.9+bbbbbbbb"));
    TEST_ASSERT_FALSE(cardShellIsOlder("2.5.0+aaaaaaaa", "2.4.9+bbbbbbbb"));
    TEST_ASSERT_FALSE(cardShellIsOlder("2.4.10+aaaaaaaa", "2.4.9+bbbbbbbb"));
}

void test_equal_version_with_different_metadata_is_not_older(void) {
    TEST_ASSERT_FALSE(cardShellIsOlder("1.3.3+aaaaaaaa", "1.3.3+bbbbbbbb"));
}

void test_multi_digit_components_compare_numerically(void) {
    TEST_ASSERT_FALSE(cardShellIsOlder("1.10.3+aaaaaaaa", "1.9.9+bbbbbbbb"));
    TEST_ASSERT_TRUE(cardShellIsOlder("1.10.3+aaaaaaaa", "1.100.0+bbbbbbbb"));
}

void test_stamp_found_at_buffer_start(void) {
    const char doc[] = "<meta name=\"cf-shell\" content=\"1.3.3+a1b2c3d4\"><title>x</title>";
    char stamp[kMaxShellStampLen];
    TEST_ASSERT_TRUE(findShellStamp(doc, sizeof(doc) - 1, stamp, sizeof(stamp)));
    TEST_ASSERT_EQUAL_STRING("1.3.3+a1b2c3d4", stamp);
}

void test_absent_stamp_is_clean_miss(void) {
    const char doc[] = "<html><head><title>No stamp</title></head></html>";
    char stamp[kMaxShellStampLen] = "not-empty";
    TEST_ASSERT_FALSE(findShellStamp(doc, sizeof(doc) - 1, stamp, sizeof(stamp)));
    TEST_ASSERT_EQUAL_STRING("", stamp);
}

void test_empty_stamp_tag_is_clean_miss(void) {
    const char doc[] = "<meta name=\"cf-shell\" content=\"\">";
    char stamp[kMaxShellStampLen] = "not-empty";
    TEST_ASSERT_FALSE(findShellStamp(doc, sizeof(doc) - 1, stamp, sizeof(stamp)));
    TEST_ASSERT_EQUAL_STRING("", stamp);
}

void test_stamp_truncated_at_buffer_end_is_clean_miss(void) {
    const char doc[] = "prefix<meta name=\"cf-shell\" content=\"1.3.3+a1b2";
    char stamp[kMaxShellStampLen] = "not-empty";
    TEST_ASSERT_FALSE(findShellStamp(doc, sizeof(doc) - 1, stamp, sizeof(stamp)));
    TEST_ASSERT_EQUAL_STRING("", stamp);
}

void test_non_terminated_buffer_is_supported(void) {
    const char text[] = "xx<meta name=\"cf-shell\" content=\"12.34.567+Ab-c\">yy";
    char doc[sizeof(text) - 1];
    std::memcpy(doc, text, sizeof(doc));
    char stamp[kMaxShellStampLen];
    TEST_ASSERT_TRUE(findShellStamp(doc, sizeof(doc), stamp, sizeof(stamp)));
    TEST_ASSERT_EQUAL_STRING("12.34.567+Ab-c", stamp);
}

void test_extraction_rejects_unescaped_or_oversized_values(void) {
    const char unsafe[] = "<meta name=\"cf-shell\" content=\"1.2.3+bad/value\">";
    const char longValue[] = "<meta name=\"cf-shell\" content=\"1.2.3+abcdefghijklmnopqrstuvwxyz\">";
    char stamp[kMaxShellStampLen];
    TEST_ASSERT_FALSE(findShellStamp(unsafe, sizeof(unsafe) - 1, stamp, sizeof(stamp)));
    TEST_ASSERT_FALSE(findShellStamp(longValue, sizeof(longValue) - 1, stamp, sizeof(stamp)));
}

void test_portal_password_has_eight_digits_and_preserves_leading_zeroes(void) {
    char password[9];
    PortalPassword::generate(password, []() { return uint32_t(100000001); });
    TEST_ASSERT_EQUAL_STRING("00000001", password);
    // The screen now draws this same string (no "0000 0001" grouping: the
    // space read as part of the password), so it must be exactly eight
    // digits with nothing between them.
    TEST_ASSERT_EQUAL_size_t(8, strlen(password));
    for (int i = 0; i < 8; ++i) TEST_ASSERT_TRUE(password[i] >= '0' && password[i] <= '9');
}

void test_portal_password_rejects_biased_random_values(void) {
    char password[9];
    int calls = 0;
    PortalPassword::generate(password, [&calls]() {
        return ++calls == 1 ? uint32_t(0) : uint32_t(199999999);
    });
    TEST_ASSERT_EQUAL_INT(2, calls);
    TEST_ASSERT_EQUAL_STRING("99999999", password);
}

void test_portal_password_changes_with_supplied_random_value(void) {
    char first[9], second[9];
    PortalPassword::generate(first, []() { return uint32_t(100000000); });
    PortalPassword::generate(second, []() { return uint32_t(112345678); });
    TEST_ASSERT_EQUAL_STRING("00000000", first);
    TEST_ASSERT_EQUAL_STRING("12345678", second);
}

// The portal network name: prefix + last four of the canonical unit id
// (HIL-A's id 004b12a30b50 -> "CyberFidget-0b50", the website's fingerprint).
void test_portal_ssid_uses_last_four_of_the_unit_id(void) {
    char ssid[PortalSsid::kMaxLen + 1];
    PortalSsid::build("004b12a30b50", ssid);
    TEST_ASSERT_EQUAL_STRING("CyberFidget-0b50", ssid);
    PortalSsid::build("004b12a30570", ssid);
    TEST_ASSERT_EQUAL_STRING("CyberFidget-0570", ssid);
    TEST_ASSERT_TRUE(strlen(ssid) <= 32);
    TEST_ASSERT_EQUAL_size_t(PortalSsid::kMaxLen, strlen(ssid));
}

void test_portal_ssid_is_lowercase(void) {
    char ssid[PortalSsid::kMaxLen + 1];
    PortalSsid::build("004B12A3ABCD", ssid);
    TEST_ASSERT_EQUAL_STRING("CyberFidget-abcd", ssid);
    for (const char* p = ssid + strlen(PortalSsid::kPrefix); *p; ++p) {
        TEST_ASSERT_FALSE(*p >= 'A' && *p <= 'Z');
    }
}

void test_portal_ssid_short_or_missing_id(void) {
    char ssid[PortalSsid::kMaxLen + 1];
    PortalSsid::build("ab", ssid);
    TEST_ASSERT_EQUAL_STRING("CyberFidget-ab", ssid);
    PortalSsid::build(nullptr, ssid);
    TEST_ASSERT_EQUAL_STRING("CyberFidget-", ssid);
}

void setUp(void)    {}
void tearDown(void) {}

int main(int /*argc*/, char** /*argv*/) {
    UNITY_BEGIN();
    RUN_TEST(test_absent_empty_and_malformed_card_stamps_are_older);
    RUN_TEST(test_unparseable_built_in_stamp_fails_open);
    RUN_TEST(test_identical_stamp_is_not_older);
    RUN_TEST(test_card_older_by_each_numeric_component);
    RUN_TEST(test_card_newer_by_each_numeric_component);
    RUN_TEST(test_equal_version_with_different_metadata_is_not_older);
    RUN_TEST(test_multi_digit_components_compare_numerically);
    RUN_TEST(test_stamp_found_at_buffer_start);
    RUN_TEST(test_absent_stamp_is_clean_miss);
    RUN_TEST(test_empty_stamp_tag_is_clean_miss);
    RUN_TEST(test_stamp_truncated_at_buffer_end_is_clean_miss);
    RUN_TEST(test_non_terminated_buffer_is_supported);
    RUN_TEST(test_extraction_rejects_unescaped_or_oversized_values);
    RUN_TEST(test_portal_password_has_eight_digits_and_preserves_leading_zeroes);
    RUN_TEST(test_portal_password_rejects_biased_random_values);
    RUN_TEST(test_portal_password_changes_with_supplied_random_value);
    RUN_TEST(test_portal_ssid_uses_last_four_of_the_unit_id);
    RUN_TEST(test_portal_ssid_is_lowercase);
    RUN_TEST(test_portal_ssid_short_or_missing_id);
    return UNITY_END();
}
