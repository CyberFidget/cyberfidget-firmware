// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

// test/test_sync_ferry/test_ferry.cpp
//
// SyncProtocol::FerrySession driven directly with an in-memory storage fake
// and a scripted byte source: the write-session state machine behind
// fwrite / fwdata / fwcommit / fwabort, and the lapply manifest apply. Reply
// bytes are asserted exactly, terminators included, because they are the
// frozen serial wire format. No UART or filesystem; SerialCli is not
// compiled here.

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include <unity.h>
#include "FerrySession.h"
#include "LoadoutManifest.h"
#include "SyncProtocol.h"

using namespace SyncProtocol;

// ---------- fakes ----------

class FakeStorage : public FerryStorage {
public:
    std::map<std::string, std::vector<uint8_t>> files;
    std::vector<std::string> removed;

    bool     mountOk = true;
    size_t   freeB = 1u << 20;
    bool     allocOk = true;
    bool     openOk = true;
    bool     seekOk = true;
    size_t   shortWriteBy = 0;   // write reports this many bytes fewer
    bool     readBackOk = true;
    int      renameFailures = 0; // fail this many renames first
    bool     saveOk = true;      // manifest persist succeeds
    int      payloadLive = 0;    // acquired-but-not-released buffers
    int      applyCalls = 0;

    LoadoutManifest::Loadout manifest;

    bool mount() override { return mountOk; }
    size_t freeBytes() override { return freeB; }

    uint8_t* acquirePayload() override {
        if (!allocOk) return nullptr;
        payloadLive = 1;
        return buf_;
    }
    void releasePayload() override { payloadLive = 0; }

    bool openTemp(const char* tempPath) override {
        if (!openOk) return false;
        tempName_ = tempPath;
        files[tempName_].clear();
        pos_ = 0;
        tempOpen_ = true;
        return true;
    }
    bool seekTemp(uint32_t offset) override {
        if (!seekOk) return false;
        pos_ = offset;
        return true;
    }
    size_t writeTemp(const uint8_t* data, size_t len) override {
        size_t n = len - (shortWriteBy < len ? shortWriteBy : len);
        auto it = files.find(tempName_);
        if (it == files.end()) return 0;
        std::vector<uint8_t>& f = it->second;
        if (f.size() < pos_ + n) f.resize(pos_ + n);
        std::memcpy(f.data() + pos_, data, n);
        pos_ += n;
        return n;
    }
    void closeTemp() override { tempOpen_ = false; }

    bool openReadBack(const char* path, uint32_t& sizeOut) override {
        if (!readBackOk) return false;
        auto it = files.find(path);
        if (it == files.end()) return false;
        readData_ = it->second;
        readPos_ = 0;
        sizeOut = (uint32_t)readData_.size();
        return true;
    }
    int readBack(uint8_t* buf, size_t cap) override {
        size_t n = readData_.size() - readPos_;
        if (n > cap) n = cap;
        std::memcpy(buf, readData_.data() + readPos_, n);
        readPos_ += n;
        return (int)n;
    }
    void closeReadBack() override {}

    bool rename(const char* from, const char* to) override {
        if (renameFailures > 0) { renameFailures--; return false; }
        auto it = files.find(from);
        if (it == files.end()) return false;
        files[to] = it->second;
        files.erase(from);
        return true;
    }
    bool remove(const char* path) override {
        removed.push_back(path);
        // Like the device's LittleFS: a file that is still open cannot be
        // unlinked ("Has open FD"), so the removal silently does nothing.
        if (tempOpen_ && tempName_ == path) return false;
        return files.erase(path) > 0;
    }

    bool applyManifestOps(const char* opsJson, int& entriesOut,
                          int& appliedOut) override {
        applyCalls++;
        LoadoutManifest::Loadout work = manifest;
        int applied = 0;
        if (!LoadoutManifest::applyOps(work, opsJson, &applied)) return false;
        if (!saveOk) return false;
        manifest = work;
        manifestStored = true;
        entriesOut = (int)manifest.entries.size();
        appliedOut = applied;
        return true;
    }

    // ---- batch apply effects ----
    bool     manifestStored = true; // false = no manifest (lget present=0)
    bool     writeFileOk = true;    // false = injected record-write failure
    uint32_t clock = 0;
    int      loadCalls = 0;
    int      writeFileCalls = 0;
    int      listCalls = 0;

    bool loadManifest(std::string& jsonOut) override {
        loadCalls++;
        if (!manifestStored) return false;
        jsonOut = LoadoutManifest::serializeManifest(manifest);
        return true;
    }
    bool writeFile(const char* path, const uint8_t* data, size_t len) override {
        writeFileCalls++;
        if (!writeFileOk) {
            files[path] = std::vector<uint8_t>(data, data + len / 2); // torn
            return false;
        }
        files[path] = std::vector<uint8_t>(data, data + len);
        return true;
    }
    bool listFiles(const char* dir, std::vector<std::string>& namesOut) override {
        listCalls++;
        const std::string prefix = std::string(dir) + "/";
        for (const auto& kv : files) {
            if (kv.first.compare(0, prefix.size(), prefix) != 0) continue;
            const std::string rest = kv.first.substr(prefix.size());
            if (rest.find('/') != std::string::npos) continue; // not recursive
            namesOut.push_back(rest);
        }
        return true;
    }
    uint32_t nowEpochSeconds() override { return clock; }

    bool tempOpen() const { return tempOpen_; }
    bool has(const char* path) const { return files.count(path) > 0; }
    std::string content(const char* path) const {
        auto it = files.find(path);
        if (it == files.end()) return std::string();
        return std::string(it->second.begin(), it->second.end());
    }

private:
    uint8_t buf_[kPayloadBufBytes];
    std::string tempName_;
    size_t pos_ = 0;
    bool tempOpen_ = false;
    std::vector<uint8_t> readData_;
    size_t readPos_ = 0;
};

// Byte source scripted with whatever bytes "arrived"; asking for more than
// that is a transport timeout.
class FakeBytes : public FerryByteSource {
public:
    std::string data;
    size_t pos = 0;
    size_t drained = 0;

    explicit FakeBytes(const std::string& d = std::string()) : data(d) {}

    bool readExact(uint8_t* buf, size_t n) override {
        if (data.size() - pos < n) { pos = data.size(); return false; }
        std::memcpy(buf, data.data() + pos, n);
        pos += n;
        return true;
    }
    void drain(size_t n) override {
        size_t avail = data.size() - pos;
        size_t take = n < avail ? n : avail;
        pos += take;
        drained += take;
    }
};

static std::string text(const FerryReply& r) {
    return std::string(r.text, r.len);
}

static uint32_t crcOf(const std::string& s) {
    return crc32(s.data(), s.size());
}

static std::string hdr(const char* fmt, ...) {
    char b[160];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    return b;
}

static void assertReply(const char* want, const FerryReply& r) {
    TEST_ASSERT_EQUAL_STRING(want, text(r).c_str());
}

// Open /apps/hello.wasm for "HELLO".
static void openHello(FerrySession& s) {
    FerryReply r = s.open("/apps/hello.wasm 5 c1446436");
    assertReply("[cmd] fwrite.ok=/apps/hello.wasm size=5 chunk=4096 crc=c1446436\n", r);
    TEST_ASSERT_TRUE(r.ok);
}

// ---------- legal transitions ----------

void test_starts_idle(void) {
    FakeStorage fs;
    FerrySession s(fs);
    TEST_ASSERT_TRUE(s.state() == FerrySession::State::Idle);
    TEST_ASSERT_FALSE(s.active());
}

