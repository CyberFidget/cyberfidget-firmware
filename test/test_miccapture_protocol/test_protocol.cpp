// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

// test/test_miccapture_protocol/test_protocol.cpp
//
// Pure-logic tests for the live caption link: the typed-JSON message
// parser/builders (LiveLinkProtocol.h) and the caption word-wrap
// (CaptionWrap.h, measured with a fake fixed-width font). No HAL, no
// Arduino — these are the host-testable halves of the /ws/live surface.

#include <unity.h>
#include <cstring>

#include "LiveLinkProtocol.h"
#include "CaptionWrap.h"

void setUp(void) {}
void tearDown(void) {}

// ─── LiveLink::parseMessage ───

void test_parse_time_message(void) {
    LiveLink::Msg msg;
    TEST_ASSERT_TRUE(LiveLink::parseMessage(
        "{\"t\":\"time\",\"epoch\":1765584000123}", msg));
    TEST_ASSERT_EQUAL(LiveLink::MsgType::Time, msg.type);
    TEST_ASSERT_EQUAL_UINT64(1765584000123ULL, msg.epochMs);
}

void test_parse_caption_partial(void) {
    LiveLink::Msg msg;
    TEST_ASSERT_TRUE(LiveLink::parseMessage(
        "{\"t\":\"caption\",\"text\":\"hello there\",\"final\":false}", msg));
    TEST_ASSERT_EQUAL(LiveLink::MsgType::Caption, msg.type);
    TEST_ASSERT_FALSE(msg.isFinal);
    TEST_ASSERT_EQUAL_STRING("hello there", msg.text);
}

void test_parse_caption_final(void) {
    LiveLink::Msg msg;
    TEST_ASSERT_TRUE(LiveLink::parseMessage(
        "{\"t\":\"caption\",\"final\":true,\"text\":\"done.\"}", msg));
    TEST_ASSERT_TRUE(msg.isFinal);
    TEST_ASSERT_EQUAL_STRING("done.", msg.text);
}

void test_parse_caption_escapes(void) {
    LiveLink::Msg msg;
    TEST_ASSERT_TRUE(LiveLink::parseMessage(
        "{\"t\":\"caption\",\"text\":\"say \\\"hi\\\"\\nback\\\\slash\",\"final\":true}",
        msg));
    // \n becomes a space; quotes and backslash unescape literally.
    TEST_ASSERT_EQUAL_STRING("say \"hi\" back\\slash", msg.text);
}

void test_parse_caption_unicode_escape(void) {
    LiveLink::Msg msg;
    TEST_ASSERT_TRUE(LiveLink::parseMessage(
        "{\"t\":\"caption\",\"text\":\"caf\\u00e9 \\u0041\",\"final\":true}", msg));
    // Non-ASCII becomes '?', ASCII A decodes to 'A'.
    TEST_ASSERT_EQUAL_STRING("caf? A", msg.text);
}

void test_parse_caption_truncates_long_text(void) {
    char json[1024] = "{\"t\":\"caption\",\"text\":\"";
    size_t base = std::strlen(json);
    for (int i = 0; i < 400; ++i) json[base + i] = 'a' + (i % 26);
    json[base + 400] = '\0';
    std::strcat(json, "\",\"final\":true}");

    LiveLink::Msg msg;
    TEST_ASSERT_TRUE(LiveLink::parseMessage(json, msg));
    TEST_ASSERT_EQUAL(LiveLink::kMaxCaptionText, std::strlen(msg.text));
    TEST_ASSERT_TRUE(msg.isFinal);  // fields after the long string still parse
}

void test_parse_rejects_garbage(void) {
    LiveLink::Msg msg;
    TEST_ASSERT_FALSE(LiveLink::parseMessage("", msg));
    TEST_ASSERT_FALSE(LiveLink::parseMessage("not json", msg));
    TEST_ASSERT_FALSE(LiveLink::parseMessage("{\"t\":\"bogus\"}", msg));
    TEST_ASSERT_FALSE(LiveLink::parseMessage("{\"t\":\"time\"}", msg));       // no epoch
    TEST_ASSERT_FALSE(LiveLink::parseMessage("{\"t\":\"caption\"}", msg));    // no text
    TEST_ASSERT_FALSE(LiveLink::parseMessage(nullptr, msg));
}

void test_parse_caption_missing_final_defaults_partial(void) {
    LiveLink::Msg msg;
    TEST_ASSERT_TRUE(LiveLink::parseMessage(
        "{\"t\":\"caption\",\"text\":\"x\"}", msg));
    TEST_ASSERT_FALSE(msg.isFinal);
}

// ─── LiveLink builders ───

void test_build_hello(void) {
    char buf[128];
    size_t n = LiveLink::buildHello(buf, sizeof(buf), "1.2.2+abc123");
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_EQUAL_STRING(
        "{\"t\":\"hello\",\"rate\":16000,\"bits\":16,\"ch\":1,\"fw\":\"1.2.2+abc123\"}",
        buf);
}

void test_build_error_and_stop(void) {
    char buf[80];
    TEST_ASSERT_TRUE(LiveLink::buildError(buf, sizeof(buf), "busy") > 0);
    TEST_ASSERT_EQUAL_STRING("{\"t\":\"err\",\"reason\":\"busy\"}", buf);
    TEST_ASSERT_TRUE(LiveLink::buildStop(buf, sizeof(buf), "portal closed") > 0);
    TEST_ASSERT_EQUAL_STRING("{\"t\":\"stop\",\"reason\":\"portal closed\"}", buf);
}

