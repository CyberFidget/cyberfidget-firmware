// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

// lib/VoiceRecorderApp/RecNaming.h
//
// Pure naming + index.csv row logic for the voice recorder. No Arduino or
// ESP types so it compiles host-side for the native test env
// (test_voicerecorder). File existence is injected as a predicate so the
// collision-skip logic is testable without a filesystem.
//
// Header-only (vs the .h/.cpp split in T-088's notes) because the native
// test envs build with lib_ldf_mode = off, which won't compile lib .cpp
// files into the test binary.

#ifndef REC_NAMING_H
#define REC_NAMING_H

#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <ctime>

namespace RecNaming {

constexpr const char* kRecordingsDir = "/recordings";
constexpr uint32_t kCounterExhausted = 0xFFFFFFFFu;

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

} // namespace RecNaming

#endif // REC_NAMING_H