void test_full_write_commit(void) {
    FakeStorage fs;
    FerrySession s(fs);
    openHello(s);
    TEST_ASSERT_TRUE(s.state() == FerrySession::State::Active);
    TEST_ASSERT_TRUE(fs.has("/apps/hello.wasm.part"));
    TEST_ASSERT_EQUAL_INT(1, fs.payloadLive);

    FakeBytes in("HELLO");
    assertReply("[cmd] fwdata.ok=off 0 len 5\n", s.chunk("0 5 c1446436", in));
    FerryReply r = s.commit();
    assertReply("[cmd] fwcommit.ok=/apps/hello.wasm size=5 crc=c1446436\n", r);
    TEST_ASSERT_TRUE(r.ok);
    TEST_ASSERT_FALSE(s.active());
    TEST_ASSERT_EQUAL_STRING("HELLO", fs.content("/apps/hello.wasm").c_str());
    TEST_ASSERT_FALSE(fs.has("/apps/hello.wasm.part"));
    TEST_ASSERT_EQUAL_INT(0, fs.payloadLive);   // buffer released at idle
    TEST_ASSERT_FALSE(fs.tempOpen());
}

void test_out_of_order_chunks_commit(void) {
    FakeStorage fs;
    FerrySession s(fs);
    openHello(s);
    FakeBytes tail("LLO");
    assertReply("[cmd] fwdata.ok=off 2 len 3\n",
                s.chunk(hdr("2 3 %08x", crcOf("LLO")).c_str(), tail));
    FakeBytes head("HE");
    assertReply("[cmd] fwdata.ok=off 0 len 2\n",
                s.chunk(hdr("0 2 %08x", crcOf("HE")).c_str(), head));
    assertReply("[cmd] fwcommit.ok=/apps/hello.wasm size=5 crc=c1446436\n", s.commit());
}

void test_commit_overwrites_existing_via_rename_fallback(void) {
    FakeStorage fs;
    fs.files["/apps/hello.wasm"] = {'o', 'l', 'd'};
    fs.renameFailures = 1;   // first rename refused -> remove final, retry
    FerrySession s(fs);
    openHello(s);
    FakeBytes in("HELLO");
    s.chunk("0 5 c1446436", in);
    assertReply("[cmd] fwcommit.ok=/apps/hello.wasm size=5 crc=c1446436\n", s.commit());
    TEST_ASSERT_EQUAL_STRING("HELLO", fs.content("/apps/hello.wasm").c_str());
}

void test_commit_rename_failure(void) {
    FakeStorage fs;
    fs.renameFailures = 2;
    FerrySession s(fs);
    openHello(s);
    FakeBytes in("HELLO");
    s.chunk("0 5 c1446436", in);
    assertReply("[err] fwcommit.rename=/apps/hello.wasm\n", s.commit());
    TEST_ASSERT_FALSE(s.active());
    TEST_ASSERT_FALSE(fs.has("/apps/hello.wasm.part"));
}

void test_nested_path_opens_temp(void) {
    FakeStorage fs;
    FerrySession s(fs);
    assertReply("[cmd] fwrite.ok=/assets/sub/x.dat size=1 chunk=4096 crc=00000000\n",
                s.open("/assets/sub/x.dat 1 0"));
    TEST_ASSERT_TRUE(fs.has("/assets/sub/x.dat.part"));
}

// ---------- duplicate begin ----------

void test_duplicate_begin_discards_prior_temp_only(void) {
    FakeStorage fs;
    fs.files["/apps/keep.bin"] = {'k'};
    FerrySession s(fs);
    openHello(s);
    FakeBytes in("HELLO");
    s.chunk("0 5 c1446436", in);

    assertReply("[cmd] fwrite.ok=/apps/other.bin size=2 chunk=4096 crc=0000abcd\n",
                s.open("/apps/other.bin 2 abcd"));
    TEST_ASSERT_TRUE(s.active());
    TEST_ASSERT_EQUAL_INT(1, (int)fs.removed.size());
    TEST_ASSERT_EQUAL_STRING("/apps/hello.wasm.part", fs.removed[0].c_str());
    TEST_ASSERT_FALSE(fs.has("/apps/hello.wasm.part"));
    TEST_ASSERT_TRUE(fs.has("/apps/other.bin.part"));
    TEST_ASSERT_TRUE(fs.has("/apps/keep.bin"));
    TEST_ASSERT_EQUAL_INT(1, fs.payloadLive);
}

void test_rejected_begin_keeps_active_session(void) {
    // Checks before the stale-session abort (usage, path, mount, space)
    // leave an active session alone, as the serial CLI always has.
    FakeStorage fs;
    FerrySession s(fs);
    openHello(s);
    s.open("/etc/passwd 1 0");
    s.open("garbage");
    fs.freeB = 1;
    assertReply("[err] fwrite.space=need 5 free 1\n", s.open("/apps/b.bin 5 0"));
    TEST_ASSERT_TRUE(s.active());
    TEST_ASSERT_TRUE(fs.removed.empty());
}

// ---------- open failures ----------

void test_open_errors(void) {
    FakeStorage fs;
    FerrySession s(fs);
    assertReply("[err] fwrite.usage=fwrite <path> <size> <crc32>\r\n", s.open("/apps/x"));
    fs.mountOk = false;
    assertReply("[err] fwrite.fs=mount failed\r\n", s.open("/apps/x.bin 1 0"));
    fs.mountOk = true;
    fs.allocOk = false;
    assertReply("[err] fwrite.nomem\r\n", s.open("/apps/x.bin 1 0"));
    fs.allocOk = true;
    fs.openOk = false;
    assertReply("[err] fwrite.open=/apps/x.bin.part\n", s.open("/apps/x.bin 1 0"));
    TEST_ASSERT_FALSE(s.active());
    TEST_ASSERT_EQUAL_INT(0, fs.payloadLive);
}

// ---------- confinement ----------

void test_confinement_rejections(void) {
    FakeStorage fs;
    FerrySession s(fs);
    assertReply("[err] fwrite.path=/etc/passwd (confined to /apps/ or /assets/)\n",
                s.open("/etc/passwd 1 0"));
    assertReply("[err] fwrite.path=/apps/../x (confined to /apps/ or /assets/)\n",
                s.open("/apps/../x 1 0"));
    assertReply("[err] fwrite.path=/apps/ (confined to /apps/ or /assets/)\n",
                s.open("/apps/ 1 0"));
    TEST_ASSERT_FALSE(s.active());
    TEST_ASSERT_TRUE(fs.files.empty());
}

void test_loadout_manifest_not_writable_by_fwrite(void) {
    FakeStorage fs;
    FerrySession s(fs);
    assertReply("[err] fwrite.path=/loadout.json (confined to /apps/ or /assets/)\n",
                s.open("/loadout.json 2 0"));
    assertReply("[err] fwrite.path=/apps/../loadout.json (confined to /apps/ or /assets/)\n",
                s.open("/apps/../loadout.json 2 0"));
    TEST_ASSERT_FALSE(s.active());
    TEST_ASSERT_TRUE(fs.files.empty());
}

// ---------- chunk failures ----------