void test_build_reports_overflow(void) {
    char tiny[8];
    TEST_ASSERT_EQUAL(0, LiveLink::buildHello(tiny, sizeof(tiny), "1.0.0"));
}

void test_frame_constants_pinned(void) {
    // The contract-pinned framing: 16kHz s16le mono, 5120-byte (160ms) frames.
    TEST_ASSERT_EQUAL_UINT32(16000, LiveLink::kPcmSampleRate);
    TEST_ASSERT_EQUAL_UINT8(16, LiveLink::kPcmBits);
    TEST_ASSERT_EQUAL_UINT8(1, LiveLink::kPcmChannels);
    TEST_ASSERT_EQUAL_UINT32(5120, LiveLink::kPcmFrameBytes);
    // 5120 bytes / 2 bytes-per-sample / 16000 Hz = 160 ms exactly.
    TEST_ASSERT_EQUAL_UINT32(
        160, (LiveLink::kPcmFrameBytes / 2) * 1000 / LiveLink::kPcmSampleRate);
}

// ─── CaptionWrap (fake fixed-width font: 6px per char) ───

static int fakeWidth(const char* s, void*) { return (int)std::strlen(s) * 6; }

void test_wrap_short_text_single_line(void) {
    char lines[4][CaptionWrap::kMaxLineChars + 1];
    int n = CaptionWrap::wrap("hello world", 120, fakeWidth, nullptr, lines, 4);
    TEST_ASSERT_EQUAL(1, n);
    TEST_ASSERT_EQUAL_STRING("hello world", lines[0]);
}

void test_wrap_breaks_at_words(void) {
    char lines[4][CaptionWrap::kMaxLineChars + 1];
    // 120px / 6px = 20 chars per line.
    int n = CaptionWrap::wrap("the quick brown fox jumps over the lazy dog",
                              120, fakeWidth, nullptr, lines, 4);
    TEST_ASSERT_EQUAL(3, n);
    TEST_ASSERT_EQUAL_STRING("the quick brown fox", lines[0]);
    TEST_ASSERT_EQUAL_STRING("jumps over the lazy", lines[1]);
    TEST_ASSERT_EQUAL_STRING("dog", lines[2]);
}

void test_wrap_hard_breaks_long_word(void) {
    char lines[4][CaptionWrap::kMaxLineChars + 1];
    int n = CaptionWrap::wrap("abcdefghijklmnopqrstuvwxyz", 60, fakeWidth,
                              nullptr, lines, 4);  // 10 chars per line
    TEST_ASSERT_EQUAL(3, n);
    TEST_ASSERT_EQUAL_STRING("abcdefghij", lines[0]);
    TEST_ASSERT_EQUAL_STRING("klmnopqrst", lines[1]);
    TEST_ASSERT_EQUAL_STRING("uvwxyz", lines[2]);
}

void test_wrap_keeps_tail_on_overflow(void) {
    char lines[2][CaptionWrap::kMaxLineChars + 1];
    int n = CaptionWrap::wrap("one two three four five six", 24, fakeWidth,
                              nullptr, lines, 2);  // 4 chars per line
    // Many lines wrapped, only the LAST two survive.
    TEST_ASSERT_EQUAL(2, n);
    TEST_ASSERT_EQUAL_STRING("five", lines[0]);
    TEST_ASSERT_EQUAL_STRING("six", lines[1]);
}

void test_wrap_collapses_spaces_and_empty(void) {
    char lines[4][CaptionWrap::kMaxLineChars + 1];
    TEST_ASSERT_EQUAL(0, CaptionWrap::wrap("", 120, fakeWidth, nullptr, lines, 4));
    TEST_ASSERT_EQUAL(0, CaptionWrap::wrap("   ", 120, fakeWidth, nullptr, lines, 4));
    TEST_ASSERT_EQUAL(0, CaptionWrap::wrap(nullptr, 120, fakeWidth, nullptr, lines, 4));
    int n = CaptionWrap::wrap("a    b", 120, fakeWidth, nullptr, lines, 4);
    TEST_ASSERT_EQUAL(1, n);
    TEST_ASSERT_EQUAL_STRING("a b", lines[0]);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_parse_time_message);
    RUN_TEST(test_parse_caption_partial);
    RUN_TEST(test_parse_caption_final);
    RUN_TEST(test_parse_caption_escapes);
    RUN_TEST(test_parse_caption_unicode_escape);
    RUN_TEST(test_parse_caption_truncates_long_text);
    RUN_TEST(test_parse_rejects_garbage);
    RUN_TEST(test_parse_caption_missing_final_defaults_partial);
    RUN_TEST(test_build_hello);
    RUN_TEST(test_build_error_and_stop);
    RUN_TEST(test_build_reports_overflow);
    RUN_TEST(test_frame_constants_pinned);
    RUN_TEST(test_wrap_short_text_single_line);
    RUN_TEST(test_wrap_breaks_at_words);
    RUN_TEST(test_wrap_hard_breaks_long_word);
    RUN_TEST(test_wrap_keeps_tail_on_overflow);
    RUN_TEST(test_wrap_collapses_spaces_and_empty);
    return UNITY_END();
}
