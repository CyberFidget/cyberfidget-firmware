// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2026 Dismo Industries LLC

// The `.cfs` v1 reader (lib/DataScreensaver/CfsFormat): byte layout, a
// bit-identical round trip of the 8-frame dinosaur through the streaming
// reader, the 32 KB / 255-frame / 128x64 limits, and every refusal.

#include <unity.h>

#include <string.h>
#include <vector>

#include "CfsFormat.h"
#include "cfs_dino_fixture.h"

using namespace Cfs;

namespace {

// Test-side encoder, written from the layout table in CfsFormat.h.
std::vector<uint8_t> encode(uint16_t w, uint16_t h, uint8_t loop,
                            const std::vector<uint16_t>& durations,
                            const std::vector<std::vector<uint8_t>>& frames,
                            uint8_t version = 1, uint8_t encoding = 0,
                            uint8_t reserved = 0) {
    std::vector<uint8_t> out = {'C', 'F', 'S', '1', version, encoding, loop, reserved};
    auto u16 = [&](uint16_t v) { out.push_back((uint8_t)(v & 0xFF)); out.push_back((uint8_t)(v >> 8)); };
    u16(w);
    u16(h);
    u16((uint16_t)frames.size());
    for (uint16_t d : durations) u16(d);
    for (const auto& f : frames) out.insert(out.end(), f.begin(), f.end());
    return out;
}

std::vector<std::vector<uint8_t>> blankFrames(uint16_t w, uint16_t h, size_t n) {
    std::vector<std::vector<uint8_t>> frames;
    for (size_t i = 0; i < n; ++i) {
        std::vector<uint8_t> f(frameBytes(w, h));
        for (size_t k = 0; k < f.size(); ++k) f[k] = (uint8_t)(k * 31 + i * 7 + 1);
        frames.push_back(f);
    }
    return frames;
}

std::vector<uint8_t> simpleFile(uint16_t w, uint16_t h, size_t n, uint16_t ms = 100,
                                uint8_t loop = 2) {
    return encode(w, h, loop, std::vector<uint16_t>(n, ms), blankFrames(w, h, n));
}

// An in-memory byte source that records every read.
struct MemSource {
    const std::vector<uint8_t>* bytes;
    size_t shortBy = 0;           // return this many bytes fewer than asked
    size_t reads = 0;
    size_t largestRead = 0;
    size_t totalRead = 0;
};

size_t memRead(void* ctx, uint32_t offset, uint8_t* dst, size_t len) {
    MemSource* s = static_cast<MemSource*>(ctx);
    s->reads++;
    if (len > s->largestRead) s->largestRead = len;
    if (offset >= s->bytes->size()) return 0;
    size_t n = len;
    if (offset + n > s->bytes->size()) n = s->bytes->size() - offset;
    if (s->shortBy) n = n > s->shortBy ? n - s->shortBy : 0;
    memcpy(dst, s->bytes->data() + offset, n);
    s->totalRead += n;
    return n;
}

Status openBytes(Reader& r, const std::vector<uint8_t>& bytes, MemSource* srcOut = nullptr) {
    static MemSource src;
    src = MemSource{&bytes};
    Status st = r.open(memRead, &src, (uint32_t)bytes.size());
    if (srcOut) *srcOut = src;
    return st;
}

Status statusOf(const std::vector<uint8_t>& bytes) {
    static Reader r;  // big (one frame buffer): keep it off the stack
    return openBytes(r, bytes);
}

void assertRefused(Status want, const std::vector<uint8_t>& bytes, const char* what) {
    static Reader r;
    // Open a good file first: a refused open must also close the reader.
    const std::vector<uint8_t> good = simpleFile(8, 8, 2);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("ok", statusName(openBytes(r, good)), what);
    TEST_ASSERT_TRUE_MESSAGE(r.loadFrame(1), what);
    const Status got = openBytes(r, bytes);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(statusName(want), statusName(got), what);
    TEST_ASSERT_FALSE_MESSAGE(r.isOpen(), what);
    TEST_ASSERT_FALSE_MESSAGE(r.loadFrame(0), what);
}

Reader g_reader;

}  // namespace

void setUp(void) {}
void tearDown(void) {}

