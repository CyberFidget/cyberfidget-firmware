// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2026 Dismo Industries LLC

// The portal's catch-all DNS answer (CaptiveDns.h) and the rule for when the
// portal screen shows its address (OpenAddressHint.h). Pure, no device.

#include <unity.h>
#include <cstring>
#include <vector>
#include "CaptiveDns.h"
#include "OpenAddressHint.h"

static const uint8_t kIp[4] = {192, 168, 4, 1};

// A standard query for `name`, type `type`, optionally with an EDNS(0) OPT
// additional record (what Android, Apple and browser resolvers send).
static std::vector<uint8_t> query(const char* name, uint16_t type, bool edns,
                                  uint16_t id = 0xBEEF) {
    std::vector<uint8_t> q = {(uint8_t)(id >> 8), (uint8_t)id, 0x01, 0x00,
                              0, 1, 0, 0, 0, 0, 0, (uint8_t)(edns ? 1 : 0)};
    const char* p = name;
    while (*p) {
        const char* dot = strchr(p, '.');
        size_t n = dot ? (size_t)(dot - p) : strlen(p);
        q.push_back((uint8_t)n);
        q.insert(q.end(), p, p + n);
        p += n;
        if (*p == '.') p++;
    }
    q.push_back(0);
    q.push_back((uint8_t)(type >> 8)); q.push_back((uint8_t)type);
    q.push_back(0); q.push_back(1);
    if (edns) {
        const uint8_t opt[] = {0, 0, 41, 0x10, 0, 0, 0, 0x80, 0, 0, 0};
        q.insert(q.end(), opt, opt + sizeof(opt));
    }
    return q;
}

static uint16_t u16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }

void test_a_query_answers_the_fidget(void) {
    auto q = query("www.msftconnecttest.com", CaptiveDns::kTypeA, false);
    uint8_t out[CaptiveDns::kMaxReplyBytes];
    CaptiveDns::Question qq;
    size_t n = CaptiveDns::buildReply(q.data(), q.size(), kIp, out, &qq);
    TEST_ASSERT_EQUAL_STRING("www.msftconnecttest.com", qq.name);
    TEST_ASSERT_EQUAL_UINT16(0xBEEF, u16(out));
    TEST_ASSERT_TRUE(out[2] & 0x80);             // a response
    TEST_ASSERT_TRUE(out[2] & 0x01);             // RD echoed
    TEST_ASSERT_EQUAL_UINT8(0, out[3] & 0x0F);   // no error
    TEST_ASSERT_EQUAL_UINT16(1, u16(out + 4));
    TEST_ASSERT_EQUAL_UINT16(1, u16(out + 6));
    TEST_ASSERT_EQUAL_UINT16(0, u16(out + 10));
    TEST_ASSERT_EQUAL_size_t(q.size() + 16, n);
    TEST_ASSERT_EQUAL_UINT16(CaptiveDns::kTypeA, u16(out + q.size() + 2));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(kIp, out + n - 4, 4);
}

// The framework DNSServer answered "no such name" to these.
void test_edns_query_still_answers_the_fidget(void) {
    auto q = query("connectivitycheck.gstatic.com", CaptiveDns::kTypeA, true);
    uint8_t out[CaptiveDns::kMaxReplyBytes];
    size_t n = CaptiveDns::buildReply(q.data(), q.size(), kIp, out);
    TEST_ASSERT_EQUAL_UINT8(0, out[3] & 0x0F);
    TEST_ASSERT_EQUAL_UINT16(1, u16(out + 6));
    TEST_ASSERT_EQUAL_UINT16(0, u16(out + 10));  // OPT not echoed
    // Reply = question section (without the 11-byte OPT) + one A answer.
    TEST_ASSERT_EQUAL_size_t(q.size() - 11 + 16, n);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(kIp, out + n - 4, 4);
}

// The framework DNSServer answered these with an A record.
void test_other_types_get_an_empty_answer(void) {
    const uint16_t types[] = {28 /*AAAA*/, 65 /*HTTPS*/, 16 /*TXT*/};
    for (uint16_t t : types) {
        auto q = query("captive.apple.com", t, true);
        uint8_t out[CaptiveDns::kMaxReplyBytes];
        size_t n = CaptiveDns::buildReply(q.data(), q.size(), kIp, out);
        TEST_ASSERT_EQUAL_UINT8(0, out[3] & 0x0F);  // no error: the name exists
        TEST_ASSERT_EQUAL_UINT16(0, u16(out + 6));  // ...with no records of this type
        TEST_ASSERT_EQUAL_size_t(q.size() - 11, n);
    }
}

void test_malformed_or_non_queries_get_nothing(void) {
    uint8_t out[CaptiveDns::kMaxReplyBytes];
    auto q = query("example.com", CaptiveDns::kTypeA, false);
    TEST_ASSERT_EQUAL_size_t(0, CaptiveDns::buildReply(q.data(), 10, kIp, out));
    auto r = q; r[2] |= 0x80;                       // a response
    TEST_ASSERT_EQUAL_size_t(0, CaptiveDns::buildReply(r.data(), r.size(), kIp, out));
    auto two = q; two[5] = 2;                       // two questions
    TEST_ASSERT_EQUAL_size_t(0, CaptiveDns::buildReply(two.data(), two.size(), kIp, out));
    auto cut = q; cut.resize(q.size() - 3);         // truncated type/class
    TEST_ASSERT_EQUAL_size_t(0, CaptiveDns::buildReply(cut.data(), cut.size(), kIp, out));
    auto ptr = q; ptr[12] = 0xC0;                   // compressed name in a question
    TEST_ASSERT_EQUAL_size_t(0, CaptiveDns::buildReply(ptr.data(), ptr.size(), kIp, out));
    auto upd = q; upd[2] = (uint8_t)(5 << 3);       // UPDATE opcode
    TEST_ASSERT_EQUAL_size_t(0, CaptiveDns::buildReply(upd.data(), upd.size(), kIp, out));
}

