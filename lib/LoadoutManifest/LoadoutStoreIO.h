// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#ifndef LOADOUT_STORE_IO_H
#define LOADOUT_STORE_IO_H

#include <cstdint>
#include <cstring>
#include <string>

// Shared file operations, also exercised with an in-memory filesystem.
namespace LoadoutStoreIO {

constexpr const char* kManifestPath = "/loadout.json";
constexpr const char* kTempPath = "/loadout.json.tmp";

enum class ReadResult { Missing, Ok, Unreadable };

template <typename Filesystem>
ReadResult read(Filesystem& fs, std::string& json) {
    json.clear();
    if (!fs.exists(kManifestPath)) return ReadResult::Missing;
    auto file = fs.open(kManifestPath, "r");
    if (!file || file.isDirectory()) return ReadResult::Unreadable;
    const size_t size = file.size();
    json.reserve(size);
    uint8_t buf[256];
    while (json.size() < size) {
        const size_t remaining = size - json.size();
        const size_t count = remaining < sizeof(buf) ? remaining : sizeof(buf);
        const size_t n = file.read(buf, count);
        if (!n || n > count) break;
        json.append(reinterpret_cast<const char*>(buf), n);
    }
    file.close();
    return json.size() == size ? ReadResult::Ok : ReadResult::Unreadable;
}

template <typename Filesystem>
bool save(Filesystem& fs, const std::string& json) {
    auto file = fs.open(kTempPath, "w");
    if (!file) return false;
    const size_t written = file.write(reinterpret_cast<const uint8_t*>(json.data()), json.size());
    file.flush();
    file.close();
    if (written != json.size()) return false;

    // Check the closed file byte for byte before switching the directory entry.
    auto check = fs.open(kTempPath, "r");
    if (!check || check.isDirectory()) return false;
    bool verified = check.size() == json.size();
    size_t offset = 0;
    uint8_t buf[256];
    while (verified && offset < json.size()) {
        const size_t remaining = json.size() - offset;
        const size_t count = remaining < sizeof(buf) ? remaining : sizeof(buf);
        const size_t n = check.read(buf, count);
        verified = n > 0 && n <= count && std::memcmp(buf, json.data() + offset, n) == 0;
        offset += n;
    }
    check.close();
    if (!verified) return false;

    // LittleFS replaces atomically. If the VFS refuses replacement, fail with
    // the old manifest intact and the verified temp available for inspection.
    // Removing the destination before retrying would create a power-loss gap.
    return fs.rename(kTempPath, kManifestPath);
}

} // namespace LoadoutStoreIO

#endif // LOADOUT_STORE_IO_H