// ---------- layout ----------

// Hand-written bytes, independent of the test encoder: a 9x2 (two bytes per
// row, the 9th pixel in the padded byte), 2-frame ping-pong file.
void test_layout_golden_bytes(void) {
    const std::vector<uint8_t> file = {
        'C', 'F', 'S', '1',
        0x01,              // version
        0x00,              // encoding raw
        0x03,              // ping-pong
        0x00,              // reserved
        0x09, 0x00,        // width 9
        0x02, 0x00,        // height 2
        0x02, 0x00,        // 2 frames
        0x2C, 0x01,        // 300 ms
        0x14, 0x00,        // 20 ms
        0x01, 0x01, 0xFF, 0x00,  // frame 0: (0,0) and (8,0) lit, row 1 = 8 lit
        0x80, 0x00, 0x00, 0x01,  // frame 1: (7,0) and (8,1) lit
    };
    TEST_ASSERT_EQUAL_UINT32(4, frameBytes(9, 2));
    TEST_ASSERT_EQUAL_UINT32(1024, frameBytes(128, 64));
    TEST_ASSERT_EQUAL_UINT32(1024, (uint32_t)kMaxFrameBytes);

    Header h;
    TEST_ASSERT_EQUAL_STRING("ok", statusName(parseHeader(file.data(), file.size(),
                                                          (uint32_t)file.size(), h)));
    TEST_ASSERT_EQUAL_UINT8(1, h.version);
    TEST_ASSERT_EQUAL_UINT8(0, h.encoding);
    TEST_ASSERT_EQUAL_UINT8(3, h.loopMode);
    TEST_ASSERT_EQUAL_UINT16(9, h.width);
    TEST_ASSERT_EQUAL_UINT16(2, h.height);
    TEST_ASSERT_EQUAL_UINT16(2, h.frameCount);
    TEST_ASSERT_EQUAL_UINT32(18, framesOffset(h));
    TEST_ASSERT_EQUAL_UINT32(22, frameOffset(h, 1));
    TEST_ASSERT_EQUAL_UINT32(26, expectedFileBytes(h));

    TEST_ASSERT_EQUAL_STRING("ok", statusName(openBytes(g_reader, file)));
    TEST_ASSERT_EQUAL_UINT16(300, g_reader.durations()[0]);
    TEST_ASSERT_EQUAL_UINT16(20, g_reader.durations()[1]);
    TEST_ASSERT_EQUAL_INT(-1, g_reader.loadedIndex());
    TEST_ASSERT_TRUE(g_reader.loadFrame(1));
    TEST_ASSERT_EQUAL_INT(1, g_reader.loadedIndex());
    const uint8_t want1[] = {0x80, 0x00, 0x00, 0x01};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(want1, g_reader.frame(), 4);
    TEST_ASSERT_TRUE(g_reader.loadFrame(0));
    const uint8_t want0[] = {0x01, 0x01, 0xFF, 0x00};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(want0, g_reader.frame(), 4);
    TEST_ASSERT_FALSE(g_reader.loadFrame(2));  // out of range
}

// The test encoder agrees with the hand-written layout.
void test_encoder_matches_golden_layout(void) {
    const std::vector<uint8_t> enc = encode(9, 2, 3, {300, 20},
        {{0x01, 0x01, 0xFF, 0x00}, {0x80, 0x00, 0x00, 0x01}});
    const uint8_t head[] = {'C','F','S','1',1,0,3,0, 9,0, 2,0, 2,0, 0x2C,0x01, 0x14,0x00};
    TEST_ASSERT_EQUAL_UINT32(26, enc.size());
    TEST_ASSERT_EQUAL_UINT8_ARRAY(head, enc.data(), sizeof(head));
}

// ---------- round trip ----------