void test_chunk_crc_failure_nak_keeps_session(void) {
    FakeStorage fs;
    FerrySession s(fs);
    openHello(s);
    FakeBytes bad("HELLX");
    FerryReply r = s.chunk("0 5 c1446436", bad);
    std::string want = hdr("[err] fwdata.crc=off 0 got %08x want c1446436\n",
                           crcOf("HELLX"));
    TEST_ASSERT_EQUAL_STRING(want.c_str(), text(r).c_str());
    TEST_ASSERT_FALSE(r.ok);
    TEST_ASSERT_TRUE(s.active());
    TEST_ASSERT_EQUAL_INT(0, (int)fs.content("/apps/hello.wasm.part").size());
    // Resend the same offset.
    FakeBytes good("HELLO");
    assertReply("[cmd] fwdata.ok=off 0 len 5\n", s.chunk("0 5 c1446436", good));
    assertReply("[cmd] fwcommit.ok=/apps/hello.wasm size=5 crc=c1446436\n", s.commit());
}

void test_chunk_header_errors(void) {
    FakeStorage fs;
    FerrySession s(fs);
    FakeBytes none;
    assertReply("[err] fwdata.usage=fwdata <offset> <len> <crc32>\r\n", s.chunk("0 x", none));
    assertReply("[err] fwdata.len=0\r\n", s.chunk("0 0 0", none));
    FakeBytes big(std::string(4097, 'a'));
    assertReply("[err] fwdata.toobig=4097 max=4096\n", s.chunk("0 4097 0", big));
    TEST_ASSERT_EQUAL_UINT32(4097u, (uint32_t)big.drained);
}

void test_chunk_without_session_drains(void) {
    FakeStorage fs;
    FerrySession s(fs);
    FakeBytes in("HELLO");
    assertReply("[err] fwdata.nosession\r\n", s.chunk("0 5 c1446436", in));
    TEST_ASSERT_EQUAL_UINT32(5u, (uint32_t)in.drained);
}

void test_chunk_offset_gap_beyond_size_rejected(void) {
    FakeStorage fs;
    FerrySession s(fs);
    openHello(s);
    FakeBytes in("LLO");
    assertReply("[err] fwdata.range=off 3 len 3 size 5\n", s.chunk("3 3 0", in));
    TEST_ASSERT_EQUAL_UINT32(3u, (uint32_t)in.drained);
    TEST_ASSERT_TRUE(s.active());
}

void test_offset_gap_caught_at_commit(void) {
    // A skipped chunk leaves the temp short; commit rejects it and discards.
    FakeStorage fs;
    FerrySession s(fs);
    openHello(s);
    FakeBytes head("HE");
    s.chunk(hdr("0 2 %08x", crcOf("HE")).c_str(), head);
    assertReply("[err] fwcommit.size=got 2 want 5\n", s.commit());
    TEST_ASSERT_FALSE(s.active());
    TEST_ASSERT_FALSE(fs.has("/apps/hello.wasm.part"));
    TEST_ASSERT_FALSE(fs.has("/apps/hello.wasm"));
}

void test_chunk_timeout_aborts_session(void) {
    FakeStorage fs;
    FerrySession s(fs);
    openHello(s);
    FakeBytes shortIn("HEL");
    assertReply("[err] fwdata.timeout\r\n", s.chunk("0 5 c1446436", shortIn));
    TEST_ASSERT_FALSE(s.active());
    TEST_ASSERT_FALSE(fs.has("/apps/hello.wasm.part"));
    TEST_ASSERT_EQUAL_INT(0, fs.payloadLive);
}

void test_chunk_seek_failure_keeps_session(void) {
    FakeStorage fs;
    FerrySession s(fs);
    openHello(s);
    fs.seekOk = false;
    FakeBytes in("HELLO");
    assertReply("[err] fwdata.seek=0\n", s.chunk("0 5 c1446436", in));
    TEST_ASSERT_TRUE(s.active());
}

void test_chunk_short_write_aborts_session(void) {
    FakeStorage fs;
    FerrySession s(fs);
    openHello(s);
    fs.shortWriteBy = 2;
    FakeBytes in("HELLO");
    assertReply("[err] fwdata.write=3/5\n", s.chunk("0 5 c1446436", in));
    TEST_ASSERT_FALSE(s.active());
    TEST_ASSERT_FALSE(fs.has("/apps/hello.wasm.part"));
}

// ---------- abort / reset ----------

void test_abort_removes_only_temp(void) {
    FakeStorage fs;
    fs.files["/apps/hello.wasm"] = {'o', 'l', 'd'};
    FerrySession s(fs);
    openHello(s);
    FerryReply r = s.abort();
    assertReply("[cmd] fwabort.ok\r\n", r);
    TEST_ASSERT_TRUE(r.ok);
    TEST_ASSERT_FALSE(s.active());
    TEST_ASSERT_EQUAL_INT(1, (int)fs.removed.size());
    TEST_ASSERT_EQUAL_STRING("/apps/hello.wasm.part", fs.removed[0].c_str());
    TEST_ASSERT_EQUAL_STRING("old", fs.content("/apps/hello.wasm").c_str());
    TEST_ASSERT_EQUAL_INT(0, fs.payloadLive);
}

void test_abort_when_idle_is_ok_and_removes_nothing(void) {
    FakeStorage fs;
    FerrySession s(fs);
    assertReply("[cmd] fwabort.ok\r\n", s.abort());
    TEST_ASSERT_TRUE(fs.removed.empty());
}

void test_commit_without_session(void) {
    FakeStorage fs;
    FerrySession s(fs);
    assertReply("[err] fwcommit.nosession\r\n", s.commit());
    openHello(s);
    s.abort();
    assertReply("[err] fwcommit.nosession\r\n", s.commit());
}

void test_destruction_removes_only_temp(void) {
    FakeStorage fs;
    fs.files["/apps/keep.bin"] = {'k'};
    {
        FerrySession s(fs);
        openHello(s);
    }
    TEST_ASSERT_EQUAL_INT(1, (int)fs.removed.size());
    TEST_ASSERT_EQUAL_STRING("/apps/hello.wasm.part", fs.removed[0].c_str());
    TEST_ASSERT_TRUE(fs.has("/apps/keep.bin"));
    TEST_ASSERT_FALSE(fs.tempOpen());
}

void test_reset_returns_to_idle(void) {
    FakeStorage fs;
    FerrySession s(fs);
    openHello(s);
    s.reset();
    TEST_ASSERT_FALSE(s.active());
    TEST_ASSERT_FALSE(fs.has("/apps/hello.wasm.part"));
    // Idle reset / destruction removes nothing further.
    s.reset();
    TEST_ASSERT_EQUAL_INT(1, (int)fs.removed.size());
}

// ---------- commit failures ----------

void test_commit_bad_whole_file_crc(void) {
    FakeStorage fs;
    FerrySession s(fs);
    // Header promises a different whole-file crc than the chunks deliver.
    assertReply("[cmd] fwrite.ok=/apps/hello.wasm size=5 chunk=4096 crc=deadbeef\n",
                s.open("/apps/hello.wasm 5 deadbeef"));
    FakeBytes in("HELLO");
    assertReply("[cmd] fwdata.ok=off 0 len 5\n", s.chunk("0 5 c1446436", in));
    FerryReply r = s.commit();
    assertReply("[err] fwcommit.crc=got c1446436 want deadbeef\n", r);
    TEST_ASSERT_FALSE(r.ok);
    TEST_ASSERT_FALSE(s.active());
    TEST_ASSERT_FALSE(fs.has("/apps/hello.wasm.part"));
    TEST_ASSERT_FALSE(fs.has("/apps/hello.wasm"));
}

