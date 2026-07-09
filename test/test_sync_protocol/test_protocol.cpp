// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

// test/test_sync_protocol/test_protocol.cpp
//
// Pure serial-sync transport logic: the CRC-32 the framing checksums every
// payload with (including the corruption-rejection case), the write
// confinement predicate (traversal / root-escape battery), and the command
// argument parsers. No UART, no filesystem — SerialCli's device glue is not
// compiled here.

#include <unity.h>
#include <cstring>
#include "SyncProtocol.h"

using namespace SyncProtocol;

// ---------- CRC-32 ----------

void test_crc32_known_check_value(void) {
    // The canonical CRC-32 (IEEE, reflected) check value for "123456789".
    const char* s = "123456789";
    TEST_ASSERT_EQUAL_HEX32(0xCBF43926u, crc32(s, std::strlen(s)));
}

void test_crc32_empty_is_zero(void) {
    TEST_ASSERT_EQUAL_HEX32(0x00000000u, crc32("", 0));
}

void test_crc32_streaming_matches_oneshot(void) {
    const char* s = "the quick brown fox";
    size_t n = std::strlen(s);
    uint32_t whole = crc32(s, n);
    // Split the buffer and stream it in two halves.
    uint32_t c = crc32Begin();
    c = crc32Update(c, s, 7);
    c = crc32Update(c, s + 7, n - 7);
    TEST_ASSERT_EQUAL_HEX32(whole, crc32Finish(c));
}

void test_crc32_detects_single_bit_flip(void) {
    // The corruption-rejection guarantee reduces to this: a payload with any
    // byte changed yields a different crc, so the framing NAKs it.
    uint8_t good[64];
    for (int i = 0; i < 64; i++) good[i] = (uint8_t)(i * 7 + 3);
    uint32_t a = crc32(good, sizeof(good));
    good[40] ^= 0x01; // flip one bit
    uint32_t b = crc32(good, sizeof(good));
    TEST_ASSERT_NOT_EQUAL(a, b);
}

// ---------- path confinement ----------

void test_confined_paths_accepted(void) {
    TEST_ASSERT_TRUE(pathConfined("/apps/hello.wasm"));
    TEST_ASSERT_TRUE(pathConfined("/assets/img.bin"));
    TEST_ASSERT_TRUE(pathConfined("/apps/sub/dir/nested.dat"));
}

void test_confinement_rejects_root_escape(void) {
    TEST_ASSERT_FALSE(pathConfined("/"));            // bare root
    TEST_ASSERT_FALSE(pathConfined("/etc/passwd"));  // outside allowed roots
    TEST_ASSERT_FALSE(pathConfined("/loadout.json")); // manifest is lapply-only
    TEST_ASSERT_FALSE(pathConfined("apps/x"));       // not absolute
    TEST_ASSERT_FALSE(pathConfined(""));
    TEST_ASSERT_FALSE(pathConfined(nullptr));
}

void test_confinement_rejects_traversal(void) {
    TEST_ASSERT_FALSE(pathConfined("/apps/../secret"));
    TEST_ASSERT_FALSE(pathConfined("/apps/../../etc/x"));
    TEST_ASSERT_FALSE(pathConfined("/assets/./x"));
    TEST_ASSERT_FALSE(pathConfined("/apps//x"));     // empty segment
    TEST_ASSERT_FALSE(pathConfined("/apps/.."));
}

void test_confinement_rejects_dir_and_bad_bytes(void) {
    TEST_ASSERT_FALSE(pathConfined("/apps/"));       // names a dir, not a file
    TEST_ASSERT_FALSE(pathConfined("/apps/a b"));    // embedded space
    char withCtrl[] = "/apps/x";
    withCtrl[6] = 0x09; // tab in the name
    TEST_ASSERT_FALSE(pathConfined(withCtrl));
}

void test_confinement_rejects_overlong(void) {
    char buf[kMaxPathLen + 16];
    std::strcpy(buf, "/apps/");
    size_t base = std::strlen(buf);
    for (size_t i = base; i < sizeof(buf) - 1; i++) buf[i] = 'a';
    buf[sizeof(buf) - 1] = '\0';
    TEST_ASSERT_FALSE(pathConfined(buf));
}