void test_dino_round_trip_bit_identical(void) {
    std::vector<std::vector<uint8_t>> frames;
    std::vector<uint16_t> durations;
    for (int i = 0; i < kDinoFrames; ++i) {
        frames.emplace_back(kDinoFramesBits[i], kDinoFramesBits[i] + kDinoFrameBytes);
        durations.push_back(kDinoDurationMs[i]);
    }
    const std::vector<uint8_t> file = encode(kDinoW, kDinoH, 2, durations, frames);
    TEST_ASSERT_EQUAL_UINT32(14 + 2 * 8 + 8 * 512, file.size());

    MemSource src;
    TEST_ASSERT_EQUAL_STRING("ok", statusName(openBytes(g_reader, file, &src)));
    const Header& h = g_reader.header();
    TEST_ASSERT_EQUAL_UINT16(64, h.width);
    TEST_ASSERT_EQUAL_UINT16(64, h.height);
    TEST_ASSERT_EQUAL_UINT16(8, h.frameCount);
    TEST_ASSERT_EQUAL_UINT8(2, h.loopMode);
    for (int i = 0; i < kDinoFrames; ++i)
        TEST_ASSERT_EQUAL_UINT16(kDinoDurationMs[i], g_reader.durations()[i]);

    // Every frame, out of order, comes back byte for byte.
    const int order[] = {3, 0, 7, 1, 6, 2, 5, 4, 0, 7};
    for (int i : order) {
        TEST_ASSERT_TRUE(g_reader.loadFrame((uint16_t)i));
        TEST_ASSERT_EQUAL_UINT8_ARRAY_MESSAGE(kDinoFramesBits[i], g_reader.frame(),
                                              kDinoFrameBytes, "dino frame");
    }
    // The fixture frames really differ, so a reader stuck on one frame fails.
    TEST_ASSERT_NOT_EQUAL(0, memcmp(kDinoFramesBits[0], kDinoFramesBits[1], kDinoFrameBytes));
}

// The reader streams: opening reads only the header and the duration table,
// and each frame is one frame-sized read - never the whole file.
void test_reader_streams_one_frame_at_a_time(void) {
    const std::vector<uint8_t> file = simpleFile(128, 64, 20);
    static MemSource src;
    src = MemSource{&file};
    TEST_ASSERT_EQUAL_STRING("ok", statusName(g_reader.open(memRead, &src, (uint32_t)file.size())));
    TEST_ASSERT_EQUAL_UINT32(14 + 40, src.totalRead);
    src.totalRead = 0;
    src.largestRead = 0;
    TEST_ASSERT_TRUE(g_reader.loadFrame(19));
    TEST_ASSERT_EQUAL_UINT32(1024, src.totalRead);
    TEST_ASSERT_EQUAL_UINT32(1024, src.largestRead);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(file.data() + 14 + 40 + 19 * 1024, g_reader.frame(), 1024);
}

// ---------- limits that are accepted ----------

void test_accepts_limits(void) {
    // Biggest file that fits: 31 full-screen frames (31,820 bytes).
    const std::vector<uint8_t> big = simpleFile(128, 64, 31);
    TEST_ASSERT_TRUE(big.size() <= kMaxFileBytes);
    TEST_ASSERT_EQUAL_STRING("ok", statusName(statusOf(big)));
    // 255 frames of a 1x1 drawing.
    TEST_ASSERT_EQUAL_STRING("ok", statusName(statusOf(simpleFile(1, 1, 255))));
    // Exactly 32 KB: 159 frames of 96x17 (204 bytes) = 14 + 318 + 32,436.
    const std::vector<uint8_t> cap = simpleFile(96, 17, 159);
    TEST_ASSERT_EQUAL_UINT32(kMaxFileBytes, cap.size());
    TEST_ASSERT_EQUAL_STRING("ok", statusName(statusOf(cap)));
    // Full width and height, and the smallest drawing.
    TEST_ASSERT_EQUAL_STRING("ok", statusName(statusOf(simpleFile(128, 64, 1))));
    TEST_ASSERT_EQUAL_STRING("ok", statusName(statusOf(simpleFile(1, 1, 1))));
    // Every loop mode, the shortest duration, a non-zero reserved byte.
    for (uint8_t loop = 0; loop <= 3; ++loop)
        TEST_ASSERT_EQUAL_STRING("ok", statusName(statusOf(simpleFile(16, 8, 3, 100, loop))));
    TEST_ASSERT_EQUAL_STRING("ok", statusName(statusOf(simpleFile(16, 8, 3, 20))));
    TEST_ASSERT_EQUAL_STRING("ok", statusName(statusOf(
        encode(8, 1, 2, {100}, {{0x5A}}, 1, 0, 0xEE))));
}

