// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#include "DeviceIdentity.h"

#include <cstring>
#include <cstdio>

namespace DeviceIdentity {

bool validFlashReads(bool firstOk, uint64_t first, bool secondOk, uint64_t second) {
    return firstOk && secondOk && first == second && first != 0 && first != UINT64_MAX;
}

void hex64(uint64_t value, char out[17]) {
    snprintf(out, 17, "%016llx", (unsigned long long)value);
}

void serialHex(uint32_t value, char out[9]) {
    if (value) snprintf(out, 9, "%08x", (unsigned)value);
    else out[0] = '\0';
}

bool matches(const Fingerprint& stored, const Fingerprint& live, bool releaseBuild) {
    if (!stored.id[0] && releaseBuild) return false;
    if (stored.id[0] && live.id[0] && strcmp(stored.id, live.id) != 0) return false;
    if (stored.flashId[0] && live.flashId[0] &&
        strcmp(stored.flashId, live.flashId) != 0) return false;
    if (stored.serial[0] && live.serial[0] &&
        strcmp(stored.serial, live.serial) != 0) return false;
    return true;
}

} // namespace DeviceIdentity

#ifndef HOST_TEST
#include <Arduino.h>
#include <Preferences.h>
#include <esp_flash.h>
#include <atomic>
#include "HAL.h"
#include "LinkSession.h"
#include "SyncProtocol.h"

namespace DeviceIdentity {

namespace {
std::atomic<bool> mismatchNotice{false};

class PrefsStore : public CloudSync::LinkStore {
public:
    explicit PrefsStore(Preferences& pair) : pair_(pair) {}
    std::string getString(const char* key) override { return pair_.getString(key, "").c_str(); }
    uint32_t getUInt(const char* key) override { return pair_.getUInt(key, 0); }
    bool getBool(const char* key) override { return pair_.getBool(key, false); }
    bool putString(const char* key, const std::string& value) override {
        return pair_.putString(key, value.c_str()) == value.size();
    }
    bool putUInt(const char* key, uint32_t value) override { return pair_.putUInt(key, value) != 0; }
    bool putBool(const char* key, bool value) override { return pair_.putBool(key, value) != 0; }
    bool remove(const char* key) override { return !pair_.isKey(key) || pair_.remove(key); }
private:
    Preferences& pair_;
};
}

bool takeMismatchNotice() { return mismatchNotice.exchange(false); }

Fingerprint readLive() {
    Fingerprint live;
    SyncProtocol::formatDeviceId(ESP.getEfuseMac(), live.id);
    uint64_t first = 0, second = 0;
    const bool firstOk = esp_flash_read_unique_chip_id(esp_flash_default_chip, &first) == ESP_OK;
    const bool secondOk = esp_flash_read_unique_chip_id(esp_flash_default_chip, &second) == ESP_OK;
    if (validFlashReads(firstOk, first, secondOk, second)) hex64(first, live.flashId);
    serialHex(HAL::boardInfo().serial, live.serial);
    return live;
}

bool checkStored(bool clean) {
    Preferences pair;
    if (!pair.begin("pair", !clean)) return false;
    PrefsStore store(pair);
    if (clean && !CloudSync::recoverPendingRevoke(store)) { pair.end(); return false; }
    if (clean && pair.getBool("unlk", false) && !CloudSync::prepareUnlink(store)) {
        pair.end(); return false;
    }
    const bool linked = pair.getBool("ok", false) &&
                        !pair.getString("tok", "").isEmpty();
    const bool pending = CloudSync::hasPendingRevoke(store);
    if (!linked) {
        if (clean) {
            pair.remove("ok"); pair.remove("tok");
            pair.remove("acct"); pair.remove("aref"); pair.remove("at");
        }
        if (!pending) { pair.end(); return true; }
    }
    Fingerprint stored;
    pair.getString("id", stored.id, sizeof(stored.id));
    pair.getString("fid", stored.flashId, sizeof(stored.flashId));
    pair.getString("ser", stored.serial, sizeof(stored.serial));
#ifdef CF_TEST_CLI
    const bool releaseBuild = false;
#else
    const bool releaseBuild = true;
#endif
    const bool ok = matches(stored, readLive(), releaseBuild);
    if (!ok && clean) {
        CloudSync::wipeMismatchedLink(store);
        mismatchNotice = true;
    }
    pair.end();
    return ok;
}

} // namespace DeviceIdentity
#endif
