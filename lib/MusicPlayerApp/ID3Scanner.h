// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#pragma once

#include <Arduino.h>
#include <SD.h>
#include <vector>

struct TrackInfo {
    String path;
    String title;
    String artist;
    String display;  // Precomputed "Artist - Title" or filename

    void computeDisplay();
};

namespace ID3Scanner {
    // Read ID3 tags from a single MP3 file
    bool scanFile(const char* path, String& title, String& artist);

    // Read bitrate from first MP3 frame header (returns bits/sec, e.g. 192000; 0 on failure)
    int readMP3Bitrate(const char* path);

    // Scan all MP3 files using AudioSourceIdxSD file listing
    // progressCb fires with (current, total) for UI updates
    void scanAllFiles(const char* rootDir, const char* ext,
                      std::vector<TrackInfo>& out,
                      void (*progressCb)(int current, int total) = nullptr);

    // List the files with this extension in a directory tree (the files scanAllFiles reads)
    void listFiles(const char* rootDir, const char* ext, std::vector<String>& out);

    // Cache management
    bool loadCache(const char* cachePath, std::vector<TrackInfo>& out);
    bool saveCache(const char* cachePath, const std::vector<TrackInfo>& tracks);
    bool isCacheValid(const char* cachePath, int expectedFileCount);
}