void test_commit_reopen_failure(void) {
    FakeStorage fs;
    FerrySession s(fs);
    openHello(s);
    FakeBytes in("HELLO");
    s.chunk("0 5 c1446436", in);
    fs.readBackOk = false;
    assertReply("[err] fwcommit.reopen\r\n", s.commit());
    TEST_ASSERT_FALSE(s.active());
    TEST_ASSERT_FALSE(fs.has("/apps/hello.wasm.part"));
}

// ---------- lapply ----------

static LoadoutManifest::Loadout baseline(void) {
    LoadoutManifest::Loadout l;
    const char* ids[] = { "APP_A", "APP_B" };
    for (int i = 0; i < 2; i++) {
        LoadoutManifest::LoadoutEntry e;
        e.id = ids[i];
        e.name = ids[i];
        e.category = "Games";
        e.position = i;
        l.entries.push_back(e);
    }
    return l;
}

static FerryReply lapply(FerrySession& s, const std::string& doc) {
    FakeBytes in(doc);
    std::string args = hdr("%u %08x", (unsigned)doc.size(), crcOf(doc));
    return s.applyManifest(args.c_str(), in);
}

void test_lapply_applies(void) {
    FakeStorage fs;
    fs.manifest = baseline();
    FerrySession s(fs);
    FerryReply r = lapply(s, "{\"ops\":[{\"op\":\"hide\",\"id\":\"APP_B\",\"hidden\":true}]}");
    assertReply("[cmd] lapply.ok=applied 1 entries 2\n", r);
    TEST_ASSERT_TRUE(r.ok);
    TEST_ASSERT_TRUE(fs.manifest.entries[1].hidden);
    TEST_ASSERT_EQUAL_INT(0, fs.payloadLive);
}

void test_lapply_rejected_op_leaves_manifest_untouched(void) {
    FakeStorage fs;
    fs.manifest = baseline();
    FerrySession s(fs);
    const std::string doc =
        "{\"ops\":["
        "{\"op\":\"add\",\"entry\":{\"id\":\"APP_E\",\"category\":\"Games\"}},"
        "{\"op\":\"remove\",\"id\":\"APP_NOPE\"}]}";
    assertReply("[err] lapply.reject\r\n", lapply(s, doc));
    TEST_ASSERT_EQUAL_INT(2, (int)fs.manifest.entries.size());
    TEST_ASSERT_EQUAL_STRING("APP_A", fs.manifest.entries[0].id.c_str());
    TEST_ASSERT_EQUAL_STRING("APP_B", fs.manifest.entries[1].id.c_str());
    TEST_ASSERT_EQUAL_INT(0, fs.payloadLive);
}

void test_lapply_failed_save_rejects(void) {
    FakeStorage fs;
    fs.manifest = baseline();
    fs.saveOk = false;
    FerrySession s(fs);
    assertReply("[err] lapply.reject\r\n",
                lapply(s, "{\"ops\":[{\"op\":\"remove\",\"id\":\"APP_A\"}]}"));
    TEST_ASSERT_EQUAL_INT(2, (int)fs.manifest.entries.size());
}

void test_lapply_crc_and_framing_errors(void) {
    FakeStorage fs;
    fs.manifest = baseline();
    FerrySession s(fs);
    FakeBytes none;
    assertReply("[err] lapply.usage=lapply <len> <crc32>\r\n", s.applyManifest("x", none));
    assertReply("[err] lapply.len=0\r\n", s.applyManifest("0 0", none));
    FakeBytes big(std::string(8193, ' '));
    assertReply("[err] lapply.toobig=8193 max=8192\n", s.applyManifest("8193 0", big));
    TEST_ASSERT_EQUAL_UINT32(8193u, (uint32_t)big.drained);

    const std::string doc = "{\"ops\":[]}";
    FakeBytes in(doc);
    FerryReply r = s.applyManifest(hdr("%u 1234abcd", (unsigned)doc.size()).c_str(), in);
    std::string want = hdr("[err] lapply.crc=got %08x want 1234abcd\n", crcOf(doc));
    TEST_ASSERT_EQUAL_STRING(want.c_str(), text(r).c_str());

    FakeBytes shortIn("{\"o");
    assertReply("[err] lapply.timeout\r\n",
                s.applyManifest(hdr("%u %08x", (unsigned)doc.size(), crcOf(doc)).c_str(),
                                shortIn));
    fs.allocOk = false;
    FakeBytes drop(doc);
    assertReply("[err] lapply.nomem\r\n",
                s.applyManifest(hdr("%u %08x", (unsigned)doc.size(), crcOf(doc)).c_str(),
                                drop));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)doc.size(), (uint32_t)drop.drained);
    TEST_ASSERT_EQUAL_INT(0, fs.applyCalls);
    TEST_ASSERT_EQUAL_INT(2, (int)fs.manifest.entries.size());
}

void test_lapply_during_write_keeps_session_and_buffer(void) {
    FakeStorage fs;
    fs.manifest = baseline();
    FerrySession s(fs);
    openHello(s);
    assertReply("[cmd] lapply.ok=applied 1 entries 2\n",
                lapply(s, "{\"ops\":[{\"op\":\"hide\",\"id\":\"APP_A\",\"hidden\":true}]}"));
    TEST_ASSERT_TRUE(s.active());
    TEST_ASSERT_EQUAL_INT(1, fs.payloadLive);  // still held for the write
    FakeBytes in("HELLO");
    assertReply("[cmd] fwdata.ok=off 0 len 5\n", s.chunk("0 5 c1446436", in));
    assertReply("[cmd] fwcommit.ok=/apps/hello.wasm size=5 crc=c1446436\n", s.commit());
}

// ---------- lapply batch contract (batch / base / replace / orphan sweep) ----------

static uint32_t manifestCrc(const FakeStorage& fs) {
    return crcOf(LoadoutManifest::serializeManifest(fs.manifest));
}

static std::string hex8(uint32_t v) {
    return hdr("%08x", (unsigned)v);
}

// A baseline whose APP_B is a delivered blob app.
static LoadoutManifest::Loadout deliveredBaseline(void) {
    LoadoutManifest::Loadout l = baseline();
    l.entries[1].format   = "wasm";
    l.entries[1].blobPath = "/apps/APP_B-0123abcd.wasm";
    l.entries[1].version  = "1.0";
    l.entries[1].abi      = "1";
    l.entries[1].hidden   = true;
    return l;
}

void test_lapply_legacy_doc_takes_no_batch_effects(void) {
    FakeStorage fs;
    fs.manifest = baseline();
    fs.files["/apps/orphan-deadbeef.wasm"] = {1};
    FerrySession s(fs);
    assertReply("[cmd] lapply.ok=applied 1 entries 2\n",
                lapply(s, "{\"ops\":[{\"op\":\"hide\",\"id\":\"APP_B\",\"hidden\":true}]}"));
    TEST_ASSERT_EQUAL_INT(1, fs.applyCalls);
    TEST_ASSERT_EQUAL_INT(0, fs.loadCalls);
    TEST_ASSERT_EQUAL_INT(0, fs.writeFileCalls);
    TEST_ASSERT_EQUAL_INT(0, fs.listCalls);
    TEST_ASSERT_TRUE(fs.removed.empty());
    TEST_ASSERT_TRUE(fs.has("/apps/orphan-deadbeef.wasm"));
    TEST_ASSERT_FALSE(fs.has(kAppliedRecordPath));
}

