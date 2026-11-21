// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#include <unity.h>
#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include "../../lib/LoadoutManifest/LoadoutStoreIO.h"
#include "../../lib/SyncProtocol/LoadoutReply.h"
#include "../../lib/LoadoutManifest/LoadoutManifest.cpp"
#include "../../lib/SyncProtocol/SyncProtocol.cpp"

using LoadoutStoreIO::kManifestPath;
using LoadoutStoreIO::kTempPath;
using LoadoutStoreIO::ReadResult;

namespace {
const std::string kOld = R"({"schemaVersion":1,"entries":[{"id":"old","name":"Old"}]})";
// Cross the 256-byte read boundary, including during read-back verification.
const std::string kNew = "{\"schemaVersion\":1,\"entries\":[{\"id\":\"new\",\"name\":\"" +
                         std::string(300, 'x') + "\"}]}";

struct PowerLoss {};

struct MemoryFS {
    std::map<std::string, std::string> files;
    int steps = 0, cutAt = 0, writes = 0, renames = 0;
    bool failWriteOpen = false, failReadOpen = false, shortWrite = false;
    bool badReadback = false, shortRead = false, failRename = false;
    bool readError = false, truncateOnFlush = false;

    void step() {
        if (++steps == cutAt) throw PowerLoss{};
    }

    struct File {
        MemoryFS* fs = nullptr;
        std::string path;
        size_t offset = 0;
        explicit operator bool() const { return fs != nullptr; }
        bool isDirectory() { fs->step(); fs->step(); return false; }
        size_t size() {
            fs->step();
            const size_t n = fs->files.at(path).size();
            fs->step();
            return n;
        }
        size_t write(const uint8_t* bytes, size_t len) {
            fs->step();
            ++fs->writes;
            const size_t n = fs->shortWrite ? len / 2 : len;
            // A cut can leave any prefix of the temp file on disk.
            for (size_t i = 0; i < n; ++i) {
                fs->files.at(path).push_back(static_cast<char>(bytes[i]));
                fs->step();
            }
            fs->step();
            return n;
        }
        size_t read(uint8_t* out, size_t cap) {
            fs->step();
            if (fs->readError) { fs->step(); return static_cast<size_t>(-1); }
            const auto& data = fs->files.at(path);
            const size_t n = fs->shortRead ? 0 : std::min(cap, data.size() - offset);
            std::memcpy(out, data.data() + offset, n);
            if (fs->badReadback && path == kTempPath && n) out[0] ^= 1;
            offset += n;
            fs->step();
            return n;
        }
        void flush() {
            fs->step();
            if (fs->truncateOnFlush && !fs->files.at(path).empty()) fs->files.at(path).pop_back();
            fs->step();
        }
        void close() { fs->step(); fs->step(); }
    };

    bool exists(const char* path) {
        step();
        const bool found = files.count(path) != 0;
        step();
        return found;
    }
    File open(const char* path, const char* mode) {
        step();
        const bool writing = mode[0] == 'w';
        if ((writing && failWriteOpen) || (!writing && failReadOpen)) {
            step();
            return {};
        }
        if (writing) { ++writes; files[path].clear(); }
        const bool found = files.count(path) != 0;
        step();
        return found ? File{this, path, 0} : File{};
    }
    bool rename(const char* from, const char* to) {
        step();
        ++renames;
        if (failRename) { step(); return false; }
        // LittleFS guarantees replacement is one atomic directory operation.
        files[to] = files.at(from);
        files.erase(from);
        step();
        return true;
    }
};

std::string reply(MemoryFS& fs) {
    std::string json;
    const auto read = LoadoutStoreIO::read(fs, json);
    if (read == ReadResult::Unreadable) return "[err] lget.read present=1\n";
    char header[SyncProtocol::kReadReplyBytes];
    bool corrupt = false;
    const bool present = read != ReadResult::Missing;
    const size_t n = SyncProtocol::formatLoadoutHeader(header, sizeof(header), present, json, corrupt);
    TEST_ASSERT_TRUE(n > 0);
    return std::string(header, n) + (present && !corrupt ? json : "");
}

void assertReadable(const MemoryFS& fs) {
    const auto found = fs.files.find(kManifestPath);
    TEST_ASSERT_TRUE(found != fs.files.end());
    TEST_ASSERT_TRUE(found->second == kOld || found->second == kNew);
    LoadoutManifest::Loadout parsed;
    TEST_ASSERT_TRUE(LoadoutManifest::parseManifest(found->second.c_str(), parsed));
}
} // namespace

