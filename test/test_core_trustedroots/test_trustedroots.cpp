// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

// The generated trusted root table: every entry is exactly one DER
// certificate, hashes to the SHA-256 recorded from roots/roots.json, and the
// PEM text handed to the HTTP client decodes back to the same bytes.

#include <unity.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "TrustedRoots.h"

using TrustedRoots::kRootCount;
using TrustedRoots::kRoots;

void setUp(void) {}
void tearDown(void) {}

namespace {

// --- SHA-256 (FIPS 180-4), host-only reference for the fingerprint check ---
const uint32_t kK[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

std::string sha256Hex(const uint8_t* data, size_t len) {
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                     0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::vector<uint8_t> msg(data, data + len);
    msg.push_back(0x80);
    while (msg.size() % 64 != 56) msg.push_back(0);
    const uint64_t bits = (uint64_t)len * 8;
    for (int i = 7; i >= 0; --i) msg.push_back((uint8_t)(bits >> (i * 8)));
    for (size_t off = 0; off < msg.size(); off += 64) {
        uint32_t w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = (uint32_t)msg[off + i * 4] << 24 | (uint32_t)msg[off + i * 4 + 1] << 16 |
                   (uint32_t)msg[off + i * 4 + 2] << 8 | msg[off + i * 4 + 3];
        for (int i = 16; i < 64; ++i) {
            uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            uint32_t t1 = hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + kK[i] + w[i];
            uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    char hex[65];
    for (int i = 0; i < 8; ++i) snprintf(hex + i * 8, 9, "%08x", h[i]);
    return std::string(hex, 64);
}

// --- DER length walk ---
// Reads one TLV at der[pos]; returns the offset just past it, or 0 on error.
size_t tlvEnd(const uint8_t* der, size_t len, size_t pos, uint8_t* tag, size_t* bodyStart) {
    if (pos + 2 > len) return 0;
    *tag = der[pos];
    size_t n = der[pos + 1];
    size_t p = pos + 2;
    if (n & 0x80) {
        const size_t count = n & 0x7f;
        if (count == 0 || count > 3 || p + count > len) return 0;
        n = 0;
        for (size_t i = 0; i < count; ++i) n = (n << 8) | der[p++];
    }
    if (p + n > len) return 0;
    *bodyStart = p;
    return p + n;
}

// --- base64 decode for the PEM round trip ---
int b64Value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

bool b64Decode(const std::string& text, std::vector<uint8_t>& out) {
    uint32_t acc = 0;
    int bits = 0;
    for (char c : text) {
        if (c == '\n' || c == '=') continue;
        const int v = b64Value(c);
        if (v < 0) return false;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back((uint8_t)(acc >> bits));
        }
    }
    return true;
}

std::string pemText() {
    std::vector<char> buf(TrustedRoots::pemSize());
    const size_t n = TrustedRoots::writePem(buf.data(), buf.size());
    return std::string(buf.data(), n);
}

}  // namespace

void test_sha256_reference_vector(void) {
    // FIPS 180-2 example: SHA-256("abc").
    TEST_ASSERT_EQUAL_STRING(
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
        sha256Hex((const uint8_t*)"abc", 3).c_str());
}

void test_table_is_populated(void) {
    TEST_ASSERT_GREATER_THAN(0u, kRootCount);
    for (size_t i = 0; i < kRootCount; ++i) {
        TEST_ASSERT_NOT_NULL(kRoots[i].name);
        TEST_ASSERT_NOT_NULL(kRoots[i].der);
        TEST_ASSERT_GREATER_THAN(0u, kRoots[i].derLen);
        TEST_ASSERT_EQUAL_size_t(64, strlen(kRoots[i].sha256));
        for (size_t j = 0; j < i; ++j)
            TEST_ASSERT_FALSE_MESSAGE(strcmp(kRoots[i].sha256, kRoots[j].sha256) == 0,
                                      kRoots[i].name);
    }
}

void test_each_root_is_exactly_one_der_certificate(void) {
    for (size_t i = 0; i < kRootCount; ++i) {
        const uint8_t* der = kRoots[i].der;
        const size_t len = kRoots[i].derLen;
        uint8_t tag = 0;
        size_t body = 0;
        // Certificate ::= SEQUENCE { tbsCertificate, signatureAlgorithm, signatureValue }
        TEST_ASSERT_EQUAL_size_t_MESSAGE(len, tlvEnd(der, len, 0, &tag, &body), kRoots[i].name);
        TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x30, tag, kRoots[i].name);
        size_t pos = body;
        const uint8_t expected[3] = {0x30, 0x30, 0x03};
        for (uint8_t want : expected) {
            size_t inner = 0;
            pos = tlvEnd(der, len, pos, &tag, &inner);
            TEST_ASSERT_NOT_EQUAL_MESSAGE(0, pos, kRoots[i].name);
            TEST_ASSERT_EQUAL_HEX8_MESSAGE(want, tag, kRoots[i].name);
        }
        TEST_ASSERT_EQUAL_size_t_MESSAGE(len, pos, kRoots[i].name);
    }
}

void test_each_root_matches_its_fingerprint(void) {
    for (size_t i = 0; i < kRootCount; ++i) {
        TEST_ASSERT_EQUAL_STRING_MESSAGE(kRoots[i].sha256,
                                         sha256Hex(kRoots[i].der, kRoots[i].derLen).c_str(),
                                         kRoots[i].name);
    }
}

void test_pem_size_is_exact_and_short_buffer_is_refused(void) {
    const size_t size = TrustedRoots::pemSize();
    std::vector<char> buf(size);
    TEST_ASSERT_EQUAL_size_t(size - 1, TrustedRoots::writePem(buf.data(), size));
    TEST_ASSERT_EQUAL_size_t(size - 1, strlen(buf.data()));
    TEST_ASSERT_EQUAL_size_t(0, TrustedRoots::writePem(buf.data(), size - 1));
    TEST_ASSERT_EQUAL_size_t(0, TrustedRoots::writePem(nullptr, size));
}

void test_pem_decodes_back_to_each_root(void) {
    const std::string pem = pemText();
    const std::string begin = "-----BEGIN CERTIFICATE-----\n";
    const std::string end = "-----END CERTIFICATE-----\n";
    size_t pos = 0;
    for (size_t i = 0; i < kRootCount; ++i) {
        TEST_ASSERT_EQUAL_size_t_MESSAGE(pos, pem.find(begin, pos), kRoots[i].name);
        const size_t bodyStart = pos + begin.size();
        const size_t bodyEnd = pem.find(end, bodyStart);
        TEST_ASSERT_NOT_EQUAL_MESSAGE(std::string::npos, bodyEnd, kRoots[i].name);
        const std::string body = pem.substr(bodyStart, bodyEnd - bodyStart);
        size_t lineStart = 0;
        while (lineStart < body.size()) {
            const size_t nl = body.find('\n', lineStart);
            TEST_ASSERT_NOT_EQUAL(std::string::npos, nl);
            TEST_ASSERT_LESS_OR_EQUAL_size_t(64, nl - lineStart);
            lineStart = nl + 1;
        }
        std::vector<uint8_t> der;
        TEST_ASSERT_TRUE(b64Decode(body, der));
        TEST_ASSERT_EQUAL_size_t_MESSAGE(kRoots[i].derLen, der.size(), kRoots[i].name);
        TEST_ASSERT_EQUAL_MEMORY_MESSAGE(kRoots[i].der, der.data(), der.size(), kRoots[i].name);
        pos = bodyEnd + end.size();
    }
    TEST_ASSERT_EQUAL_size_t(pem.size(), pos);
}

// Stand-in for mbedtls_x509_crt: only the fields chainCount() walks.
struct FakeCrt {
    int version;
    FakeCrt* next;
};

void test_chain_count_skips_empty_nodes(void) {
    TEST_ASSERT_EQUAL_size_t(0, TrustedRoots::chainCount<FakeCrt>(nullptr));
    FakeCrt empty = {0, nullptr};  // what mbedtls_x509_crt_init leaves
    TEST_ASSERT_EQUAL_size_t(0, TrustedRoots::chainCount(&empty));
    FakeCrt third = {3, nullptr};
    FakeCrt second = {3, &third};
    FakeCrt first = {3, &second};
    TEST_ASSERT_EQUAL_size_t(3, TrustedRoots::chainCount(&first));
    FakeCrt tail = {0, nullptr};
    third.next = &tail;
    TEST_ASSERT_EQUAL_size_t(3, TrustedRoots::chainCount(&first));
}

void test_parse_complete_needs_zero_and_every_root(void) {
    TEST_ASSERT_TRUE(TrustedRoots::parseComplete(0, kRootCount));
    // mbedtls_x509_crt_parse returns the number of certificates it skipped.
    TEST_ASSERT_FALSE(TrustedRoots::parseComplete(1, kRootCount - 1));
    TEST_ASSERT_FALSE(TrustedRoots::parseComplete(1, kRootCount));
    TEST_ASSERT_FALSE(TrustedRoots::parseComplete(0, kRootCount - 1));
    TEST_ASSERT_FALSE(TrustedRoots::parseComplete(0, kRootCount + 1));
    TEST_ASSERT_FALSE(TrustedRoots::parseComplete(-0x2180, 0));  // hard parse error
    TEST_ASSERT_FALSE(TrustedRoots::parseComplete(0, 0));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_sha256_reference_vector);
    RUN_TEST(test_table_is_populated);
    RUN_TEST(test_each_root_is_exactly_one_der_certificate);
    RUN_TEST(test_each_root_matches_its_fingerprint);
    RUN_TEST(test_pem_size_is_exact_and_short_buffer_is_refused);
    RUN_TEST(test_pem_decodes_back_to_each_root);
    RUN_TEST(test_chain_count_skips_empty_nodes);
    RUN_TEST(test_parse_complete_needs_zero_and_every_root);
    return UNITY_END();
}