void test_lapply_replace_swaps_blob_keeps_placement(void) {
    FakeStorage fs;
    fs.manifest = deliveredBaseline();
    FerrySession s(fs);
    const std::string doc =
        "{\"ops\":[{\"op\":\"replace\",\"entry\":{\"id\":\"APP_B\",\"name\":\"B2\","
        "\"category\":\"Tools\",\"hidden\":false,\"position\":0,"
        "\"blobPath\":\"/apps/APP_B-89abcdef.wasm\",\"version\":\"2.0\",\"abi\":\"2\"}}]}";
    assertReply("[cmd] lapply.ok=applied 1 entries 2\n", lapply(s, doc));
    const LoadoutManifest::LoadoutEntry& e = fs.manifest.entries[1];
    TEST_ASSERT_EQUAL_STRING("APP_B", e.id.c_str());
    TEST_ASSERT_EQUAL_STRING("/apps/APP_B-89abcdef.wasm", e.blobPath.c_str());
    TEST_ASSERT_EQUAL_STRING("2.0", e.version.c_str());
    TEST_ASSERT_EQUAL_STRING("2", e.abi.c_str());
    TEST_ASSERT_EQUAL_STRING("B2", e.name.c_str());
    TEST_ASSERT_EQUAL_STRING("Games", e.category.c_str()); // kept
    TEST_ASSERT_TRUE(e.hidden);                            // kept
    TEST_ASSERT_EQUAL_INT(1, e.position);                  // kept
    TEST_ASSERT_EQUAL_STRING("wasm", e.format.c_str());    // kept
}

void test_lapply_replace_unknown_id_refuses_document(void) {
    FakeStorage fs;
    fs.manifest = deliveredBaseline();
    const uint32_t before = manifestCrc(fs);
    FerrySession s(fs);
    const std::string doc =
        "{\"batch\":\"b-1\",\"base\":\"" + hex8(before) + "\",\"ops\":["
        "{\"op\":\"hide\",\"id\":\"APP_A\",\"hidden\":true},"
        "{\"op\":\"replace\",\"entry\":{\"id\":\"APP_NOPE\","
        "\"blobPath\":\"/apps/APP_NOPE-00000000.wasm\"}}]}";
    assertReply("[err] lapply.reject\r\n", lapply(s, doc));
    TEST_ASSERT_EQUAL_UINT32(before, manifestCrc(fs));
    TEST_ASSERT_FALSE(fs.manifest.entries[0].hidden); // the hide was not kept
    TEST_ASSERT_FALSE(fs.has(kAppliedRecordPath));
}

void test_lapply_stale_base_refused_and_unchanged(void) {
    FakeStorage fs;
    fs.manifest = deliveredBaseline();
    fs.files["/apps/orphan-deadbeef.wasm"] = {1};
    const uint32_t current = manifestCrc(fs);
    const uint32_t wrong = current ^ 1u;
    FerrySession s(fs);
    const std::string doc = "{\"batch\":\"b-1\",\"base\":\"" + hex8(wrong) +
        "\",\"ops\":[{\"op\":\"remove\",\"id\":\"APP_A\"}]}";
    FerryReply r = lapply(s, doc);
    TEST_ASSERT_FALSE(r.ok);
    TEST_ASSERT_EQUAL_STRING(("[err] lapply.stale=" + hex8(current) + "\n").c_str(),
                             text(r).c_str());
    TEST_ASSERT_EQUAL_INT(0, fs.applyCalls);
    TEST_ASSERT_EQUAL_UINT32(current, manifestCrc(fs));
    TEST_ASSERT_FALSE(fs.has(kAppliedRecordPath));
    TEST_ASSERT_TRUE(fs.has("/apps/orphan-deadbeef.wasm")); // no sweep either
    TEST_ASSERT_EQUAL_INT(0, fs.payloadLive);
}

void test_lapply_matching_base_applies(void) {
    FakeStorage fs;
    fs.manifest = baseline();
    FerrySession s(fs);
    // Upper-case hex is the same CRC.
    std::string base = hex8(manifestCrc(fs));
    for (char& ch : base) if (ch >= 'a' && ch <= 'f') ch = (char)(ch - 'a' + 'A');
    const std::string doc = "{\"base\":\"" + base +
        "\",\"ops\":[{\"op\":\"remove\",\"id\":\"APP_A\"}]}";
    assertReply("[cmd] lapply.ok=applied 1 entries 1\n", lapply(s, doc));
    TEST_ASSERT_EQUAL_INT(1, (int)fs.manifest.entries.size());
    TEST_ASSERT_FALSE(fs.has(kAppliedRecordPath)); // no batch -> no record
}

void test_lapply_base_zero_matches_absent_manifest(void) {
    FakeStorage fs;
    fs.manifest = baseline();   // what the device seeds from the registry
    fs.manifestStored = false;  // but nothing is stored yet
    FerrySession s(fs);
    assertReply("[err] lapply.stale=00000000\n",
                lapply(s, "{\"base\":\"12345678\",\"ops\":[]}"));
    assertReply("[cmd] lapply.ok=applied 1 entries 2\n",
                lapply(s, "{\"base\":\"00000000\",\"ops\":"
                          "[{\"op\":\"hide\",\"id\":\"APP_A\",\"hidden\":true}]}"));
}

void test_lapply_batch_recorded_before_success_reply(void) {
    FakeStorage fs;
    fs.manifest = baseline();
    fs.clock = 1790000000u;
    FerrySession s(fs);
    const std::string doc = "{\"batch\":\"b-42\",\"base\":\"" + hex8(manifestCrc(fs)) +
        "\",\"ops\":[{\"op\":\"add\",\"entry\":{\"id\":\"APP_E\",\"category\":\"Games\","
        "\"format\":\"wasm\",\"blobPath\":\"/apps/APP_E-00c0ffee.wasm\"}}]}";
    FerryReply r = lapply(s, doc);
    assertReply("[cmd] lapply.ok=applied 1 entries 3\n", r);
    TEST_ASSERT_TRUE(fs.has(kAppliedRecordPath));
    TEST_ASSERT_FALSE(fs.has(kAppliedRecordTemp));
    LoadoutManifest::AppliedRecord rec;
    TEST_ASSERT_TRUE(LoadoutManifest::parseAppliedRecord(
        fs.content(kAppliedRecordPath).c_str(), rec));
    TEST_ASSERT_EQUAL_STRING("b-42", rec.batch.c_str());
    TEST_ASSERT_EQUAL_STRING("applied", rec.result.c_str());
    TEST_ASSERT_EQUAL_UINT32(manifestCrc(fs), rec.crcAfter);
    TEST_ASSERT_EQUAL_UINT32(1790000000u, rec.at);
    TEST_ASSERT_EQUAL_INT(1, rec.ops);
    TEST_ASSERT_EQUAL_INT(3, rec.entries);

    // The success reply depends on the record: when the record cannot be
    // made durable there is no success line.
    FakeStorage fs2;
    fs2.manifest = baseline();
    fs2.writeFileOk = false;
    FerrySession s2(fs2);
    FerryReply r2 = lapply(s2, "{\"batch\":\"b-43\",\"base\":\"" + hex8(manifestCrc(fs2)) +
                               "\",\"ops\":[{\"op\":\"hide\",\"id\":\"APP_A\",\"hidden\":true}]}");
    TEST_ASSERT_FALSE(r2.ok);
}

