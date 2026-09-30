// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

// test/test_core_boardinfo/test_boardinfo.cpp
//
// Pure board identity block parsing: unprogrammed boards fall back to the
// rev 1.2 defaults, a layout-1 block yields its fields, flag bits decode
// independently, and an unrecognised layout keeps the defaults while
// reporting the raw layout number. No eFuse access; the HAL glue that reads
// the block is not compiled here.

#include <unity.h>
#include <cstring>
#include "BoardInfo.h"
#include "DeviceIdentity.h"

using namespace BoardInfo;

static void fill(uint8_t blk[32], uint8_t value) { std::memset(blk, value, 32); }

static void assertDefaults(const Info& info) {
    TEST_ASSERT_EQUAL_UINT8(1, info.major);
    TEST_ASSERT_EQUAL_UINT8(2, info.minor);
    TEST_ASSERT_FALSE(info.hil);
    TEST_ASSERT_FALSE(info.engSample);
    TEST_ASSERT_EQUAL_UINT8(0, info.year);
    TEST_ASSERT_EQUAL_UINT8(0, info.month);
    TEST_ASSERT_EQUAL_UINT8(0, info.lot);
    TEST_ASSERT_EQUAL_UINT8(0, info.variant);
}

static void buildV1(uint8_t blk[32], uint8_t flags) {
    fill(blk, 0);
    blk[7]  = 0xCF;
    blk[8]  = 0x01;
    blk[9]  = 1;
    blk[10] = 2;
    blk[11] = 5;      // 2025
    blk[20] = flags;
    blk[21] = 9;      // September
    blk[22] = 3;      // lot
    blk[24] = 0x04;   // variant bitfield
}

void test_magic_absent_is_default_rev_1_2(void) {
    uint8_t blk[32];
    fill(blk, 0);
    blk[8] = 0x01;    // layout byte alone means nothing without the magic
    blk[9] = 7;
    const Info info = parseBoardBlock(blk);
    TEST_ASSERT_TRUE(info.source == Source::Default);
    TEST_ASSERT_EQUAL_UINT8(0, info.layoutVersion);
    assertDefaults(info);
}

void test_valid_v1_block_yields_fields(void) {
    uint8_t blk[32];
    buildV1(blk, 0x00);
    const Info info = parseBoardBlock(blk);
    TEST_ASSERT_TRUE(info.source == Source::Efuse);
    TEST_ASSERT_EQUAL_UINT8(1, info.layoutVersion);
    TEST_ASSERT_EQUAL_UINT8(1, info.major);
    TEST_ASSERT_EQUAL_UINT8(2, info.minor);
    TEST_ASSERT_EQUAL_UINT8(5, info.year);
    TEST_ASSERT_EQUAL_UINT8(9, info.month);
    TEST_ASSERT_EQUAL_UINT8(3, info.lot);
    TEST_ASSERT_EQUAL_UINT8(0x04, info.variant);
    TEST_ASSERT_FALSE(info.hil);
    TEST_ASSERT_FALSE(info.engSample);
}

void test_v1_block_other_rev(void) {
    uint8_t blk[32];
    buildV1(blk, 0x00);
    blk[9] = 2;
    blk[10] = 0;
    const Info info = parseBoardBlock(blk);
    TEST_ASSERT_EQUAL_UINT8(2, info.major);
    TEST_ASSERT_EQUAL_UINT8(0, info.minor);
}

void test_hil_flag_bit(void) {
    uint8_t blk[32];
    buildV1(blk, 0x01);
    const Info info = parseBoardBlock(blk);
    TEST_ASSERT_TRUE(info.hil);
    TEST_ASSERT_FALSE(info.engSample);
}

void test_eng_sample_flag_bit(void) {
    uint8_t blk[32];
    buildV1(blk, 0x02);
    const Info info = parseBoardBlock(blk);
    TEST_ASSERT_FALSE(info.hil);
    TEST_ASSERT_TRUE(info.engSample);
}

void test_both_flags_and_reserved_bits_ignored(void) {
    uint8_t blk[32];
    buildV1(blk, 0xFF);
    const Info info = parseBoardBlock(blk);
    TEST_ASSERT_TRUE(info.hil);
    TEST_ASSERT_TRUE(info.engSample);
}

void test_unknown_layout_keeps_defaults_and_raw_layout(void) {
    uint8_t blk[32];
    buildV1(blk, 0x03);
    blk[8] = 0x02;
    const Info info = parseBoardBlock(blk);
    TEST_ASSERT_TRUE(info.source == Source::UnknownLayout);
    TEST_ASSERT_EQUAL_UINT8(2, info.layoutVersion);
    assertDefaults(info);
}

void test_all_zero_block_is_default(void) {
    uint8_t blk[32];
    fill(blk, 0x00);
    const Info info = parseBoardBlock(blk);
    TEST_ASSERT_TRUE(info.source == Source::Default);
    assertDefaults(info);
}

void test_all_ff_block_is_default(void) {
    uint8_t blk[32];
    fill(blk, 0xFF);
    const Info info = parseBoardBlock(blk);
    TEST_ASSERT_TRUE(info.source == Source::Default);
    TEST_ASSERT_EQUAL_UINT8(0, info.layoutVersion);
    assertDefaults(info);
}