// ---------- command argument parsers ----------

void test_parse_write_open_ok(void) {
    char path[kMaxPathLen + 1];
    uint32_t size = 0, crc = 0;
    TEST_ASSERT_TRUE(parseWriteOpen("/apps/x.wasm 4096 1a2b3c4d",
                                    path, sizeof(path), size, crc));
    TEST_ASSERT_EQUAL_STRING("/apps/x.wasm", path);
    TEST_ASSERT_EQUAL_UINT32(4096u, size);
    TEST_ASSERT_EQUAL_HEX32(0x1a2b3c4du, crc);
}

void test_parse_write_open_rejects_malformed(void) {
    char path[kMaxPathLen + 1];
    uint32_t size = 0, crc = 0;
    TEST_ASSERT_FALSE(parseWriteOpen("/apps/x 4096", path, sizeof(path), size, crc)); // missing crc
    TEST_ASSERT_FALSE(parseWriteOpen("/apps/x 4096 zz", path, sizeof(path), size, crc)); // bad hex
    TEST_ASSERT_FALSE(parseWriteOpen("/apps/x 4096 1a2b3c4d junk", path, sizeof(path), size, crc)); // trailing
    TEST_ASSERT_FALSE(parseWriteOpen("", path, sizeof(path), size, crc));
    TEST_ASSERT_FALSE(parseWriteOpen(nullptr, path, sizeof(path), size, crc));
}

void test_parse_chunk_header_ok(void) {
    uint32_t off = 0, len = 0, crc = 0;
    TEST_ASSERT_TRUE(parseChunkHeader("8192 256 deadbeef", off, len, crc));
    TEST_ASSERT_EQUAL_UINT32(8192u, off);
    TEST_ASSERT_EQUAL_UINT32(256u, len);
    TEST_ASSERT_EQUAL_HEX32(0xdeadbeefu, crc);
}

void test_parse_chunk_header_rejects_overflow(void) {
    uint32_t off = 0, len = 0, crc = 0;
    // Offset over 2^32 must not silently wrap.
    TEST_ASSERT_FALSE(parseChunkHeader("4294967296 1 0", off, len, crc));
    TEST_ASSERT_FALSE(parseChunkHeader("0 1 1abcdef012", off, len, crc)); // >8 hex digits
}

void test_parse_apply_header_ok(void) {
    uint32_t len = 0, crc = 0;
    TEST_ASSERT_TRUE(parseApplyHeader("128 0", len, crc));
    TEST_ASSERT_EQUAL_UINT32(128u, len);
    TEST_ASSERT_EQUAL_HEX32(0u, crc);
}

void test_parse_path_arg(void) {
    char path[kMaxPathLen + 1];
    TEST_ASSERT_TRUE(parsePathArg("/assets/y.bin", path, sizeof(path)));
    TEST_ASSERT_EQUAL_STRING("/assets/y.bin", path);
    TEST_ASSERT_FALSE(parsePathArg("/assets/y.bin extra", path, sizeof(path)));
    TEST_ASSERT_FALSE(parsePathArg("   ", path, sizeof(path)));
}

void setUp(void)    {}
void tearDown(void) {}

int main(int /*argc*/, char** /*argv*/) {
    UNITY_BEGIN();
    RUN_TEST(test_crc32_known_check_value);
    RUN_TEST(test_crc32_empty_is_zero);
    RUN_TEST(test_crc32_streaming_matches_oneshot);
    RUN_TEST(test_crc32_detects_single_bit_flip);
    RUN_TEST(test_confined_paths_accepted);
    RUN_TEST(test_confinement_rejects_root_escape);
    RUN_TEST(test_confinement_rejects_traversal);
    RUN_TEST(test_confinement_rejects_dir_and_bad_bytes);
    RUN_TEST(test_confinement_rejects_overlong);
    RUN_TEST(test_parse_write_open_ok);
    RUN_TEST(test_parse_write_open_rejects_malformed);
    RUN_TEST(test_parse_chunk_header_ok);
    RUN_TEST(test_parse_chunk_header_rejects_overflow);
    RUN_TEST(test_parse_apply_header_ok);
    RUN_TEST(test_parse_path_arg);
    return UNITY_END();
}