void test_lapply_repeat_batch_not_reapplied(void) {
    FakeStorage fs;
    fs.manifest = baseline();
    FerrySession s(fs);
    const std::string doc = "{\"batch\":\"b-7\",\"base\":\"" + hex8(manifestCrc(fs)) +
        "\",\"ops\":[{\"op\":\"add\",\"entry\":{\"id\":\"APP_E\",\"category\":\"Games\"}}]}";
    assertReply("[cmd] lapply.ok=applied 1 entries 3\n", lapply(s, doc));
    const uint32_t after = manifestCrc(fs);
    // The same batch again (lost reply): same success line, not re-applied
    // (a re-apply would be refused - APP_E already exists - and the base is
    // now stale).
    assertReply("[cmd] lapply.ok=applied 1 entries 3\n", lapply(s, doc));
    TEST_ASSERT_EQUAL_INT(1, fs.applyCalls);
    TEST_ASSERT_EQUAL_UINT32(after, manifestCrc(fs));
    TEST_ASSERT_EQUAL_INT(0, fs.payloadLive);

    // A different batch id is a new document and goes through the checks.
    const std::string next = "{\"batch\":\"b-8\",\"base\":\"" + hex8(after) +
        "\",\"ops\":[{\"op\":\"remove\",\"id\":\"APP_E\"}]}";
    assertReply("[cmd] lapply.ok=applied 1 entries 2\n", lapply(s, next));
    TEST_ASSERT_EQUAL_INT(2, fs.applyCalls);
}

// Defined consistent state for a failure between the manifest save and the
// record write (the injected record failure stands in for a power cut at
// that point): the NEW manifest is in place, the record is the previous one
// or none (here: none), no temp is left behind, and no success line was sent.
// The sender's retry of the same batch carries the old `base`, so it is
// refused as stale - reporting the post-apply CRC - and never applied twice.
void test_lapply_failure_between_apply_and_record_is_consistent(void) {
    FakeStorage fs;
    fs.manifest = baseline();
    const uint32_t before = manifestCrc(fs);
    fs.writeFileOk = false;
    FerrySession s(fs);
    const std::string doc = "{\"batch\":\"b-9\",\"base\":\"" + hex8(before) +
        "\",\"ops\":[{\"op\":\"remove\",\"id\":\"APP_A\"}]}";
    FerryReply r = lapply(s, doc);
    const uint32_t after = manifestCrc(fs);
    TEST_ASSERT_FALSE(r.ok);
    TEST_ASSERT_EQUAL_STRING(("[err] lapply.record=" + hex8(after) + "\n").c_str(),
                             text(r).c_str());
    TEST_ASSERT_EQUAL_INT(1, (int)fs.manifest.entries.size()); // applied
    TEST_ASSERT_FALSE(fs.has(kAppliedRecordPath));
    TEST_ASSERT_FALSE(fs.has(kAppliedRecordTemp));             // torn temp removed

    fs.writeFileOk = true;
    FerryReply retry = lapply(s, doc);
    TEST_ASSERT_EQUAL_STRING(("[err] lapply.stale=" + hex8(after) + "\n").c_str(),
                             text(retry).c_str());
    TEST_ASSERT_EQUAL_INT(1, fs.applyCalls);
    TEST_ASSERT_EQUAL_INT(1, (int)fs.manifest.entries.size());

    // A rename failure after a good temp write is a record failure too, and
    // leaves no temp behind.
    FakeStorage fs2;
    fs2.manifest = baseline();
    fs2.files[kAppliedRecordPath] = std::vector<uint8_t>{'o', 'l', 'd'};
    FerrySession s2(fs2);
    fs2.renameFailures = 2;
    FerryReply r2 = lapply(s2, "{\"batch\":\"b-10\",\"base\":\"" + hex8(manifestCrc(fs2)) +
                               "\",\"ops\":[{\"op\":\"hide\",\"id\":\"APP_A\",\"hidden\":true}]}");
    TEST_ASSERT_FALSE(r2.ok);
    TEST_ASSERT_FALSE(fs2.has(kAppliedRecordTemp));
}

void test_lapply_sweep_deletes_only_orphan_delivered_blobs(void) {
    FakeStorage fs;
    fs.manifest = deliveredBaseline();                 // references APP_B-0123abcd
    fs.files["/apps/APP_B-0123abcd.wasm"] = {1};       // referenced: kept
    fs.files["/apps/APP_B-89abcdef.wasm"] = {1};       // orphan delivered: deleted
    fs.files["/apps/legacy.wasm"] = {1};               // browser-send name: kept
    fs.files["/apps/APP_C-DEADBEEF.wasm"] = {1};       // upper-case hash: kept
    fs.files["/apps/APP_C-0123abc.wasm"] = {1};        // 7-digit hash: kept
    fs.files["/apps/APP_C-0123abcd.bin"] = {1};        // not .wasm: kept
    fs.files["/apps/sub/x-00000000.wasm"] = {1};       // nested: kept
    fs.files["/apps/.diary/batdiary.bin"] = {1};       // internal: kept
    fs.files["/assets/y-00000000.wasm"] = {1};         // other root: kept
    FerrySession s(fs);
    assertReply("[cmd] lapply.ok=applied 1 entries 2\n",
                lapply(s, "{\"batch\":\"b-gc\",\"base\":\"" + hex8(manifestCrc(fs)) +
                          "\",\"ops\":[{\"op\":\"hide\",\"id\":\"APP_A\",\"hidden\":true}]}"));
    TEST_ASSERT_FALSE(fs.has("/apps/APP_B-89abcdef.wasm"));
    TEST_ASSERT_TRUE(fs.has("/apps/APP_B-0123abcd.wasm"));
    TEST_ASSERT_TRUE(fs.has("/apps/legacy.wasm"));
    TEST_ASSERT_TRUE(fs.has("/apps/APP_C-DEADBEEF.wasm"));
    TEST_ASSERT_TRUE(fs.has("/apps/APP_C-0123abc.wasm"));
    TEST_ASSERT_TRUE(fs.has("/apps/APP_C-0123abcd.bin"));
    TEST_ASSERT_TRUE(fs.has("/apps/sub/x-00000000.wasm"));
    TEST_ASSERT_TRUE(fs.has("/apps/.diary/batdiary.bin"));
    TEST_ASSERT_TRUE(fs.has("/assets/y-00000000.wasm"));
    TEST_ASSERT_TRUE(fs.has(kAppliedRecordPath));
    TEST_ASSERT_EQUAL_INT(1, (int)fs.removed.size());
}

