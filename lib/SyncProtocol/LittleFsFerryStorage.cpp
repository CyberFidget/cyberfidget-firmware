// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#ifndef HOST_TEST
#include "LittleFsFerryStorage.h"

#include <LittleFS.h>
#include <esp_heap_caps.h>
#include <time.h>
#include <cstring>
#include <cstdlib>

#include "AppManager.h"
#include "AppDefs.h"
#include "LoadoutStore.h"

namespace SyncProtocol {
namespace {
void ensureParentDirs(const char* path) {
    char dir[128];
    strncpy(dir, path, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = '\0';
    for (char* s = dir + 1; *s; ++s) {
        if (*s != '/') continue;
        *s = '\0';
        if (!LittleFS.exists(dir)) LittleFS.mkdir(dir);
        *s = '/';
    }
}
}

bool LittleFsFerryStorage::mount() { return LoadoutStore::begin(); }
size_t LittleFsFerryStorage::freeBytes() {
    const size_t total = LittleFS.totalBytes(), used = LittleFS.usedBytes();
    return total > used ? total - used : 0;
}
uint8_t* LittleFsFerryStorage::acquirePayload() {
    if (!payload_) {
        payload_ = static_cast<uint8_t*>(heap_caps_malloc(kPayloadBufBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!payload_) payload_ = static_cast<uint8_t*>(malloc(kPayloadBufBytes));
    }
    return payload_;
}
void LittleFsFerryStorage::releasePayload() {
    free(payload_);
    payload_ = nullptr;
}
bool LittleFsFerryStorage::openTemp(const char* path) {
    ensureParentDirs(path);
    writeFile_ = LittleFS.open(path, FILE_WRITE);
    return (bool)writeFile_;
}
bool LittleFsFerryStorage::seekTemp(uint32_t offset) { return writeFile_.seek(offset, SeekSet); }
size_t LittleFsFerryStorage::writeTemp(const uint8_t* data, size_t len) { return writeFile_.write(data, len); }
void LittleFsFerryStorage::closeTemp() {
    if (writeFile_) { writeFile_.flush(); writeFile_.close(); }
}
bool LittleFsFerryStorage::openReadBack(const char* path, uint32_t& sizeOut) {
    readFile_ = LittleFS.open(path, FILE_READ);
    if (!readFile_) return false;
    sizeOut = (uint32_t)readFile_.size();
    return true;
}
int LittleFsFerryStorage::readBack(uint8_t* buf, size_t cap) { return readFile_.read(buf, cap); }
void LittleFsFerryStorage::closeReadBack() { readFile_.close(); }
bool LittleFsFerryStorage::rename(const char* from, const char* to) { return LittleFS.rename(from, to); }
bool LittleFsFerryStorage::remove(const char* path) { return LittleFS.remove(path); }
bool LittleFsFerryStorage::applyManifestOps(const char* json, int& entries, int& applied) {
    return AppManager::instance().applyLoadoutOps(json, &entries, &applied);
}
bool LittleFsFerryStorage::loadManifest(std::string& jsonOut) {
    LoadoutStore::begin();
    LoadoutManifest::Loadout lo;
    return loadLoadoutManifest(lo, &jsonOut);
}
bool LittleFsFerryStorage::writeFile(const char* path, const uint8_t* data, size_t len) {
    ensureParentDirs(path);
    File f = LittleFS.open(path, FILE_WRITE);
    if (!f) return false;
    const size_t wrote = f.write(data, len);
    f.flush(); f.close();
    return wrote == len;
}
bool LittleFsFerryStorage::listFiles(const char* dir, std::vector<std::string>& namesOut) {
    File directory = LittleFS.open(dir, FILE_READ);
    if (!directory) return false;
    if (!directory.isDirectory()) { directory.close(); return false; }
    for (File e = directory.openNextFile(); e; e = directory.openNextFile()) {
        const bool isDir = e.isDirectory();
        const char* name = e.name();
        if (!isDir && name) {
            const char* slash = strrchr(name, '/');
            namesOut.push_back(slash ? slash + 1 : name);
        }
        e.close();
    }
    directory.close();
    return true;
}
uint32_t LittleFsFerryStorage::nowEpochSeconds() {
    const time_t now = time(nullptr);
    return now > (time_t)1577836800 ? (uint32_t)now : 0;
}
} // namespace SyncProtocol
#endif // HOST_TEST