void test_null_block_is_default(void) {
    const Info info = parseBoardBlock(nullptr);
    TEST_ASSERT_TRUE(info.source == Source::Default);
    assertDefaults(info);
}

void test_source_names(void) {
    TEST_ASSERT_EQUAL_STRING("efuse", sourceName(Source::Efuse));
    TEST_ASSERT_EQUAL_STRING("default", sourceName(Source::Default));
    TEST_ASSERT_EQUAL_STRING("unknown-layout", sourceName(Source::UnknownLayout));
    TEST_ASSERT_EQUAL_STRING("read-error", sourceName(Source::ReadError));
}

void test_serial_from_layout_block(void) {
    uint8_t blk[32];
    buildV1(blk, 0);
    TEST_ASSERT_EQUAL_UINT32(0, parseBoardBlock(blk).serial);
    blk[25] = 0x12;
    blk[26] = 0x34;
    blk[27] = 0xab;
    blk[28] = 0xcd;
    TEST_ASSERT_EQUAL_UINT32(0x1234abcd, parseBoardBlock(blk).serial);
    blk[8] = 2;
    TEST_ASSERT_EQUAL_UINT32(0, parseBoardBlock(blk).serial);
}

void test_flash_read_sanity(void) {
    using DeviceIdentity::validFlashReads;
    TEST_ASSERT_FALSE(validFlashReads(false, 1, true, 1));
    TEST_ASSERT_FALSE(validFlashReads(true, 1, false, 1));
    TEST_ASSERT_FALSE(validFlashReads(true, 1, true, 2));
    TEST_ASSERT_FALSE(validFlashReads(true, 0, true, 0));
    TEST_ASSERT_FALSE(validFlashReads(true, UINT64_MAX, true, UINT64_MAX));
    TEST_ASSERT_TRUE(validFlashReads(true, 0x0102030405060708ULL,
                                     true, 0x0102030405060708ULL));
    char hex[17];
    DeviceIdentity::hex64(0x0102030405060708ULL, hex);
    TEST_ASSERT_EQUAL_STRING("0102030405060708", hex);
}

void test_fingerprint_both_sides_rule(void) {
    using DeviceIdentity::Fingerprint;
    Fingerprint stored, live;
    TEST_ASSERT_FALSE(DeviceIdentity::matches(stored, live, true));
    TEST_ASSERT_TRUE(DeviceIdentity::matches(stored, live, false));
    std::strcpy(stored.id, "aabbccddeeff");
    TEST_ASSERT_TRUE(DeviceIdentity::matches(stored, live, true));
    std::strcpy(live.id, "aabbccddeeff");
    TEST_ASSERT_TRUE(DeviceIdentity::matches(stored, live, true));
    std::strcpy(live.id, "112233445566");
    TEST_ASSERT_FALSE(DeviceIdentity::matches(stored, live, true));
    std::strcpy(live.id, stored.id);
    std::strcpy(stored.flashId, "0123456789abcdef");
    TEST_ASSERT_TRUE(DeviceIdentity::matches(stored, live, true));
    std::strcpy(live.flashId, "0123456789abcdef");
    TEST_ASSERT_TRUE(DeviceIdentity::matches(stored, live, true));
    std::strcpy(live.flashId, "fedcba9876543210");
    TEST_ASSERT_FALSE(DeviceIdentity::matches(stored, live, true));
    live.flashId[0] = 0;
    std::strcpy(stored.serial, "1234abcd");
    TEST_ASSERT_TRUE(DeviceIdentity::matches(stored, live, true));
    std::strcpy(live.serial, "1234abcd");
    TEST_ASSERT_TRUE(DeviceIdentity::matches(stored, live, true));
    std::strcpy(live.serial, "1234abce");
    TEST_ASSERT_FALSE(DeviceIdentity::matches(stored, live, true));
    stored.serial[0] = 0;
    TEST_ASSERT_TRUE(DeviceIdentity::matches(stored, live, true));
    char serial[9];
    DeviceIdentity::serialHex(0, serial);
    TEST_ASSERT_EQUAL_STRING("", serial);
    DeviceIdentity::serialHex(0x1234abcd, serial);
    TEST_ASSERT_EQUAL_STRING("1234abcd", serial);
}

void setUp(void)    {}
void tearDown(void) {}

int main(int /*argc*/, char** /*argv*/) {
    UNITY_BEGIN();
    RUN_TEST(test_magic_absent_is_default_rev_1_2);
    RUN_TEST(test_valid_v1_block_yields_fields);
    RUN_TEST(test_v1_block_other_rev);
    RUN_TEST(test_hil_flag_bit);
    RUN_TEST(test_eng_sample_flag_bit);
    RUN_TEST(test_both_flags_and_reserved_bits_ignored);
    RUN_TEST(test_unknown_layout_keeps_defaults_and_raw_layout);
    RUN_TEST(test_all_zero_block_is_default);
    RUN_TEST(test_all_ff_block_is_default);
    RUN_TEST(test_null_block_is_default);
    RUN_TEST(test_source_names);
    RUN_TEST(test_serial_from_layout_block);
    RUN_TEST(test_flash_read_sanity);
    RUN_TEST(test_fingerprint_both_sides_rule);
    return UNITY_END();
}