// Data screensavers under /assets/ss are swept by the same rule: removing
// the entry frees its file; only the delivered `<id>-<hash8>.cfs` shape at
// the top of /assets/ss is ever deleted.
void test_lapply_remove_sweeps_orphan_data_screensaver(void) {
    FakeStorage fs;
    fs.manifest = baseline();
    LoadoutManifest::LoadoutEntry ss;
    ss.id = "doodle";
    ss.name = "Doodle";
    ss.category = "Screensavers";
    ss.format = "cfsprite";
    ss.blobPath = "/assets/ss/doodle-0123abcd.cfs";
    fs.manifest.entries.push_back(ss);
    LoadoutManifest::LoadoutEntry keep = ss;
    keep.id = "dino";
    keep.blobPath = "/assets/ss/dino-89abcdef.cfs";
    fs.manifest.entries.push_back(keep);
    fs.files["/assets/ss/doodle-0123abcd.cfs"] = {1};  // its entry is removed: deleted
    fs.files["/assets/ss/dino-89abcdef.cfs"] = {1};    // still referenced: kept
    fs.files["/assets/ss/old-00c0ffee.cfs"] = {1};     // orphan of the shape: deleted
    fs.files["/assets/ss/notes.cfs"] = {1};            // other name: kept
    fs.files["/assets/ss/x-00c0ffee.wasm"] = {1};      // not .cfs: kept
    fs.files["/assets/ss/sub/y-00000000.cfs"] = {1};   // nested: kept
    fs.files["/assets/z-00000000.cfs"] = {1};          // other directory: kept
    FerrySession s(fs);
    assertReply("[cmd] lapply.ok=applied 1 entries 3\n",
                lapply(s, "{\"batch\":\"b-ss\",\"base\":\"" + hex8(manifestCrc(fs)) +
                          "\",\"ops\":[{\"op\":\"remove\",\"id\":\"doodle\"}]}"));
    TEST_ASSERT_FALSE(fs.has("/assets/ss/doodle-0123abcd.cfs"));
    TEST_ASSERT_FALSE(fs.has("/assets/ss/old-00c0ffee.cfs"));
    TEST_ASSERT_TRUE(fs.has("/assets/ss/dino-89abcdef.cfs"));
    TEST_ASSERT_TRUE(fs.has("/assets/ss/notes.cfs"));
    TEST_ASSERT_TRUE(fs.has("/assets/ss/x-00c0ffee.wasm"));
    TEST_ASSERT_TRUE(fs.has("/assets/ss/sub/y-00000000.cfs"));
    TEST_ASSERT_TRUE(fs.has("/assets/z-00000000.cfs"));
    TEST_ASSERT_EQUAL_INT(2, (int)fs.removed.size());
}

void test_lapply_replace_then_sweep_drops_old_blob(void) {
    FakeStorage fs;
    fs.manifest = deliveredBaseline();
    fs.files["/apps/APP_B-0123abcd.wasm"] = {1};
    fs.files["/apps/APP_B-89abcdef.wasm"] = {2};  // new blob, already ferried
    FerrySession s(fs);
    const std::string doc = "{\"batch\":\"b-r\",\"base\":\"" + hex8(manifestCrc(fs)) +
        "\",\"ops\":[{\"op\":\"replace\",\"entry\":{\"id\":\"APP_B\",\"name\":\"B\","
        "\"blobPath\":\"/apps/APP_B-89abcdef.wasm\",\"version\":\"2\",\"abi\":\"1\"}}]}";
    assertReply("[cmd] lapply.ok=applied 1 entries 2\n", lapply(s, doc));
    TEST_ASSERT_FALSE(fs.has("/apps/APP_B-0123abcd.wasm"));
    TEST_ASSERT_TRUE(fs.has("/apps/APP_B-89abcdef.wasm"));
}

void test_lapply_sweep_skipped_during_write_session(void) {
    FakeStorage fs;
    fs.manifest = baseline();
    fs.files["/apps/APP_E-00c0ffee.wasm"] = {1};  // not yet in the manifest
    FerrySession s(fs);
    openHello(s);
    assertReply("[cmd] lapply.ok=applied 1 entries 2\n",
                lapply(s, "{\"batch\":\"b-w\",\"base\":\"" + hex8(manifestCrc(fs)) +
                          "\",\"ops\":[{\"op\":\"hide\",\"id\":\"APP_A\",\"hidden\":true}]}"));
    TEST_ASSERT_TRUE(fs.has("/apps/APP_E-00c0ffee.wasm"));
    TEST_ASSERT_EQUAL_INT(0, fs.listCalls);
    TEST_ASSERT_TRUE(s.active());
    TEST_ASSERT_EQUAL_INT(1, fs.payloadLive);
}

void test_lapply_invalid_batch_or_base_rejected(void) {
    FakeStorage fs;
    fs.manifest = baseline();
    FerrySession s(fs);
    const std::string longId(41, 'x');
    assertReply("[err] lapply.reject\r\n",
                lapply(s, "{\"batch\":\"" + longId + "\",\"ops\":[]}"));
    assertReply("[err] lapply.reject\r\n", lapply(s, "{\"batch\":\"\",\"ops\":[]}"));
    assertReply("[err] lapply.reject\r\n", lapply(s, "{\"batch\":7,\"ops\":[]}"));
    assertReply("[err] lapply.reject\r\n", lapply(s, "{\"base\":\"xyz\",\"ops\":[]}"));
    assertReply("[err] lapply.reject\r\n",
                lapply(s, "{\"base\":\"123456789\",\"ops\":[]}"));
    TEST_ASSERT_FALSE(fs.has(kAppliedRecordPath));
    TEST_ASSERT_EQUAL_INT(2, (int)fs.manifest.entries.size());
}

void test_applied_record_not_reachable_by_fwrite(void) {
    FakeStorage fs;
    FerrySession s(fs);
    assertReply("[err] fwrite.path=/apps/.applied.json (confined to /apps/ or /assets/)\n",
                s.open("/apps/.applied.json 5 c1446436"));
    assertReply("[err] fwrite.path=/apps/.applied.json.part (confined to /apps/ or /assets/)\n",
                s.open("/apps/.applied.json.part 5 c1446436"));
    TEST_ASSERT_FALSE(s.active());
}

void test_lapply_batch_id_reused_for_other_document_refused(void) {
    FakeStorage fs;
    fs.manifest = baseline();
    FerrySession s(fs);
    const std::string doc = "{\"batch\":\"b-1\",\"base\":\"" + hex8(manifestCrc(fs)) +
        "\",\"ops\":[{\"op\":\"hide\",\"id\":\"APP_A\",\"hidden\":true}]}";
    assertReply("[cmd] lapply.ok=applied 1 entries 2\n", lapply(s, doc));
    LoadoutManifest::AppliedRecord rec;
    TEST_ASSERT_TRUE(LoadoutManifest::parseAppliedRecord(
        fs.content(kAppliedRecordPath).c_str(), rec));
    TEST_ASSERT_EQUAL_UINT32(crcOf(doc), rec.docCrc);
    const std::string recBefore = fs.content(kAppliedRecordPath);
    const uint32_t after = manifestCrc(fs);

    // Same id, different bytes (current base, so only the reuse can refuse it).
    const std::string other = "{\"batch\":\"b-1\",\"base\":\"" + hex8(after) +
        "\",\"ops\":[{\"op\":\"remove\",\"id\":\"APP_B\"}]}";
    FerryReply r = lapply(s, other);
    assertReply("[err] lapply.batchreuse\r\n", r);
    TEST_ASSERT_FALSE(r.ok);
    TEST_ASSERT_EQUAL_INT(1, fs.applyCalls);
    TEST_ASSERT_EQUAL_UINT32(after, manifestCrc(fs));
    TEST_ASSERT_EQUAL_STRING(recBefore.c_str(), fs.content(kAppliedRecordPath).c_str());
    TEST_ASSERT_EQUAL_INT(0, fs.payloadLive);

    // The original bytes are still a plain retry.
    assertReply("[cmd] lapply.ok=applied 1 entries 2\n", lapply(s, doc));
    TEST_ASSERT_EQUAL_INT(1, fs.applyCalls);
}

