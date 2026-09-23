// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#include "TrustedRoots.h"

#include <cstring>

#ifndef HOST_TEST
#include <esp_heap_caps.h>
#include <mbedtls/x509_crt.h>
#endif

namespace TrustedRoots {
namespace {

constexpr char kBegin[] = "-----BEGIN CERTIFICATE-----\n";
constexpr char kEnd[] = "-----END CERTIFICATE-----\n";
constexpr size_t kLineChars = 64;
constexpr char kAlphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

size_t blockSize(size_t derLen) {
    const size_t b64 = (derLen + 2) / 3 * 4;
    const size_t newlines = (b64 + kLineChars - 1) / kLineChars;
    return (sizeof(kBegin) - 1) + b64 + newlines + (sizeof(kEnd) - 1);
}

}  // namespace

size_t pemSize() {
    size_t size = 1;  // NUL
    for (size_t i = 0; i < kRootCount; ++i) size += blockSize(kRoots[i].derLen);
    return size;
}

size_t writePem(char* out, size_t cap) {
    if (!out || cap < pemSize()) return 0;
    char* p = out;
    for (size_t r = 0; r < kRootCount; ++r) {
        const uint8_t* der = kRoots[r].der;
        const size_t len = kRoots[r].derLen;
        memcpy(p, kBegin, sizeof(kBegin) - 1);
        p += sizeof(kBegin) - 1;
        size_t column = 0;
        for (size_t i = 0; i < len; i += 3) {
            uint32_t v = (uint32_t)der[i] << 16;
            if (i + 1 < len) v |= (uint32_t)der[i + 1] << 8;
            if (i + 2 < len) v |= der[i + 2];
            const char quad[4] = {
                kAlphabet[(v >> 18) & 63],
                kAlphabet[(v >> 12) & 63],
                i + 1 < len ? kAlphabet[(v >> 6) & 63] : '=',
                i + 2 < len ? kAlphabet[v & 63] : '=',
            };
            for (char c : quad) {
                *p++ = c;
                if (++column == kLineChars) {
                    *p++ = '\n';
                    column = 0;
                }
            }
        }
        if (column) *p++ = '\n';
        memcpy(p, kEnd, sizeof(kEnd) - 1);
        p += sizeof(kEnd) - 1;
    }
    *p = '\0';
    return (size_t)(p - out);
}

bool parseComplete(int parseResult, size_t parsedCount) {
    return parseResult == 0 && parsedCount == kRootCount;
}

#ifndef HOST_TEST
bool pemParsesCompletely(const char* pem) {
    if (!pem) return false;
    mbedtls_x509_crt chain;
    mbedtls_x509_crt_init(&chain);
    const int rc = mbedtls_x509_crt_parse(&chain, (const unsigned char*)pem, strlen(pem) + 1);
    const bool ok = parseComplete(rc, chainCount(&chain));
    mbedtls_x509_crt_free(&chain);
    return ok;
}

char* newPem() {
    const size_t size = pemSize();
    char* pem = (char*)heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!pem) pem = (char*)heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (pem && writePem(pem, size) == 0) {
        heap_caps_free(pem);
        pem = nullptr;
    }
    return pem;
}

void freePem(char* pem) {
    heap_caps_free(pem);
}
#endif

}  // namespace TrustedRoots