void setUp() {}
void tearDown() {}

void test_corrupt_manifest_reports_raw_metadata_without_writing() {
    const std::vector<std::string> cases = {
        "{broken", "", R"({"schemaVersion":2,"entries":[]})", "null",
        kOld + std::string(1, '\0') + "garbage"
    };
    for (const auto& raw : cases) {
        MemoryFS fs;
        fs.files[kManifestPath] = raw;
        const auto before = fs.files;
        char expected[192];
        std::snprintf(expected, sizeof(expected),
            "[err] lget.present=1 entries=0 schema=0 len=%u crc=%08x corrupt=1\n",
            static_cast<unsigned>(raw.size()),
            static_cast<unsigned>(SyncProtocol::crc32(raw.data(), raw.size())));
        TEST_ASSERT_EQUAL_STRING(expected, reply(fs).c_str());
        TEST_ASSERT_TRUE(before == fs.files);
        TEST_ASSERT_EQUAL_INT(0, fs.writes);
        TEST_ASSERT_EQUAL_INT(0, fs.renames);
    }
}

void test_missing_manifest_keeps_the_absent_reply() {
    MemoryFS fs;
    // An interrupted first save's temp must never count as a stored manifest.
    fs.files[kTempPath] = "{";
    const auto before = fs.files;
    const std::string expected = std::string(SyncProtocol::kLgetAbsentReply) + "\n";
    TEST_ASSERT_EQUAL_STRING(expected.c_str(), reply(fs).c_str());
    TEST_ASSERT_TRUE(before == fs.files);
    TEST_ASSERT_EQUAL_INT(0, fs.writes);
}

void test_valid_manifest_reply_preserves_raw_bytes_and_legacy_ids() {
    MemoryFS fs;
    const std::string raw = " {\"schemaVersion\":1,\"entries\":[{\"id\":\"APP_TIMER\"}]} \n";
    fs.files[kManifestPath] = raw;
    const auto before = fs.files;
    char header[192];
    std::snprintf(header, sizeof(header),
        "[cmd] lget.present=1 entries=1 schema=1 len=%u crc=%08x\n",
        static_cast<unsigned>(raw.size()),
        static_cast<unsigned>(SyncProtocol::crc32(raw.data(), raw.size())));
    const std::string expected = header + raw;
    TEST_ASSERT_EQUAL_STRING(expected.c_str(), reply(fs).c_str());
    TEST_ASSERT_TRUE(before == fs.files);
    TEST_ASSERT_EQUAL_INT(0, fs.writes);
}

void test_unreadable_file_is_never_absent() {
    for (int fault = 0; fault < 3; ++fault) {
        MemoryFS fs;
        fs.files[kManifestPath] = kOld;
        fs.failReadOpen = fault == 0;
        fs.shortRead = fault == 1;
        fs.readError = fault == 2;
        TEST_ASSERT_EQUAL_STRING("[err] lget.read present=1\n", reply(fs).c_str());
        TEST_ASSERT_EQUAL_STRING(kOld.c_str(), fs.files.at(kManifestPath).c_str());
        TEST_ASSERT_EQUAL_INT(0, fs.writes);
    }
}