void test_lapply_batch_without_base_is_usage_error(void) {
    FakeStorage fs;
    fs.manifest = baseline();
    fs.files["/apps/orphan-deadbeef.wasm"] = {1};
    FerrySession s(fs);
    FerryReply r = lapply(s, "{\"batch\":\"b-1\",\"ops\":"
                             "[{\"op\":\"hide\",\"id\":\"APP_A\",\"hidden\":true}]}");
    assertReply("[err] lapply.usage=batch requires base\r\n", r);
    TEST_ASSERT_FALSE(r.ok);
    TEST_ASSERT_EQUAL_INT(0, fs.applyCalls);
    TEST_ASSERT_FALSE(fs.manifest.entries[0].hidden);
    TEST_ASSERT_FALSE(fs.has(kAppliedRecordPath));
    TEST_ASSERT_TRUE(fs.has("/apps/orphan-deadbeef.wasm"));
    TEST_ASSERT_EQUAL_INT(0, fs.payloadLive);
}

void test_lapply_sweep_skipped_when_manifest_was_absent(void) {
    FakeStorage fs;
    fs.manifest = baseline();   // the registry rebuild: references no blobs
    fs.manifestStored = false;
    fs.files["/apps/APP_B-0123abcd.wasm"] = {1};
    FerrySession s(fs);
    assertReply("[cmd] lapply.ok=applied 1 entries 2\n",
                lapply(s, "{\"batch\":\"b-0\",\"base\":\"00000000\",\"ops\":"
                          "[{\"op\":\"hide\",\"id\":\"APP_A\",\"hidden\":true}]}"));
    TEST_ASSERT_TRUE(fs.has("/apps/APP_B-0123abcd.wasm"));
    TEST_ASSERT_EQUAL_INT(0, fs.listCalls);
    TEST_ASSERT_TRUE(fs.has(kAppliedRecordPath));
}

void test_lapply_replace_of_builtin_refused(void) {
    FakeStorage fs;
    fs.manifest = deliveredBaseline();
    fs.manifest.entries[0].format = "builtin";
    const uint32_t before = manifestCrc(fs);
    FerrySession s(fs);
    const std::string doc = "{\"batch\":\"b-1\",\"base\":\"" + hex8(before) +
        "\",\"ops\":[{\"op\":\"replace\",\"entry\":{\"id\":\"APP_A\","
        "\"blobPath\":\"/apps/APP_A-00000000.wasm\"}}]}";
    assertReply("[err] lapply.reject\r\n", lapply(s, doc));
    TEST_ASSERT_EQUAL_UINT32(before, manifestCrc(fs));
    TEST_ASSERT_FALSE(fs.has(kAppliedRecordPath));
}

void test_lapply_unconfined_blob_path_refused(void) {
    const char* paths[] = {
        "/loadout.json", "/apps/../x.wasm", "/sd/x-00000000.wasm",
        "/apps/.applied.json", "apps/x.wasm",
    };
    for (const char* p : paths) {
        FakeStorage fs;
        fs.manifest = deliveredBaseline();
        const uint32_t before = manifestCrc(fs);
        FerrySession s(fs);
        const std::string rep = "{\"batch\":\"b-1\",\"base\":\"" + hex8(before) +
            "\",\"ops\":[{\"op\":\"replace\",\"entry\":{\"id\":\"APP_B\","
            "\"blobPath\":\"" + std::string(p) + "\"}}]}";
        TEST_ASSERT_EQUAL_STRING_MESSAGE("[err] lapply.reject\r\n",
                                         text(lapply(s, rep)).c_str(), p);
        // The same check applies to add, in a plain document too.
        const std::string add = "{\"ops\":[{\"op\":\"add\",\"entry\":{\"id\":\"APP_E\","
            "\"format\":\"wasm\",\"blobPath\":\"" + std::string(p) + "\"}}]}";
        TEST_ASSERT_EQUAL_STRING_MESSAGE("[err] lapply.reject\r\n",
                                         text(lapply(s, add)).c_str(), p);
        TEST_ASSERT_EQUAL_INT(0, fs.applyCalls);
        TEST_ASSERT_EQUAL_UINT32(before, manifestCrc(fs));
    }
}

void setUp(void)    {}
void tearDown(void) {}

int main(int /*argc*/, char** /*argv*/) {
    UNITY_BEGIN();
    RUN_TEST(test_starts_idle);
    RUN_TEST(test_full_write_commit);
    RUN_TEST(test_out_of_order_chunks_commit);
    RUN_TEST(test_commit_overwrites_existing_via_rename_fallback);
    RUN_TEST(test_commit_rename_failure);
    RUN_TEST(test_nested_path_opens_temp);
    RUN_TEST(test_duplicate_begin_discards_prior_temp_only);
    RUN_TEST(test_rejected_begin_keeps_active_session);
    RUN_TEST(test_open_errors);
    RUN_TEST(test_confinement_rejections);
    RUN_TEST(test_loadout_manifest_not_writable_by_fwrite);
    RUN_TEST(test_chunk_crc_failure_nak_keeps_session);
    RUN_TEST(test_chunk_header_errors);
    RUN_TEST(test_chunk_without_session_drains);
    RUN_TEST(test_chunk_offset_gap_beyond_size_rejected);
    RUN_TEST(test_offset_gap_caught_at_commit);
    RUN_TEST(test_chunk_timeout_aborts_session);
    RUN_TEST(test_chunk_seek_failure_keeps_session);
    RUN_TEST(test_chunk_short_write_aborts_session);
    RUN_TEST(test_abort_removes_only_temp);
    RUN_TEST(test_abort_when_idle_is_ok_and_removes_nothing);
    RUN_TEST(test_commit_without_session);
    RUN_TEST(test_destruction_removes_only_temp);
    RUN_TEST(test_reset_returns_to_idle);
    RUN_TEST(test_commit_bad_whole_file_crc);
    RUN_TEST(test_commit_reopen_failure);
    RUN_TEST(test_lapply_applies);
    RUN_TEST(test_lapply_rejected_op_leaves_manifest_untouched);
    RUN_TEST(test_lapply_failed_save_rejects);
    RUN_TEST(test_lapply_crc_and_framing_errors);
    RUN_TEST(test_lapply_during_write_keeps_session_and_buffer);
    RUN_TEST(test_lapply_legacy_doc_takes_no_batch_effects);
    RUN_TEST(test_lapply_replace_swaps_blob_keeps_placement);
    RUN_TEST(test_lapply_replace_unknown_id_refuses_document);
    RUN_TEST(test_lapply_stale_base_refused_and_unchanged);
    RUN_TEST(test_lapply_matching_base_applies);
    RUN_TEST(test_lapply_base_zero_matches_absent_manifest);
    RUN_TEST(test_lapply_batch_recorded_before_success_reply);
    RUN_TEST(test_lapply_repeat_batch_not_reapplied);
    RUN_TEST(test_lapply_failure_between_apply_and_record_is_consistent);
    RUN_TEST(test_lapply_sweep_deletes_only_orphan_delivered_blobs);
    RUN_TEST(test_lapply_replace_then_sweep_drops_old_blob);
    RUN_TEST(test_lapply_remove_sweeps_orphan_data_screensaver);
    RUN_TEST(test_lapply_sweep_skipped_during_write_session);
    RUN_TEST(test_lapply_invalid_batch_or_base_rejected);
    RUN_TEST(test_applied_record_not_reachable_by_fwrite);
    RUN_TEST(test_lapply_batch_id_reused_for_other_document_refused);
    RUN_TEST(test_lapply_batch_without_base_is_usage_error);
    RUN_TEST(test_lapply_sweep_skipped_when_manifest_was_absent);
    RUN_TEST(test_lapply_replace_of_builtin_refused);
    RUN_TEST(test_lapply_unconfined_blob_path_refused);
    return UNITY_END();
}