// ---------- refusals ----------

void test_rejects_unknown_magic(void) {
    std::vector<uint8_t> f = simpleFile(16, 8, 2);
    f[3] = '2';
    assertRefused(Status::BadMagic, f, "CFS2");
    f = simpleFile(16, 8, 2);
    f[0] = 'c';
    assertRefused(Status::BadMagic, f, "cFS1");
    f = simpleFile(16, 8, 2);
    memcpy(f.data(), "\0asm", 4);
    assertRefused(Status::BadMagic, f, "a wasm module");
}

void test_rejects_unknown_version(void) {
    const auto frames = blankFrames(16, 8, 2);
    assertRefused(Status::BadVersion, encode(16, 8, 2, {100, 100}, frames, 0), "version 0");
    assertRefused(Status::BadVersion, encode(16, 8, 2, {100, 100}, frames, 2), "version 2");
    assertRefused(Status::BadVersion, encode(16, 8, 2, {100, 100}, frames, 255), "version 255");
}

void test_rejects_unknown_encoding(void) {
    const auto frames = blankFrames(16, 8, 2);
    assertRefused(Status::BadEncoding, encode(16, 8, 2, {100, 100}, frames, 1, 1), "encoding 1");
    assertRefused(Status::BadEncoding, encode(16, 8, 2, {100, 100}, frames, 1, 255), "encoding 255");
}

void test_rejects_unknown_loop_mode(void) {
    assertRefused(Status::BadLoopMode, simpleFile(16, 8, 2, 100, 4), "loop 4");
    assertRefused(Status::BadLoopMode, simpleFile(16, 8, 2, 100, 255), "loop 255");
}

void test_rejects_bad_dimensions(void) {
    // Each file is internally consistent (frames sized for its w/h), so only
    // the dimension rule can refuse it.
    assertRefused(Status::BadSize, simpleFile(0, 8, 2), "width 0");
    assertRefused(Status::BadSize, simpleFile(16, 0, 2), "height 0");
    assertRefused(Status::BadSize, simpleFile(129, 8, 2), "width 129");
    assertRefused(Status::BadSize, simpleFile(16, 65, 2), "height 65");
    assertRefused(Status::BadSize, simpleFile(0xFFFF, 1, 1), "width 65535");
}

void test_rejects_bad_frame_count(void) {
    assertRefused(Status::BadFrameCount, encode(16, 8, 2, {}, {}), "no frames");
    // 256 frames of a 1x1 drawing: 14 + 512 + 256 bytes, well under the cap.
    const std::vector<uint8_t> f = simpleFile(1, 1, 256);
    TEST_ASSERT_TRUE(f.size() < kMaxFileBytes);
    assertRefused(Status::BadFrameCount, f, "256 frames");
}

void test_rejects_short_durations(void) {
    const auto frames = blankFrames(16, 8, 3);
    assertRefused(Status::BadDuration, encode(16, 8, 2, {100, 100, 19}, frames), "19 ms last");
    assertRefused(Status::BadDuration, encode(16, 8, 2, {0, 100, 100}, frames), "0 ms first");
    assertRefused(Status::BadDuration, encode(16, 8, 2, {100, 1, 100}, frames), "1 ms middle");
}

void test_rejects_size_mismatch(void) {
    std::vector<uint8_t> f = simpleFile(16, 8, 2);
    f.push_back(0);
    assertRefused(Status::SizeMismatch, f, "one trailing byte");
    f = simpleFile(16, 8, 2);
    f.pop_back();
    assertRefused(Status::SizeMismatch, f, "last frame one byte short");
    f = simpleFile(16, 8, 2);
    f[12] = 3;  // claims 3 frames, carries 2
    assertRefused(Status::SizeMismatch, f, "frame count larger than the data");
}