void test_save_power_loss_at_every_step_keeps_a_readable_manifest() {
    MemoryFS complete;
    complete.files[kManifestPath] = kOld;
    TEST_ASSERT_TRUE(LoadoutStoreIO::save(complete, kNew));
    const int steps = complete.steps;
    std::printf("Replacement power-loss cut points: %d\n", steps);
    TEST_ASSERT_TRUE(steps > static_cast<int>(kNew.size()));
    TEST_ASSERT_EQUAL_STRING(kNew.c_str(), complete.files.at(kManifestPath).c_str());
    for (int cut = 1; cut <= steps; ++cut) {
        MemoryFS fs;
        fs.files[kManifestPath] = kOld;
        fs.cutAt = cut;
        bool interrupted = false;
        try { LoadoutStoreIO::save(fs, kNew); }
        catch (const PowerLoss&) { interrupted = true; }
        TEST_ASSERT_TRUE(interrupted);
        assertReadable(fs);
        fs.cutAt = 0;
        std::string recovered;
        TEST_ASSERT_EQUAL_INT(static_cast<int>(ReadResult::Ok),
                              static_cast<int>(LoadoutStoreIO::read(fs, recovered)));
        TEST_ASSERT_EQUAL_STRING(fs.files.at(kManifestPath).c_str(), recovered.c_str());
        // The next save must work even with an orphaned partial temp file.
        TEST_ASSERT_TRUE(LoadoutStoreIO::save(fs, kNew));
        TEST_ASSERT_EQUAL_STRING(kNew.c_str(), fs.files.at(kManifestPath).c_str());
    }
}

void test_failed_save_keeps_old_manifest_and_never_renames_unverified_data() {
    for (int fault = 0; fault < 8; ++fault) {
        MemoryFS fs;
        fs.files[kManifestPath] = kOld;
        fs.failWriteOpen = fault == 0;
        fs.shortWrite = fault == 1;
        fs.failReadOpen = fault == 2;
        fs.badReadback = fault == 3;
        fs.shortRead = fault == 4;
        fs.failRename = fault == 5;
        fs.readError = fault == 6;
        fs.truncateOnFlush = fault == 7;
        TEST_ASSERT_FALSE(LoadoutStoreIO::save(fs, kNew));
        TEST_ASSERT_EQUAL_STRING(kOld.c_str(), fs.files.at(kManifestPath).c_str());
        TEST_ASSERT_EQUAL_INT(fault == 5 ? 1 : 0, fs.renames);
        if (fault == 5) TEST_ASSERT_EQUAL_STRING(kNew.c_str(), fs.files.at(kTempPath).c_str());
    }
}

void test_first_save_power_loss_never_installs_a_partial_manifest() {
    MemoryFS complete;
    TEST_ASSERT_TRUE(LoadoutStoreIO::save(complete, kNew));
    const int steps = complete.steps;
    std::printf("First-save power-loss cut points: %d\n", steps);
    for (int cut = 1; cut <= steps; ++cut) {
        MemoryFS fs;
        fs.cutAt = cut;
        bool interrupted = false;
        try { LoadoutStoreIO::save(fs, kNew); }
        catch (const PowerLoss&) { interrupted = true; }
        TEST_ASSERT_TRUE(interrupted);
        if (fs.files.count(kManifestPath)) {
            TEST_ASSERT_EQUAL_STRING(kNew.c_str(), fs.files.at(kManifestPath).c_str());
        }
    }
}

void test_header_buffer_bounds() {
    char small[8] = {};
    bool corrupt = false;
    TEST_ASSERT_EQUAL_UINT32(0, SyncProtocol::formatLoadoutHeader(small, sizeof(small), true, kOld, corrupt));
    TEST_ASSERT_EQUAL_STRING("", small);
    TEST_ASSERT_EQUAL_UINT32(0, SyncProtocol::formatLoadoutHeader(nullptr, 0, true, kOld, corrupt));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_corrupt_manifest_reports_raw_metadata_without_writing);
    RUN_TEST(test_missing_manifest_keeps_the_absent_reply);
    RUN_TEST(test_valid_manifest_reply_preserves_raw_bytes_and_legacy_ids);
    RUN_TEST(test_unreadable_file_is_never_absent);
    RUN_TEST(test_save_power_loss_at_every_step_keeps_a_readable_manifest);
    RUN_TEST(test_failed_save_keeps_old_manifest_and_never_renames_unverified_data);
    RUN_TEST(test_first_save_power_loss_never_installs_a_partial_manifest);
    RUN_TEST(test_header_buffer_bounds);
    return UNITY_END();
}
