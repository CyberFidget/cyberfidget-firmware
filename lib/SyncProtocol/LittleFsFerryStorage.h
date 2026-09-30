// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#ifndef SYNC_PROTOCOL_LITTLE_FS_FERRY_STORAGE_H
#define SYNC_PROTOCOL_LITTLE_FS_FERRY_STORAGE_H

#include <FS.h>
#include "FerrySession.h"

namespace SyncProtocol {

// Device adapter shared by serial and WiFi ferry drivers. Each driver owns
// its own instance and staging buffer; the callers serialize manifest writes.
class LittleFsFerryStorage : public FerryStorage {
public:
    bool mount() override;
    size_t freeBytes() override;
    uint8_t* acquirePayload() override;
    void releasePayload() override;
    bool openTemp(const char* tempPath) override;
    bool seekTemp(uint32_t offset) override;
    size_t writeTemp(const uint8_t* data, size_t len) override;
    void closeTemp() override;
    bool openReadBack(const char* path, uint32_t& sizeOut) override;
    int readBack(uint8_t* buf, size_t cap) override;
    void closeReadBack() override;
    bool rename(const char* from, const char* to) override;
    bool remove(const char* path) override;
    bool applyManifestOps(const char* opsJson, int& entriesOut,
                          int& appliedOut) override;
    bool loadManifest(std::string& jsonOut) override;
    bool writeFile(const char* path, const uint8_t* data, size_t len) override;
    bool listFiles(const char* dir, std::vector<std::string>& namesOut) override;
    uint32_t nowEpochSeconds() override;

private:
    uint8_t* payload_ = nullptr;
    File writeFile_;
    File readFile_;
};

} // namespace SyncProtocol

#endif