void test_hint_waits_ten_seconds_after_a_join(void) {
    OpenAddressHint h;
    h.onStations(0, 0);
    TEST_ASSERT_FALSE(h.show(60000));               // nobody joined
    h.onStations(1, 1000);
    TEST_ASSERT_FALSE(h.show(1000 + OpenAddressHint::kDelayMs - 1));
    TEST_ASSERT_TRUE(h.show(1000 + OpenAddressHint::kDelayMs));
    h.onStations(1, 20000);                          // same join, time not restarted
    TEST_ASSERT_TRUE(h.show(20000));
}

void test_hint_never_shows_once_the_page_opened(void) {
    OpenAddressHint h;
    h.onStations(1, 0);
    h.onPageRequest();
    TEST_ASSERT_FALSE(h.show(OpenAddressHint::kDelayMs * 3));
    // It shows after the fact too: the page clears it.
    OpenAddressHint late;
    late.onStations(1, 0);
    TEST_ASSERT_TRUE(late.show(OpenAddressHint::kDelayMs));
    late.onPageRequest();
    TEST_ASSERT_FALSE(late.show(OpenAddressHint::kDelayMs + 1));
}

void test_hint_rearms_for_a_new_join(void) {
    OpenAddressHint h;
    h.onStations(1, 0);
    h.onPageRequest();
    h.onStations(0, 5000);                           // everyone left
    TEST_ASSERT_FALSE(h.show(6000));
    h.onStations(1, 7000);                           // a new device joins
    TEST_ASSERT_FALSE(h.show(7000 + OpenAddressHint::kDelayMs - 1));
    TEST_ASSERT_TRUE(h.show(7000 + OpenAddressHint::kDelayMs));
    h.reset();
    TEST_ASSERT_FALSE(h.show(100000));
}

// The page can be fetched between two station polls: the device joins, the
// sign-in page loads, and only then does the main loop see the station.
// That page must still count.
void test_page_before_the_first_poll_after_a_join_counts(void) {
    OpenAddressHint h;
    h.onStations(0, 0);                              // nobody on the network
    h.onPageRequest();                               // joined + fetched between polls
    h.onStations(1, 200);                            // first poll to see the station
    TEST_ASSERT_FALSE(h.show(200 + OpenAddressHint::kDelayMs * 2));
    // Also when no empty poll ever happened (page before the first poll at all).
    OpenAddressHint first;
    first.reset();
    first.onPageRequest();
    first.onStations(1, 0);
    TEST_ASSERT_FALSE(first.show(OpenAddressHint::kDelayMs * 2));
    // A later join still needs its own page.
    first.onStations(0, 30000);
    first.onStations(1, 30200);
    TEST_ASSERT_TRUE(first.show(30200 + OpenAddressHint::kDelayMs));
}

void test_hint_survives_millis_wrap(void) {
    OpenAddressHint h;
    h.onStations(1, 0xFFFFF000u);
    TEST_ASSERT_FALSE(h.show(0xFFFFF000u + 100));
    TEST_ASSERT_TRUE(h.show(0xFFFFF000u + OpenAddressHint::kDelayMs));  // wraps past 0
}

// While due, the address takes turns with the usual line (the way out), and
// once the page opens the usual line stays.
void test_address_takes_turns_with_the_usual_line(void) {
    const uint32_t d = OpenAddressHint::kDelayMs, a = OpenAddressHint::kAlternateMs;
    OpenAddressHint h;
    h.onStations(1, 1000);
    TEST_ASSERT_FALSE(h.showAddressNow(1000 + d - 1));      // not yet due
    TEST_ASSERT_TRUE(h.showAddressNow(1000 + d));           // address first
    TEST_ASSERT_TRUE(h.showAddressNow(1000 + d + a - 1));
    TEST_ASSERT_FALSE(h.showAddressNow(1000 + d + a));      // usual line's turn
    TEST_ASSERT_FALSE(h.showAddressNow(1000 + d + 2 * a - 1));
    TEST_ASSERT_TRUE(h.showAddressNow(1000 + d + 2 * a));   // and back
    h.onPageRequest();
    TEST_ASSERT_FALSE(h.showAddressNow(1000 + d + 4 * a));  // page opened: usual line only
}

void setUp(void) {}
void tearDown(void) {}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_a_query_answers_the_fidget);
    RUN_TEST(test_edns_query_still_answers_the_fidget);
    RUN_TEST(test_other_types_get_an_empty_answer);
    RUN_TEST(test_malformed_or_non_queries_get_nothing);
    RUN_TEST(test_hint_waits_ten_seconds_after_a_join);
    RUN_TEST(test_hint_never_shows_once_the_page_opened);
    RUN_TEST(test_hint_rearms_for_a_new_join);
    RUN_TEST(test_page_before_the_first_poll_after_a_join_counts);
    RUN_TEST(test_hint_survives_millis_wrap);
    RUN_TEST(test_address_takes_turns_with_the_usual_line);
    return UNITY_END();
}
