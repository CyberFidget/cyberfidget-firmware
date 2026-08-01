// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

// lib/SyncProtocol/SyncProtocol.cpp - see SyncProtocol.h for the API and
// README.md for the wire framing. Pure C++17, no Arduino.

#include "SyncProtocol.h"

#include <cstdio>
#include <cstring>

namespace SyncProtocol {

// ============================================================
// CRC-32 (reflected, poly 0xEDB88320). The constexpr lookup table stays in
// flash and requires no runtime initialization.
// ============================================================
namespace {

struct Crc32Table {
    uint32_t v[256];

    constexpr Crc32Table() : v() {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) {
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            }
            v[i] = c;
        }
    }
};

constexpr Crc32Table kCrc32Table;

// Roots a write/delete path must live under. A path outside these is
// refused before the filesystem is ever touched.
const char* const kAllowedRoots[] = { "/apps/", "/assets/" };

bool startsWith(const char* s, const char* prefix) {
    return std::strncmp(s, prefix, std::strlen(prefix)) == 0;
}

// Skip a run of spaces.
void skipSpaces(const char*& p) {
    while (*p == ' ') p++;
}

// Copy a non-space token into out (null-terminated). Fails on empty token
// or a token that doesn't fit. Advances p past the token.
bool takeToken(const char*& p, char* out, size_t cap) {
    skipSpaces(p);
    size_t n = 0;
    while (*p && *p != ' ') {
        if (n + 1 >= cap) return false; // no room for token + null
        out[n++] = *p++;
    }
    if (n == 0) return false; // empty token
    out[n] = '\0';
    return true;
}

// Parse a decimal uint32 token. Advances p. Rejects empty / overflow /
// trailing non-space garbage on the token.
bool takeU32(const char*& p, uint32_t& out) {
    skipSpaces(p);
    if (*p < '0' || *p > '9') return false;
    uint64_t v = 0;
    while (*p >= '0' && *p <= '9') {
        v = v * 10 + (uint64_t)(*p - '0');
        if (v > 0xFFFFFFFFull) return false; // overflow
        p++;
    }
    if (*p != '\0' && *p != ' ') return false; // junk glued to the number
    out = (uint32_t)v;
    return true;
}

// Parse a fixed-form hex uint32 token (1..8 hex digits). Advances p.
bool takeHex32(const char*& p, uint32_t& out) {
    skipSpaces(p);
    uint32_t v = 0;
    int digits = 0;
    while (true) {
        char h = *p;
        uint32_t d;
        if      (h >= '0' && h <= '9') d = (uint32_t)(h - '0');
        else if (h >= 'a' && h <= 'f') d = (uint32_t)(h - 'a' + 10);
        else if (h >= 'A' && h <= 'F') d = (uint32_t)(h - 'A' + 10);
        else break;
        if (digits >= 8) return false; // more than 32 bits
        v = (v << 4) | d;
        digits++;
        p++;
    }
    if (digits == 0) return false;
    if (*p != '\0' && *p != ' ') return false;
    out = v;
    return true;
}

// True once the cursor is at end-of-string (only trailing spaces allowed).
bool atEnd(const char* p) {
    while (*p == ' ') p++;
    return *p == '\0';
}

} // namespace

uint32_t crc32Begin() { return 0xFFFFFFFFu; }

uint32_t crc32Update(uint32_t crc, const void* data, size_t len) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < len; i++) {
        crc = kCrc32Table.v[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc;
}

uint32_t crc32Finish(uint32_t crc) { return crc ^ 0xFFFFFFFFu; }

uint32_t crc32(const void* data, size_t len) {
    return crc32Finish(crc32Update(crc32Begin(), data, len));
}

bool pathConfined(const char* path) {
    if (!path) return false;
    size_t len = std::strlen(path);
    if (len == 0 || len > kMaxPathLen) return false;
    if (path[0] != '/') return false;
    if (path[len - 1] == '/') return false; // must name a file, not a dir

    // Must live under one of the allowed roots.
    bool rooted = false;
    for (const char* root : kAllowedRoots) {
        if (startsWith(path, root)) { rooted = true; break; }
    }
    if (!rooted) return false;

    // Byte-level hygiene + traversal check. Reject any control byte or space,
    // and any "." / ".." segment (a "." segment is harmless but never
    // legitimate here, so refuse it too and keep the rule simple).
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)path[i];
        if (c < 0x20 || c == ' ' || c == 0x7F) return false;
    }
    // Scan segments delimited by '/'.
    size_t i = 0;
    while (i < len) {
        // i is at a '/'; find the segment [start, end).
        size_t start = i + 1;
        size_t end = start;
        while (end < len && path[end] != '/') end++;
        size_t segLen = end - start;
        if (segLen == 1 && path[start] == '.') return false;       // "."
        if (segLen == 2 && path[start] == '.' && path[start + 1] == '.') return false; // ".."
        if (segLen == 0 && start < len) return false;              // "//"
        i = end;
    }
    return true;
}

