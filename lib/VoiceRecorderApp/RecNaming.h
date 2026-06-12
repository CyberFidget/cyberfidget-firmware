// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

// lib/VoiceRecorderApp/RecNaming.h
//
// Pure naming + index.csv row logic for the voice recorder. No Arduino or
// ESP types so it compiles host-side for the native test env
// (test_voicerecorder). File existence is injected as a predicate so the
// collision-skip logic is testable without a filesystem.
//
// Header-only because the native test envs build with lib_ldf_mode = off,
// which won't compile lib .cpp files into the test binary.

#ifndef REC_NAMING_H
#define REC_NAMING_H

#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace RecNaming {

constexpr const char* kRecordingsDir = "/recordings";
constexpr uint32_t kCounterExhausted = 0xFFFFFFFFu;

// Max voice-note filename length INCLUDING the ".wav" extension. The recorder
// only ever writes "REC_NNNN.wav" (12 chars), but the web portal lets a user
// rename a note, so any buffer that holds a note's filename — the on-device
// list/playback rows AND the portal — sizes to this + 1. Kept here so
// both sides agree on the bound (a renamed name longer than this is refused by
// the portal, never silently truncated or dropped). 48-char base + ".wav".
constexpr size_t kMaxRecNameLen = 52;

typedef bool (*ExistsFn)(const char* path, void* ctx);

// "/recordings/REC_0042.wav" — zero-padded to 4 digits, grows naturally past
// 9999. Returns the formatted length, or -1 if out is too small.
inline int formatRecPath(char* out, size_t outSize, uint32_t counter) {
    int n = std::snprintf(out, outSize, "%s/REC_%04" PRIu32 ".wav",
                          kRecordingsDir, counter);
    if (n < 0 || (size_t)n >= outSize) return -1;
    return n;
}

// "REC_0042.wav" — basename for index.csv rows.
inline int formatRecBasename(char* out, size_t outSize, uint32_t counter) {
    int n = std::snprintf(out, outSize, "REC_%04" PRIu32 ".wav", counter);
    if (n < 0 || (size_t)n >= outSize) return -1;
    return n;
}

// First counter >= start whose path doesn't exist. Returns kCounterExhausted
// after maxProbes consecutive collisions.
inline uint32_t nextFreeCounter(uint32_t start, ExistsFn exists, void* ctx,
                                uint32_t maxProbes = 10000) {
    char path[40];
    for (uint32_t i = 0; i < maxProbes; ++i) {
        uint32_t candidate = start + i;
        if (formatRecPath(path, sizeof(path), candidate) < 0) continue;
        if (!exists(path, ctx)) return candidate;
    }
    return kCounterExhausted;
}

// Whole seconds of audio in `bytes` of raw PCM (floor). byteRate for
// 16kHz/16-bit/mono is 32000.
inline uint32_t durationSecondsFromPcmBytes(uint64_t bytes, uint32_t byteRate) {
    if (byteRate == 0) return 0;
    return (uint32_t)(bytes / byteRate);
}

inline const char* indexHeader() {
    return "filename,timestamp,duration_s,bytes\n";
}

// "REC_0042.wav,2026-06-09T14:23:11,123,3936044\n" — timestamp field is
// blank when localTime is null (device clock unset). Returns the formatted
// length, or -1 if out is too small.
inline int formatIndexRow(char* out, size_t outSize, const char* filename,
                          const struct tm* localTime,
                          uint32_t durationSeconds, uint64_t bytes) {
    char stamp[24] = "";
    if (localTime != nullptr) {
        if (std::strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%S",
                          localTime) == 0) {
            stamp[0] = '\0';
        }
    }
    int n = std::snprintf(out, outSize, "%s,%s,%" PRIu32 ",%" PRIu64 "\n",
                          filename, stamp, durationSeconds, bytes);
    if (n < 0 || (size_t)n >= outSize) return -1;
    return n;
}

// True if the first CSV field of `line` (up to the first comma or end of the
// line) equals `filename` exactly. Used to drop a single row from index.csv on
// delete. Prefix-collision safe: the row for "REC_0010.wav" never matches a
// delete of "REC_0001.wav", because the whole field must equal the whole name.
inline bool indexRowMatchesFilename(const char* line, const char* filename) {
    if (line == nullptr || filename == nullptr) return false;
    size_t fieldLen = 0;
    while (line[fieldLen] != '\0' && line[fieldLen] != ',' &&
           line[fieldLen] != '\r' && line[fieldLen] != '\n') {
        ++fieldLen;
    }
    size_t nameLen = std::strlen(filename);
    return fieldLen == nameLen && std::strncmp(line, filename, nameLen) == 0;
}

// Parse [begin, end) as an unsigned decimal. Returns false if the range is
// empty or contains any non-digit. (Internal helper for parseIndexRow.)
inline bool parseDigits(const char* begin, const char* end, uint64_t* out) {
    if (begin >= end) return false;
    uint64_t v = 0;
    for (const char* p = begin; p < end; ++p) {
        if (*p < '0' || *p > '9') return false;
        v = v * 10 + (uint64_t)(*p - '0');
    }
    *out = v;
    return true;
}

// Parse one index.csv DATA row ("name,timestamp,duration_s,bytes"). Copies the
// filename and timestamp out (timestamp may be empty when the clock was unset)
// and returns the numeric duration + byte count. Returns false for the header
// line, a blank line, or any malformed row (wrong field count, a name or
// timestamp that overflows its buffer, or a non-numeric number) — callers use
// the false return to skip the header and to tolerate a partially written row.
inline bool parseIndexRow(const char* line,
                          char* nameOut, size_t nameSize,
                          char* tsOut, size_t tsSize,
                          uint32_t* durationOut, uint64_t* bytesOut) {
    if (line == nullptr) return false;
    const char* c1 = std::strchr(line, ',');
    if (c1 == nullptr) return false;
    const char* c2 = std::strchr(c1 + 1, ',');
    if (c2 == nullptr) return false;
    const char* c3 = std::strchr(c2 + 1, ',');
    if (c3 == nullptr) return false;

    // duration field [c2+1, c3) — all digits, non-empty (rejects the header's
    // literal "duration_s").
    uint64_t duration = 0;
    if (!parseDigits(c2 + 1, c3, &duration)) return false;

    // bytes field [c3+1, end-of-line) — all digits, non-empty.
    const char* bstart = c3 + 1;
    const char* bend = bstart;
    while (*bend != '\0' && *bend != '\r' && *bend != '\n') ++bend;
    uint64_t bytes = 0;
    if (!parseDigits(bstart, bend, &bytes)) return false;

    // name [line, c1) — non-empty, fits.
    size_t nameLen = (size_t)(c1 - line);
    if (nameLen == 0 || nameLen >= nameSize) return false;
    // timestamp [c1+1, c2) — may be empty, must fit.
    size_t tsLen = (size_t)(c2 - (c1 + 1));
    if (tsLen >= tsSize) return false;

    std::memcpy(nameOut, line, nameLen);
    nameOut[nameLen] = '\0';
    std::memcpy(tsOut, c1 + 1, tsLen);
    tsOut[tsLen] = '\0';
    *durationOut = (uint32_t)duration;
    *bytesOut = bytes;
    return true;
}

} // namespace RecNaming

#endif // REC_NAMING_H
