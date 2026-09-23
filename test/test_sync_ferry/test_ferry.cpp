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
        entriesOut = (int)manifest.entries.size();
        appliedOut = applied;
        return true;
    }

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
    return UNITY_END();
}