bool parseWriteOpen(const char* args, char* pathOut, size_t pathCap,
                    uint32_t& sizeOut, uint32_t& crcOut) {
    if (!args) return false;
    const char* p = args;
    if (!takeToken(p, pathOut, pathCap)) return false;
    if (!takeU32(p, sizeOut)) return false;
    if (!takeHex32(p, crcOut)) return false;
    return atEnd(p);
}

bool parseChunkHeader(const char* args, uint32_t& offsetOut,
                      uint32_t& lenOut, uint32_t& crcOut) {
    if (!args) return false;
    const char* p = args;
    if (!takeU32(p, offsetOut)) return false;
    if (!takeU32(p, lenOut)) return false;
    if (!takeHex32(p, crcOut)) return false;
    return atEnd(p);
}

bool parseApplyHeader(const char* args, uint32_t& lenOut, uint32_t& crcOut) {
    if (!args) return false;
    const char* p = args;
    if (!takeU32(p, lenOut)) return false;
    if (!takeHex32(p, crcOut)) return false;
    return atEnd(p);
}

bool parsePathArg(const char* args, char* pathOut, size_t pathCap) {
    if (!args) return false;
    const char* p = args;
    if (!takeToken(p, pathOut, pathCap)) return false;
    return atEnd(p);
}

bool parseListArgs(const char* args, char* dirOut, size_t dirCap) {
    return parsePathArg(args, dirOut, dirCap);
}

bool parseStatArgs(const char* args, char* pathOut, size_t pathCap) {
    return parsePathArg(args, pathOut, pathCap);
}

bool parseReadArgs(const char* args, char* pathOut, size_t pathCap,
                   uint32_t& offsetOut, uint32_t& lenOut) {
    if (!args) return false;
    const char* p = args;
    if (!takeToken(p, pathOut, pathCap)) return false;
    if (!takeU32(p, offsetOut)) return false;
    if (!takeU32(p, lenOut)) return false;
    return atEnd(p);
}

bool makeListConfinementProbe(const char* dir, char* pathOut, size_t pathCap) {
    if (!dir || !pathOut || pathCap == 0) return false;
    const size_t len = std::strlen(dir);
    if (len == 0 || dir[len - 1] == '/') return false;
    // "/_" makes a file-shaped child without changing or interpreting any
    // directory byte. pathConfined() remains the sole security predicate.
    if (len + 3 > pathCap) return false;
    std::memcpy(pathOut, dir, len);
    pathOut[len] = '/';
    pathOut[len + 1] = '_';
    pathOut[len + 2] = '\0';
    return true;
}

bool readLengthAllowed(uint32_t len) {
    return len > 0 && len <= kMaxChunkBytes;
}

size_t formatReadHeader(char* out, size_t cap, const char* path,
                        uint32_t offset, uint32_t len, uint32_t crc) {
    if (!out || cap == 0 || !path) return 0;
    const int n = std::snprintf(
        out, cap, "[cmd] fread.ok=%s off=%u len=%u chunk=%u crc=%08x\n",
        path, (unsigned)offset, (unsigned)len, (unsigned)kMaxChunkBytes,
        (unsigned)crc);
    if (n < 0 || (size_t)n >= cap) {
        out[0] = '\0';
        return 0;
    }
    return (size_t)n;
}

bool admitListEntry(ListProgress& progress) {
    if (progress.entries >= kMaxListEntries) {
        progress.truncated = true;
        return false;
    }
    progress.entries++;
    return true;
}

size_t formatListSummary(char* out, size_t cap, const char* dir,
                         const ListProgress& progress) {
    if (!out || cap == 0 || !dir) return 0;
    const int n = std::snprintf(
        out, cap, "[cmd] flist.done=%s entries=%u truncated=%u max=%u\n",
        dir, (unsigned)progress.entries, progress.truncated ? 1u : 0u,
        (unsigned)kMaxListEntries);
    if (n < 0 || (size_t)n >= cap) {
        out[0] = '\0';
        return 0;
    }
    return (size_t)n;
}

} // namespace SyncProtocol