void test_rejects_truncated(void) {
    std::vector<uint8_t> f = simpleFile(16, 8, 2);
    f.resize(13);
    assertRefused(Status::Truncated, f, "13 bytes");
    assertRefused(Status::Truncated, std::vector<uint8_t>(), "empty file");
    Header h;
    const std::vector<uint8_t> good = simpleFile(16, 8, 2);
    TEST_ASSERT_EQUAL_STRING("truncated", statusName(parseHeader(good.data(), 13,
                                                                 (uint32_t)good.size(), h)));
    TEST_ASSERT_EQUAL_STRING("truncated", statusName(parseHeader(nullptr, 14,
                                                                 (uint32_t)good.size(), h)));
    uint16_t d[2];
    TEST_ASSERT_EQUAL_STRING("truncated", statusName(parseDurations(good.data() + 14, 3, 2, d)));
}

void test_rejects_over_32k(void) {
    std::vector<uint8_t> f = simpleFile(96, 17, 159);  // exactly the cap
    f.push_back(0);
    assertRefused(Status::TooLarge, f, "cap plus one byte");
    assertRefused(Status::TooLarge, simpleFile(128, 64, 32), "32 full frames");
    // Refused on its length alone: not one byte is read from it.
    static MemSource src;
    src = MemSource{&f};
    TEST_ASSERT_EQUAL_STRING("too_large", statusName(g_reader.open(memRead, &src, (uint32_t)f.size())));
    TEST_ASSERT_EQUAL_UINT32(0, src.reads);
    Header h;
    TEST_ASSERT_EQUAL_STRING("too_large", statusName(parseHeader(f.data(), f.size(),
                                                                 (uint32_t)f.size(), h)));
}

void test_short_reads_refused(void) {
    const std::vector<uint8_t> f = simpleFile(16, 8, 2);
    static MemSource src;
    src = MemSource{&f};
    src.shortBy = 1;
    TEST_ASSERT_EQUAL_STRING("read_failed", statusName(g_reader.open(memRead, &src, (uint32_t)f.size())));
    TEST_ASSERT_FALSE(g_reader.isOpen());
    TEST_ASSERT_EQUAL_STRING("read_failed", statusName(g_reader.open(nullptr, &src, (uint32_t)f.size())));

    // Opened fine, then the source fails mid-play: the frame load says so.
    src.shortBy = 0;
    TEST_ASSERT_EQUAL_STRING("ok", statusName(g_reader.open(memRead, &src, (uint32_t)f.size())));
    TEST_ASSERT_TRUE(g_reader.loadFrame(0));
    src.shortBy = 1;
    TEST_ASSERT_FALSE(g_reader.loadFrame(1));
    TEST_ASSERT_EQUAL_INT(-1, g_reader.loadedIndex());
}

void test_status_names_are_distinct(void) {
    const Status all[] = {Status::Ok, Status::Truncated, Status::BadMagic, Status::BadVersion,
                          Status::BadEncoding, Status::BadLoopMode, Status::BadSize,
                          Status::BadFrameCount, Status::BadDuration, Status::SizeMismatch,
                          Status::TooLarge, Status::ReadFailed};
    const int n = (int)(sizeof(all) / sizeof(all[0]));
    for (int i = 0; i < n; ++i)
        for (int j = i + 1; j < n; ++j)
            TEST_ASSERT_NOT_EQUAL(0, strcmp(statusName(all[i]), statusName(all[j])));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_layout_golden_bytes);
    RUN_TEST(test_encoder_matches_golden_layout);
    RUN_TEST(test_dino_round_trip_bit_identical);
    RUN_TEST(test_reader_streams_one_frame_at_a_time);
    RUN_TEST(test_accepts_limits);
    RUN_TEST(test_rejects_unknown_magic);
    RUN_TEST(test_rejects_unknown_version);
    RUN_TEST(test_rejects_unknown_encoding);
    RUN_TEST(test_rejects_unknown_loop_mode);
    RUN_TEST(test_rejects_bad_dimensions);
    RUN_TEST(test_rejects_bad_frame_count);
    RUN_TEST(test_rejects_short_durations);
    RUN_TEST(test_rejects_size_mismatch);
    RUN_TEST(test_rejects_truncated);
    RUN_TEST(test_rejects_over_32k);
    RUN_TEST(test_short_reads_refused);
    RUN_TEST(test_status_names_are_distinct);
    return UNITY_END();
}
