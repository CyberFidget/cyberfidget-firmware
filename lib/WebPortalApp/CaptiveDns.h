// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2026 Dismo Industries LLC

#ifndef CAPTIVE_DNS_H
#define CAPTIVE_DNS_H

// The portal's catch-all DNS answer, pure (no Arduino) so it is unit-tested
// natively. Every name resolves to the Fidget, which is what sends a
// joining phone or laptop's network check to the portal.
//
// Why not the framework's DNSServer: it answers only queries with no
// additional records, so a query carrying an EDNS(0) OPT record (sent by
// Android, Apple and many browsers' own resolvers) got "no such name"; and
// it answered EVERY query type with an A record, so an AAAA or HTTPS query
// got a mismatched answer. Here, EDNS or not: an A query - and an ANY
// query, answered with just the A record - gets the Fidget's address; every
// other type (AAAA, HTTPS, TXT, ...) gets an empty "no records of that type"
// answer.

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace CaptiveDns {

constexpr uint16_t kTypeA = 1;
constexpr uint16_t kTypeAny = 255;
constexpr uint16_t kClassIn = 1;
constexpr uint32_t kTtlSeconds = 10;  // short: nothing cached past the portal
constexpr size_t kHeaderBytes = 12;
constexpr size_t kMaxReplyBytes = 512;

struct Question {
    char name[254];  // dotted, lower-case as sent; "" for the root
    uint16_t type;
    uint16_t qclass;
};

inline uint16_t rd16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }
inline void wr16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }

// Builds the reply to `query` into `out` (at least kMaxReplyBytes). `ip` is
// the address to answer with, most significant byte first (192.168.4.1 =
// {192,168,4,1}). Returns the reply length, or 0 to send nothing (not a
// standard query with exactly one well-formed question). `q` receives the
// question when non-null (for test-build logging).
inline size_t buildReply(const uint8_t* query, size_t len, const uint8_t ip[4],
                         uint8_t* out, Question* q = nullptr) {
    if (!query || !out || len < kHeaderBytes + 5) return 0;
    const uint8_t flags1 = query[2];
    if (flags1 & 0x80) return 0;                 // a response, not a query
    if (((flags1 >> 3) & 0x0F) != 0) return 0;   // not a standard query
    if (rd16(query + 4) != 1) return 0;          // exactly one question
    // Question name: uncompressed labels ending in a zero byte.
    size_t pos = kHeaderBytes;
    size_t nameLen = 0;
    char name[254];
    for (;;) {
        if (pos >= len) return 0;
        const uint8_t label = query[pos++];
        if (label == 0) break;
        if (label > 63 || pos + label > len) return 0;
        if (nameLen + label + 1 >= sizeof(name)) return 0;
        if (nameLen) name[nameLen++] = '.';
        memcpy(name + nameLen, query + pos, label);
        nameLen += label;
        pos += label;
    }
    name[nameLen] = '\0';
    if (pos + 4 > len) return 0;
    const uint16_t type = rd16(query + pos);
    const uint16_t qclass = rd16(query + pos + 2);
    const size_t questionEnd = pos + 4;
    if (questionEnd + 16 > kMaxReplyBytes) return 0;
    if (q) {
        memcpy(q->name, name, nameLen + 1);
        q->type = type;
        q->qclass = qclass;
    }

    const bool answer = (type == kTypeA || type == kTypeAny) && qclass == kClassIn;
    // Header: same id, QR + AA, the client's RD, no error; one question,
    // one or zero answers, nothing else (any EDNS record is not echoed).
    memcpy(out, query, kHeaderBytes);
    out[2] = (uint8_t)(0x80 | 0x04 | (flags1 & 0x01));
    out[3] = 0;
    wr16(out + 4, 1);
    wr16(out + 6, answer ? 1 : 0);
    wr16(out + 8, 0);
    wr16(out + 10, 0);
    memcpy(out + kHeaderBytes, query + kHeaderBytes, questionEnd - kHeaderBytes);
    size_t n = questionEnd;
    if (answer) {
        out[n++] = 0xC0;                  // name: pointer to the question
        out[n++] = (uint8_t)kHeaderBytes;
        wr16(out + n, kTypeA); n += 2;
        wr16(out + n, kClassIn); n += 2;
        out[n++] = 0; out[n++] = 0;
        wr16(out + n, (uint16_t)kTtlSeconds); n += 2;
        wr16(out + n, 4); n += 2;
        memcpy(out + n, ip, 4); n += 4;
    }
    return n;
}

}  // namespace CaptiveDns

#endif
