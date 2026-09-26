// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

// Pure update rules: manifest parsing and gates, freshness keys, the source
// fallback seam, the install order (hash before finish, record before boot
// selection, abort on any failure), the pending record, and every branch of
// the pending-image self-test.

#include <unity.h>

#include <stdio.h>
#include <string.h>

#include <initializer_list>
#include <string>
#include <vector>

#include "BoardInfo.h"
#include "OtaUpdate.h"

using namespace OtaUpdate;

static const char* kSha = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

// The site's shipped manifest shape (update/firmware.php?manifest=1).
static std::string manifestJson(const char* overrideKey = nullptr, const char* overrideValue = nullptr) {
    struct Field { const char* key; std::string value; };
    std::vector<Field> fields = {
        {"version", "\"1.4.0\""},
        {"size", "1234567"},
        {"sha256", std::string("\"") + kSha + "\""},
        {"url", "\"/update/firmware.php?app=1&tag=v1.4.0&repo=CyberFidget%2Fcyberfidget-firmware"
                "&channel=stable&release_id=42&sha256=0123\""},
        {"hw", "{\"min_rev\":\"1.2\",\"max_rev\":\"1.2\"}"},
        {"channel", "\"stable\""},
        {"source", "\"official\""},
        {"release_id", "42"},
        {"released_at", "\"2026-09-20T10:00:00Z\""},
    };
    std::string out = "{";
    bool first = true;
    for (const Field& f : fields) {
        std::string value = f.value;
        if (overrideKey && strcmp(overrideKey, f.key) == 0) {
            if (!overrideValue) continue;   // drop the field
            value = overrideValue;
        }
        if (!first) out += ",";
        first = false;
        out += "\"" + std::string(f.key) + "\":" + value;
    }
    return out + "}";
}

static const char* parse(const std::string& json, Manifest& m) {
    return parseManifest(json.c_str(), json.size(), m);
}

static BoardInfo::Info board(uint8_t major, uint8_t minor) {
    BoardInfo::Info info = BoardInfo::defaults();
    info.source = BoardInfo::Source::Efuse;
    info.major = major;
    info.minor = minor;
    return info;
}

// ---- manifest ------------------------------------------------------------------

void test_parses_the_site_manifest(void) {
    Manifest m;
    TEST_ASSERT_NULL(parse(manifestJson(), m));
    TEST_ASSERT_EQUAL_STRING("1.4.0", m.version);
    TEST_ASSERT_EQUAL_UINT32(1234567, m.size);
    TEST_ASSERT_EQUAL_STRING(kSha, m.sha256);
    TEST_ASSERT_EQUAL_STRING("1.2", m.minRev);
    TEST_ASSERT_EQUAL_STRING("1.2", m.maxRev);
    TEST_ASSERT_EQUAL_STRING("stable", m.channel);
    TEST_ASSERT_EQUAL_STRING("official", m.source);
    TEST_ASSERT_EQUAL_UINT64(42, m.releaseId);
    uint32_t expected = 0;
    TEST_ASSERT_TRUE(parseUtc("2026-09-20T10:00:00Z", expected));
    TEST_ASSERT_EQUAL_UINT32(expected, m.releasedAt);
    TEST_ASSERT_EQUAL_UINT8('/', m.url[0]);
}

void test_every_field_is_required_and_validated(void) {
    struct Case { const char* key; const char* value; const char* reason; };
    const Case cases[] = {
        {"version", nullptr, "version"},
        {"version", "\"1.4\"", "version"},
        {"version", "\"v1.4.0\"", "version"},
        {"version", "\"1.4.0-rc.1+abcdefabcdefabcdefabcdefabcd\"", "version"},   // 32 chars
        {"version", "140", "version"},
        {"size", nullptr, "size"},
        {"size", "0", "size"},
        {"size", "-5", "size"},
        {"size", "12.5", "size"},
        {"size", "3342337", "size"},
        {"size", "\"1234\"", "size"},
        {"sha256", nullptr, "sha256"},
        {"sha256", "\"0123456789ABCDEF0123456789abcdef0123456789abcdef0123456789abcdef\"", "sha256"},
        {"sha256", "\"0123\"", "sha256"},
        {"url", nullptr, "url"},
        {"url", "\"https://evil.example/firmware.bin\"", "url"},
        {"url", "\"//evil.example/firmware.bin\"", "url"},
        {"url", "\"/update/firmware.php?app=1@evil\"", "url"},
        {"url", "\"/update/firm ware.php\"", "url"},
        {"hw", nullptr, "hw"},
        {"hw", "{\"min_rev\":\"1.2\"}", "hw"},
        {"hw", "\"1.2\"", "hw"},
        {"channel", nullptr, "channel"},
        {"channel", "\"beta\"", "channel"},
        {"source", nullptr, "source"},
        {"source", "\"fork:someone\"", "source"},
        {"source", "\"fork:someone/../x\"", "source"},
        {"source", "\"fork:some_one/x\"", "source"},
        {"source", "\"github\"", "source"},
        {"release_id", nullptr, "release_id"},
        {"release_id", "0", "release_id"},
        {"release_id", "1.5", "release_id"},
        {"release_id", "\"42\"", "release_id"},
        {"released_at", nullptr, "released_at"},
        {"released_at", "\"2026-09-20 10:00:00\"", "released_at"},
        {"released_at", "\"2026-02-30T10:00:00Z\"", "released_at"},
        {"released_at", "\"2026-09-20T10:00:00+00:00\"", "released_at"},
    };
    for (const Case& c : cases) {
        Manifest m;
        const char* reason = parse(manifestJson(c.key, c.value), m);
        char msg[160];
        snprintf(msg, sizeof(msg), "%s=%s", c.key, c.value ? c.value : "(missing)");
        TEST_ASSERT_NOT_NULL_MESSAGE(reason, msg);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(c.reason, reason, msg);
        // A refused manifest leaves nothing behind.
        TEST_ASSERT_EQUAL_STRING_MESSAGE("", m.version, msg);
    }
    Manifest m;
    TEST_ASSERT_EQUAL_STRING("json", parseManifest("{", 1, m));
    TEST_ASSERT_EQUAL_STRING("json", parseManifest("[]", 2, m));
    TEST_ASSERT_EQUAL_STRING("json", parseManifest("", 0, m));
}

void test_fork_source_and_rc_channel_parse(void) {
    Manifest m;
    TEST_ASSERT_NULL(parse(manifestJson("source", "\"fork:some-one/cyber.fidget_fw\""), m));
    TEST_ASSERT_EQUAL_STRING("fork:some-one/cyber.fidget_fw", m.source);
    TEST_ASSERT_NULL(parse(manifestJson("channel", "\"rc\""), m));
    TEST_ASSERT_NULL(parse(manifestJson("version", "\"1.4.0-rc1\""), m));
    TEST_ASSERT_NULL(parse(manifestJson("version", "\"1.4.0+abc1234.dirty\""), m));
}

void test_optional_signature_pair_is_strict(void) {
    auto withFields = [](const char* fields) {
        std::string json = manifestJson();
        json.pop_back();
        return json + fields + "}";
    };
    Manifest m;
    TEST_ASSERT_NULL(parse(withFields(",\"sig\":\"MEUCIQCHz8IEMOCjIStzZFpynuIjQvTz/HHL8olztrjWbppqKwIgPqA1QWeCpnfALWBulcoTtyg76yhQjj0ShuHib1xV0/4=\",\"key_id\":\"test-only-1\""), m));
    TEST_ASSERT_EQUAL_STRING("MEUCIQCHz8IEMOCjIStzZFpynuIjQvTz/HHL8olztrjWbppqKwIgPqA1QWeCpnfALWBulcoTtyg76yhQjj0ShuHib1xV0/4=", m.sig);
    TEST_ASSERT_EQUAL_STRING("test-only-1", m.keyId);
    TEST_ASSERT_EQUAL_STRING("key_id", parse(withFields(",\"sig\":\"MEUCIQCHz8IEMOCjIStzZFpynuIjQvTz/HHL8olztrjWbppqKwIgPqA1QWeCpnfALWBulcoTtyg76yhQjj0ShuHib1xV0/4=\""), m));
    TEST_ASSERT_EQUAL_STRING("sig", parse(withFields(",\"key_id\":\"test-only-1\""), m));
    TEST_ASSERT_EQUAL_STRING("sig", parse(withFields(",\"sig\":\"QUJ?RA==\",\"key_id\":\"test-only-1\""), m));
    TEST_ASSERT_EQUAL_STRING("sig", parse(withFields(",\"sig\":\"QUJDREVGR0g=\",\"key_id\":\"test-only-1\""), m));
    TEST_ASSERT_EQUAL_STRING("key_id", parse(withFields(",\"sig\":\"MEUCIQCHz8IEMOCjIStzZFpynuIjQvTz/HHL8olztrjWbppqKwIgPqA1QWeCpnfALWBulcoTtyg76yhQjj0ShuHib1xV0/4=\",\"key_id\":\"TEST_only\""), m));
    TEST_ASSERT_EQUAL_STRING("", m.version);
}

void test_install_permission_matrix(void) {
    for (int sig = 0; sig < 2; ++sig)
        for (int known = 0; known < 2; ++known)
            for (int optIn = 0; optIn < 2; ++optIn)
                TEST_ASSERT_EQUAL((sig && known) || (!sig && optIn),
                                  installPermitted(sig, known, optIn));
}

void test_utc_timestamps(void) {
    uint32_t t = 1;
    TEST_ASSERT_TRUE(parseUtc("1970-01-01T00:00:00Z", t));
    TEST_ASSERT_EQUAL_UINT32(0, t);
    TEST_ASSERT_TRUE(parseUtc("2000-03-01T00:00:00Z", t));
    TEST_ASSERT_EQUAL_UINT32(951868800u, t);
    TEST_ASSERT_TRUE(parseUtc("2000-02-29T23:59:59Z", t));
    TEST_ASSERT_EQUAL_UINT32(951868799u, t);
    TEST_ASSERT_TRUE(parseUtc("2026-09-24T12:26:47Z", t));
    TEST_ASSERT_EQUAL_UINT32(1790252807u, t);
    TEST_ASSERT_FALSE(parseUtc("2100-02-29T00:00:00Z", t));
    TEST_ASSERT_FALSE(parseUtc("1969-12-31T23:59:59Z", t));
    TEST_ASSERT_FALSE(parseUtc("2026-13-01T00:00:00Z", t));
    TEST_ASSERT_FALSE(parseUtc("2026-09-24T24:00:00Z", t));
    TEST_ASSERT_FALSE(parseUtc("2026-09-24T12:26:47", t));
    TEST_ASSERT_FALSE(parseUtc(nullptr, t));
}

// ---- gates ---------------------------------------------------------------------

static Manifest validManifest() {
    Manifest m;
    parse(manifestJson(), m);
    return m;
}

void test_gate_accepts_a_matching_offer(void) {
    const BoardInfo::Info b = board(1, 2);
    Context ctx;
    ctx.board = &b;
    TEST_ASSERT_EQUAL(Verdict::Ok, gate(validManifest(), ctx));
    ctx.wanted = "1.4.0";
    TEST_ASSERT_EQUAL(Verdict::Ok, gate(validManifest(), ctx));
    // An unprogrammed board compares as 1.2.
    ctx.board = nullptr;
    TEST_ASSERT_EQUAL(Verdict::Ok, gate(validManifest(), ctx));
}

void test_gate_hardware_range_runs_first(void) {
    const BoardInfo::Info b = board(2, 0);
    Context ctx;
    ctx.board = &b;
    Manifest m = validManifest();
    TEST_ASSERT_EQUAL(Verdict::HwIncompatible, gate(m, ctx));
    strcpy(m.source, "fork:a/b");   // also the wrong source: hardware wins
    TEST_ASSERT_EQUAL(Verdict::HwIncompatible, gate(m, ctx));
    m = validManifest();
    strcpy(m.minRev, "2.0");
    strcpy(m.maxRev, "1.0");
    TEST_ASSERT_EQUAL(Verdict::Malformed, gate(m, ctx));
    TEST_ASSERT_EQUAL_STRING("This update isn't made for this Fidget.", verdictCopy(Verdict::HwIncompatible));
}

void test_gate_source_and_acknowledgment(void) {
    const BoardInfo::Info b = board(1, 2);
    Context ctx;
    ctx.board = &b;
    Manifest m = validManifest();
    strcpy(m.source, "fork:someone/fw");
    TEST_ASSERT_EQUAL(Verdict::WrongSource, gate(m, ctx));   // device follows official
    ctx.source = "fork:someone/fw";
    TEST_ASSERT_EQUAL(Verdict::SourceNotAcknowledged, gate(m, ctx));
    ctx.sourceAcknowledged = true;
    TEST_ASSERT_EQUAL(Verdict::Ok, gate(m, ctx));
    // An official manifest does not satisfy a fork setting.
    TEST_ASSERT_EQUAL(Verdict::WrongSource, gate(validManifest(), ctx));
    // The stored display default is the official source.
    ctx = Context();
    ctx.board = &b;
    ctx.source = "cyberfidget.com";
    TEST_ASSERT_EQUAL(Verdict::Ok, gate(validManifest(), ctx));
}

void test_gate_channel(void) {
    const BoardInfo::Info b = board(1, 2);
    Context ctx;
    ctx.board = &b;
    Manifest rc = validManifest();
    strcpy(rc.channel, "rc");
    TEST_ASSERT_EQUAL(Verdict::WrongChannel, gate(rc, ctx));   // stable device
    ctx.channel = "rc";
    TEST_ASSERT_EQUAL(Verdict::Ok, gate(rc, ctx));
    TEST_ASSERT_EQUAL(Verdict::Ok, gate(validManifest(), ctx));   // rc device takes stable
    ctx.channel = "nonsense";   // reads as stable
    TEST_ASSERT_EQUAL(Verdict::WrongChannel, gate(rc, ctx));
}

void test_gate_freshness_size_and_choice(void) {
    const BoardInfo::Info b = board(1, 2);
    Context ctx;
    ctx.board = &b;
    const Manifest m = validManifest();
    ctx.seenTs = m.releasedAt + 1;
    TEST_ASSERT_EQUAL(Verdict::Stale, gate(m, ctx));
    ctx.seenTs = m.releasedAt;   // the same release again (a retry) is allowed
    TEST_ASSERT_EQUAL(Verdict::Ok, gate(m, ctx));
    ctx.seenTs = 0;
    Manifest big = m;
    big.size = kSlotSize + 1;
    TEST_ASSERT_EQUAL(Verdict::TooLarge, gate(big, ctx));
    ctx.wanted = "1.4.1";
    TEST_ASSERT_EQUAL(Verdict::NotWanted, gate(m, ctx));
    ctx.wanted = "1.4.0+abc";   // exact text only
    TEST_ASSERT_EQUAL(Verdict::NotWanted, gate(m, ctx));
}

void test_every_verdict_has_a_name_and_plain_copy(void) {
    const Verdict all[] = {Verdict::Malformed, Verdict::HwIncompatible, Verdict::WrongSource,
                           Verdict::SourceNotAcknowledged, Verdict::WrongChannel, Verdict::Stale,
                           Verdict::TooLarge, Verdict::NotWanted};
    for (Verdict v : all) {
        TEST_ASSERT_TRUE(strlen(verdictName(v)) > 0);
        const char* copy = verdictCopy(v);
        TEST_ASSERT_TRUE(strlen(copy) > 0);
        const char* banned[] = {"OTA", "WiFi", "firmware", "partition", "image", "manifest"};
        for (const char* word : banned) TEST_ASSERT_NULL_MESSAGE(strstr(copy, word), copy);
    }
}

void test_seen_keys_are_short_and_scoped(void) {
    char a[kSeenKeyLen + 1], b[kSeenKeyLen + 1], c[kSeenKeyLen + 1], d[kSeenKeyLen + 1];
    seenKey("", "", a);
    TEST_ASSERT_EQUAL_UINT(13, strlen(a));
    TEST_ASSERT_TRUE(strlen(a) <= 15);
    TEST_ASSERT_EQUAL_INT(0, strncmp(a, "seen_", 5));
    seenKey("official", "stable", b);
    TEST_ASSERT_EQUAL_STRING(a, b);
    seenKey("cyberfidget.com", "", b);
    TEST_ASSERT_EQUAL_STRING(a, b);
    seenKey("official", "rc", c);
    TEST_ASSERT_NOT_EQUAL(0, strcmp(a, c));
    seenKey("fork:x/y", "stable", d);
    TEST_ASSERT_NOT_EQUAL(0, strcmp(a, d));
    TEST_ASSERT_NOT_EQUAL(0, strcmp(c, d));
}

void test_only_an_unanswering_host_falls_back(void) {
    TEST_ASSERT_TRUE(fallbackAllowed(FetchOutcome::Transport));
    TEST_ASSERT_TRUE(fallbackAllowed(FetchOutcome::ServerError));
    TEST_ASSERT_FALSE(fallbackAllowed(FetchOutcome::Ok));
    TEST_ASSERT_FALSE(fallbackAllowed(FetchOutcome::Refused));
    TEST_ASSERT_FALSE(fallbackAllowed(FetchOutcome::BadManifest));
    TEST_ASSERT_FALSE(fallbackAllowed(FetchOutcome::GateRefused));
}

// ---- install order -------------------------------------------------------------

// Not SHA-256 (the device uses mbedTLS); any digest proves the ORDER and the
// comparison, which are what these tests are about.
struct FakeHasher : Hasher {
    uint8_t state[32];
    uint32_t n = 0;
    void start() override { memset(state, 0, sizeof(state)); n = 0; }
    void update(const uint8_t* d, size_t len) override {
        for (size_t i = 0; i < len; ++i, ++n) state[n % 32] = (uint8_t)(state[n % 32] * 31 + d[i] + n);
    }
    void finish(uint8_t out[32]) override { memcpy(out, state, 32); }
};

static std::string fakeDigestHex(const std::vector<uint8_t>& bytes) {
    FakeHasher h;
    h.start();
    h.update(bytes.data(), bytes.size());
    uint8_t d[32];
    h.finish(d);
    char hex[65];
    for (int i = 0; i < 32; ++i) snprintf(hex + i * 2, 3, "%02x", d[i]);
    return hex;
}

struct FakeTarget : Target {
    std::vector<std::string> calls;
    std::vector<uint8_t> written;
    std::string record;
    bool beginOk = true, writeOk = true, endOk = true, persistOk = true, bootOk = true;
    bool begin(uint32_t) override { calls.push_back("begin"); return beginOk; }
    bool write(const uint8_t* d, size_t len) override {
        calls.push_back("write");
        if (!writeOk) return false;
        written.insert(written.end(), d, d + len);
        return true;
    }
    bool end() override { calls.push_back("end"); return endOk; }
    void abort() override { calls.push_back("abort"); }
    bool persistPending(const char* r) override { calls.push_back("persist"); record = r; return persistOk; }
    bool selectBoot() override { calls.push_back("boot"); return bootOk; }
    bool called(const char* name) const {
        for (const std::string& c : calls) if (c == name) return true;
        return false;
    }
    int index(const char* name) const {
        for (size_t i = 0; i < calls.size(); ++i) if (calls[i] == name) return (int)i;
        return -1;
    }
};

static std::vector<uint8_t> image(size_t n) {
    std::vector<uint8_t> v(n);
    for (size_t i = 0; i < n; ++i) v[i] = (uint8_t)(i * 7 + 3);
    return v;
}

static Manifest manifestFor(const std::vector<uint8_t>& img) {
    Manifest m = validManifest();
    m.size = (uint32_t)img.size();
    strcpy(m.sha256, fakeDigestHex(img).c_str());
    return m;
}

static const char* kSeen = "seen_1234abcd";

static InstallResult stream(Installer& inst, const std::vector<uint8_t>& img, size_t chunk = 100) {
    for (size_t off = 0; off < img.size(); off += chunk) {
        const size_t n = img.size() - off < chunk ? img.size() - off : chunk;
        if (!inst.feed(img.data() + off, n)) return inst.result();
    }
    return inst.complete();
}

struct FakeVerifier : Verifier {
    VerifyResult answer = VerifyResult::Ok;
    int calls = 0;
    VerifyResult verify(const Manifest&, const uint8_t[32]) override { ++calls; return answer; }
};

void test_verification_refuses_bad_and_unknown_even_with_opt_in(void) {
    const auto img = image(32);
    Manifest m = manifestFor(img);
    strcpy(m.sig, "MEUCIQCHz8IEMOCjIStzZFpynuIjQvTz/HHL8olztrjWbppqKwIgPqA1QWeCpnfALWBulcoTtyg76yhQjj0ShuHib1xV0/4=");
    strcpy(m.keyId, "test-only-1");
    for (const VerifyResult refused : {VerifyResult::Bad, VerifyResult::UnknownKey}) {
        FakeTarget t;
        FakeHasher h;
        FakeVerifier v;
        v.answer = refused;
        Installer inst(t, h, &v, true);
        TEST_ASSERT_TRUE(inst.begin(m, kSeen));
        TEST_ASSERT_EQUAL(InstallResult::VerificationFailed, stream(inst, img));
        TEST_ASSERT_EQUAL_INT(1, v.calls);
        TEST_ASSERT_TRUE(t.called("abort"));
        TEST_ASSERT_FALSE(t.called("end"));
        TEST_ASSERT_FALSE(t.called("boot"));
    }
    FakeTarget t;
    FakeHasher h;
    FakeVerifier v;
    v.answer = VerifyResult::Ok;
    Installer inst(t, h, &v, false);
    TEST_ASSERT_TRUE(inst.begin(m, kSeen));
    TEST_ASSERT_EQUAL(InstallResult::Ready, stream(inst, img));
}

void test_unsigned_install_needs_opt_in(void) {
    const auto img = image(32);
    FakeTarget t;
    FakeHasher h;
    FakeVerifier v;
    v.answer = VerifyResult::Unsigned;
    Installer inst(t, h, &v, false);
    TEST_ASSERT_TRUE(inst.begin(manifestFor(img), kSeen));
    TEST_ASSERT_EQUAL(InstallResult::VerificationFailed, stream(inst, img));
    TEST_ASSERT_FALSE(t.called("boot"));
}

void test_hash_mismatch_never_reaches_signature_check(void) {
    const auto img = image(32);
    Manifest m = manifestFor(img);
    strcpy(m.sig, "MEUCIQCHz8IEMOCjIStzZFpynuIjQvTz/HHL8olztrjWbppqKwIgPqA1QWeCpnfALWBulcoTtyg76yhQjj0ShuHib1xV0/4=");
    strcpy(m.keyId, "test-only-1");
    FakeTarget t;
    FakeHasher h;
    FakeVerifier v;
    Installer inst(t, h, &v, false);
    TEST_ASSERT_TRUE(inst.begin(m, kSeen));
    auto changed = img;
    changed[0] ^= 1;
    TEST_ASSERT_EQUAL(InstallResult::HashMismatch, stream(inst, changed));
    TEST_ASSERT_EQUAL_INT(0, v.calls);
    TEST_ASSERT_FALSE(t.called("boot"));
}

void test_install_verifies_then_records_then_selects_boot(void) {
    const std::vector<uint8_t> img = image(1000);
    const Manifest m = manifestFor(img);
    FakeTarget t;
    FakeHasher h;
    Installer inst(t, h);
    TEST_ASSERT_TRUE(inst.begin(m, kSeen));
    TEST_ASSERT_EQUAL(InstallResult::Ready, stream(inst, img));
    TEST_ASSERT_TRUE(t.written == img);
    TEST_ASSERT_FALSE(t.called("abort"));
    // end -> persist -> boot, in that order, after every write.
    TEST_ASSERT_TRUE(t.index("end") > 0);
    TEST_ASSERT_TRUE(t.index("persist") > t.index("end"));
    TEST_ASSERT_TRUE(t.index("boot") > t.index("persist"));
    TEST_ASSERT_EQUAL_STRING("boot", t.calls.back().c_str());
    Pending p;
    TEST_ASSERT_TRUE(parsePending(t.record.c_str(), p));
    TEST_ASSERT_EQUAL_STRING(m.sha256, p.sha256);
    TEST_ASSERT_EQUAL_UINT32(1000, p.size);
    TEST_ASSERT_EQUAL_UINT32(m.releasedAt, p.releasedAt);
    TEST_ASSERT_EQUAL_STRING(kSeen, p.seenKey);
    TEST_ASSERT_EQUAL_STRING("1.4.0", p.version);
    TEST_ASSERT_EQUAL_UINT8(100, inst.percent());
}

void test_hash_mismatch_aborts_before_finish(void) {
    std::vector<uint8_t> img = image(1000);
    const Manifest m = manifestFor(img);
    img[500] ^= 0x01;   // one flipped bit in transit
    FakeTarget t;
    FakeHasher h;
    Installer inst(t, h);
    TEST_ASSERT_TRUE(inst.begin(m, kSeen));
    TEST_ASSERT_EQUAL(InstallResult::HashMismatch, stream(inst, img));
    TEST_ASSERT_TRUE(t.called("abort"));
    TEST_ASSERT_FALSE(t.called("end"));
    TEST_ASSERT_FALSE(t.called("persist"));
    TEST_ASSERT_FALSE(t.called("boot"));
}

void test_wrong_manifest_digest_aborts(void) {
    const std::vector<uint8_t> img = image(1000);
    Manifest m = manifestFor(img);
    strcpy(m.sha256, kSha);   // the manifest's digest is simply wrong
    FakeTarget t;
    FakeHasher h;
    Installer inst(t, h);
    TEST_ASSERT_TRUE(inst.begin(m, kSeen));
    TEST_ASSERT_EQUAL(InstallResult::HashMismatch, stream(inst, img));
    TEST_ASSERT_FALSE(t.called("boot"));
    TEST_ASSERT_FALSE(t.called("end"));
}

void test_short_and_long_streams_abort(void) {
    const std::vector<uint8_t> img = image(1000);
    const Manifest m = manifestFor(img);
    {
        FakeTarget t;
        FakeHasher h;
        Installer inst(t, h);
        TEST_ASSERT_TRUE(inst.begin(m, kSeen));
        const std::vector<uint8_t> shortImg(img.begin(), img.begin() + 999);
        TEST_ASSERT_EQUAL(InstallResult::Short, stream(inst, shortImg));
        TEST_ASSERT_TRUE(t.called("abort"));
        TEST_ASSERT_FALSE(t.called("end"));
        TEST_ASSERT_FALSE(t.called("boot"));
    }
    {
        FakeTarget t;
        FakeHasher h;
        Installer inst(t, h);
        TEST_ASSERT_TRUE(inst.begin(m, kSeen));
        std::vector<uint8_t> longImg = img;
        longImg.push_back(0);
        TEST_ASSERT_EQUAL(InstallResult::TooLong, stream(inst, longImg, 1001));
        TEST_ASSERT_TRUE(t.called("abort"));
        TEST_ASSERT_TRUE(t.written.empty());   // the oversize piece is never written
        TEST_ASSERT_FALSE(inst.feed(img.data(), 1));   // over: nothing more is taken
        TEST_ASSERT_EQUAL(InstallResult::TooLong, inst.complete());
        TEST_ASSERT_FALSE(t.called("boot"));
    }
}

void test_each_target_failure_stops_before_boot_selection(void) {
    const std::vector<uint8_t> img = image(600);
    const Manifest m = manifestFor(img);
    {
        FakeTarget t;
        t.beginOk = false;
        FakeHasher h;
        Installer inst(t, h);
        TEST_ASSERT_FALSE(inst.begin(m, kSeen));
        TEST_ASSERT_EQUAL(InstallResult::BeginFailed, inst.result());
        TEST_ASSERT_FALSE(inst.feed(img.data(), 10));
        TEST_ASSERT_FALSE(t.called("abort"));   // nothing was begun
    }
    {
        FakeTarget t;
        t.writeOk = false;
        FakeHasher h;
        Installer inst(t, h);
        TEST_ASSERT_TRUE(inst.begin(m, kSeen));
        TEST_ASSERT_EQUAL(InstallResult::WriteFailed, stream(inst, img));
        TEST_ASSERT_TRUE(t.called("abort"));
        TEST_ASSERT_FALSE(t.called("boot"));
    }
    {
        FakeTarget t;
        t.endOk = false;
        FakeHasher h;
        Installer inst(t, h);
        TEST_ASSERT_TRUE(inst.begin(m, kSeen));
        TEST_ASSERT_EQUAL(InstallResult::ImageInvalid, stream(inst, img));
        TEST_ASSERT_FALSE(t.called("persist"));
        TEST_ASSERT_FALSE(t.called("boot"));
    }
    {
        FakeTarget t;
        t.persistOk = false;
        FakeHasher h;
        Installer inst(t, h);
        TEST_ASSERT_TRUE(inst.begin(m, kSeen));
        TEST_ASSERT_EQUAL(InstallResult::StoreFailed, stream(inst, img));
        TEST_ASSERT_FALSE(t.called("boot"));   // no record, no new boot slot
    }
    {
        FakeTarget t;
        t.bootOk = false;
        FakeHasher h;
        Installer inst(t, h);
        TEST_ASSERT_TRUE(inst.begin(m, kSeen));
        TEST_ASSERT_EQUAL(InstallResult::BootFailed, stream(inst, img));
        TEST_ASSERT_TRUE(t.called("persist"));
    }
}

void test_abort_mid_stream_drops_the_image_once(void) {
    const std::vector<uint8_t> img = image(600);
    const Manifest m = manifestFor(img);
    FakeTarget t;
    FakeHasher h;
    Installer inst(t, h);
    TEST_ASSERT_TRUE(inst.begin(m, kSeen));
    TEST_ASSERT_TRUE(inst.feed(img.data(), 300));
    TEST_ASSERT_EQUAL_UINT8(50, inst.percent());
    inst.abort();
    inst.abort();
    int aborts = 0;
    for (const std::string& c : t.calls) aborts += c == "abort";
    TEST_ASSERT_EQUAL_INT(1, aborts);
    TEST_ASSERT_EQUAL(InstallResult::Aborted, inst.complete());
    TEST_ASSERT_FALSE(t.called("end"));
    TEST_ASSERT_FALSE(t.called("boot"));
}

void test_install_refuses_a_bad_seen_key(void) {
    const std::vector<uint8_t> img = image(10);
    FakeTarget t;
    FakeHasher h;
    Installer inst(t, h);
    TEST_ASSERT_FALSE(inst.begin(manifestFor(img), "seen_1"));
    TEST_ASSERT_FALSE(t.called("begin"));
}

// ---- pending record ------------------------------------------------------------

void test_pending_record_round_trip_and_refusals(void) {
    Pending p;
    strcpy(p.sha256, kSha);
    p.size = 2707200;
    p.releasedAt = 1790252807u;
    strcpy(p.seenKey, "seen_0badf00d");
    strcpy(p.version, "1.4.0+abc1234.dirty");
    char text[160];
    TEST_ASSERT_TRUE(formatPending(p, text, sizeof(text)));
    Pending q;
    TEST_ASSERT_TRUE(parsePending(text, q));
    TEST_ASSERT_EQUAL_STRING(p.sha256, q.sha256);
    TEST_ASSERT_EQUAL_UINT32(p.size, q.size);
    TEST_ASSERT_EQUAL_UINT32(p.releasedAt, q.releasedAt);
    TEST_ASSERT_EQUAL_STRING(p.seenKey, q.seenKey);
    TEST_ASSERT_EQUAL_STRING(p.version, q.version);

    TEST_ASSERT_FALSE(formatPending(p, text, 20));   // does not fit: refused
    Pending bad = p;
    strcpy(bad.version, "1.4 0");
    TEST_ASSERT_FALSE(formatPending(bad, text, sizeof(text)));

    const char* refused[] = {
        "",
        "garbage",
        "0123456789ABCDEF0123456789abcdef0123456789abcdef0123456789abcdef 10 5 seen_0badf00d 1.4.0",
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef 0 5 seen_0badf00d 1.4.0",
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef 3342337 5 seen_0badf00d 1.4.0",
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef 10 5 seen_0badf0 1.4.0",
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef 10 5 seen_0badf00d ",
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef 10 5 seen_0badf00d 1.4 .0",
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef -10 5 seen_0badf00d 1.4.0",
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef 10 5 seen_0BADF00D 1.4.0",
    };
    for (const char* r : refused) {
        Pending out;
        TEST_ASSERT_FALSE_MESSAGE(parsePending(r, out), r);
        TEST_ASSERT_EQUAL_STRING_MESSAGE("", out.version, r);
    }
}

// ---- after the restart ---------------------------------------------------------

void test_version_match_rules(void) {
    TEST_ASSERT_TRUE(versionMatches("1.4.0+abc1234.dirty", "1.4.0+abc1234.dirty"));
    TEST_ASSERT_TRUE(versionMatches("1.4.0", "1.4.0+abc1234"));
    TEST_ASSERT_TRUE(versionMatches("1.4.0-rc1", "1.4.0-rc1+abc1234"));
    TEST_ASSERT_TRUE(versionMatches("1.4.0", "1.4.0"));
    TEST_ASSERT_FALSE(versionMatches("1.4.0", "1.4.0-rc1+abc1234"));
    TEST_ASSERT_FALSE(versionMatches("1.4.0", "1.4.01+abc"));
    TEST_ASSERT_FALSE(versionMatches("1.4.0+abc1234", "1.4.0+abc1235"));
    TEST_ASSERT_FALSE(versionMatches("1.4.0+abc1234", "1.4.0"));
    TEST_ASSERT_FALSE(versionMatches("1.4.0", "1.3.3+abc1234"));
    TEST_ASSERT_FALSE(versionMatches("", "1.4.0"));
    TEST_ASSERT_FALSE(versionMatches(nullptr, "1.4.0"));
    TEST_ASSERT_FALSE(versionMatches("1.4.0", nullptr));
}

void test_self_test_needs_every_check(void) {
    SelfTestInputs all;
    all.recordValid = all.halOk = all.fsMounted = all.versionOk = all.imageOk = all.inTime = true;
    SelfTestResult r = decideSelfTest(all);
    TEST_ASSERT_TRUE(r.markValid);
    TEST_ASSERT_EQUAL_STRING("ok", r.reason);

    struct Case { bool SelfTestInputs::*field; const char* reason; };
    const Case cases[] = {
        {&SelfTestInputs::recordValid, "record"},
        {&SelfTestInputs::halOk, "hal"},
        {&SelfTestInputs::fsMounted, "filesystem"},
        {&SelfTestInputs::versionOk, "version"},
        {&SelfTestInputs::imageOk, "image"},
        {&SelfTestInputs::inTime, "deadline"},
    };
    for (const Case& c : cases) {
        SelfTestInputs in = all;
        in.*c.field = false;
        r = decideSelfTest(in);
        TEST_ASSERT_FALSE_MESSAGE(r.markValid, c.reason);
        TEST_ASSERT_EQUAL_STRING(c.reason, r.reason);
    }
    // Nothing known (the defaults) rolls back.
    TEST_ASSERT_FALSE(decideSelfTest(SelfTestInputs()).markValid);
}

void test_boot_notice_after_a_normal_boot(void) {
    TEST_ASSERT_EQUAL(BootNotice::None, bootNotice(false, false, false));
    TEST_ASSERT_EQUAL(BootNotice::Completed, bootNotice(true, true, true));
    TEST_ASSERT_EQUAL(BootNotice::DidNotFinish, bootNotice(true, true, false));
    TEST_ASSERT_EQUAL(BootNotice::DidNotFinish, bootNotice(true, false, false));
    TEST_ASSERT_EQUAL(BootNotice::DidNotFinish, bootNotice(true, false, true));
}

void test_nvs_keys_fit(void) {
    // Every key the update work stores, in every namespace.
    const char* keys[] = {kKeyPendImg, kKeyUnsigOk, kKeyFailVer, kKeyPowerAbort, kKeyAvail,
                          kBootSession, kBootVersion, kBootFailed, kTestFault};
    for (const char* k : keys) {
        TEST_ASSERT_TRUE_MESSAGE(strlen(k) > 0 && strlen(k) <= kMaxNvsKeyLen, k);
    }
    // Freshness keys for any source (the longest fork name included) and channel.
    char seen[kSeenKeyLen + 1];
    std::string longFork = "fork:" + std::string(39, 'a') + "/" + std::string(100, 'b');
    const char* sources[] = {"", "official", "cyberfidget.com", longFork.c_str()};
    const char* channels[] = {"", "stable", "rc"};
    for (const char* s : sources) {
        for (const char* c : channels) {
            seenKey(s, c, seen);
            TEST_ASSERT_TRUE(strlen(seen) <= kMaxNvsKeyLen);
            TEST_ASSERT_EQUAL_UINT(kSeenKeyLen, strlen(seen));
        }
    }
    TEST_ASSERT_TRUE(kSeenKeyLen <= kMaxNvsKeyLen);
}

void test_time_limits_can_each_fire(void) {
    // The hardware start-up budget is shorter than the pending watchdog
    // period, so it is a real check rather than a dead branch; the session's
    // watchdog outlasts any one blocking call; checks end before the frame
    // budget.
    TEST_ASSERT_TRUE(kHalBudgetMs < kPendingWdtMs);
    TEST_ASSERT_TRUE(kSelfTestBudgetMs < kConfirmBudgetMs);
    TEST_ASSERT_TRUE(kSessionCallMs < kSessionWdtMs);
    TEST_ASSERT_TRUE(kSessionWdtMs < kSessionBudgetMs);
}

void test_self_test_deadline_expiry_rolls_back(void) {
    SelfTestInputs in;
    in.recordValid = in.halOk = in.fsMounted = in.versionOk = in.imageOk = true;
    in.inTime = false;   // the device sets this from kSelfTestBudgetMs
    const SelfTestResult r = decideSelfTest(in);
    TEST_ASSERT_FALSE(r.markValid);
    TEST_ASSERT_EQUAL_STRING("deadline", r.reason);
}

void test_confirm_only_after_the_first_frame(void) {
    // Checks passed, no frame yet: wait (not kept).
    TEST_ASSERT_EQUAL(ConfirmStep::Wait, confirmStep(true, false, 1000, kConfirmBudgetMs));
    // First frame drawn in time: keep.
    TEST_ASSERT_EQUAL(ConfirmStep::Confirm, confirmStep(true, true, 1000, kConfirmBudgetMs));
    TEST_ASSERT_EQUAL(ConfirmStep::Confirm, confirmStep(true, true, kConfirmBudgetMs, kConfirmBudgetMs));
    // Budget gone before the frame (or even with it): roll back.
    TEST_ASSERT_EQUAL(ConfirmStep::RollBack, confirmStep(true, false, kConfirmBudgetMs + 1, kConfirmBudgetMs));
    TEST_ASSERT_EQUAL(ConfirmStep::RollBack, confirmStep(true, true, kConfirmBudgetMs + 1, kConfirmBudgetMs));
    // Checks not passed: never kept, frame or not.
    TEST_ASSERT_EQUAL(ConfirmStep::RollBack, confirmStep(false, true, 10, kConfirmBudgetMs));
}

void test_pending_sleep_waits_or_handles_critical_voltage(void) {
    TEST_ASSERT_EQUAL(SleepStep::Proceed, sleepStep(false, false, false));
    TEST_ASSERT_EQUAL(SleepStep::Proceed, sleepStep(false, true, true));
    TEST_ASSERT_EQUAL(SleepStep::Defer, sleepStep(true, false, false));
    TEST_ASSERT_EQUAL(SleepStep::Defer, sleepStep(true, true, false));
    TEST_ASSERT_EQUAL(SleepStep::AbortWithoutFailure, sleepStep(true, false, true));
    TEST_ASSERT_EQUAL(SleepStep::KeepFirst, sleepStep(true, true, true));
}

void test_critical_power_abort_is_not_a_failed_boot_notice(void) {
    TEST_ASSERT_EQUAL(BootNotice::DidNotFinish, bootNotice(true, true, false));
    TEST_ASSERT_EQUAL(BootNotice::None, bootNotice(true, true, false, true));
    TEST_ASSERT_EQUAL(BootNotice::None, bootNotice(true, false, false, true));
}

void test_a_failed_version_is_not_offered_automatically(void) {
    TEST_ASSERT_TRUE(automaticOfferAllowed("1.4.0", ""));
    TEST_ASSERT_TRUE(automaticOfferAllowed("1.4.0", nullptr));
    TEST_ASSERT_FALSE(automaticOfferAllowed("1.4.0", "1.4.0"));
    TEST_ASSERT_TRUE(automaticOfferAllowed("1.4.1", "1.4.0"));   // a newer one is offered
    TEST_ASSERT_FALSE(automaticOfferAllowed("", ""));
}

void setUp(void) {}
void tearDown(void) {}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_parses_the_site_manifest);
    RUN_TEST(test_every_field_is_required_and_validated);
    RUN_TEST(test_fork_source_and_rc_channel_parse);
    RUN_TEST(test_optional_signature_pair_is_strict);
    RUN_TEST(test_install_permission_matrix);
    RUN_TEST(test_utc_timestamps);
    RUN_TEST(test_gate_accepts_a_matching_offer);
    RUN_TEST(test_gate_hardware_range_runs_first);
    RUN_TEST(test_gate_source_and_acknowledgment);
    RUN_TEST(test_gate_channel);
    RUN_TEST(test_gate_freshness_size_and_choice);
    RUN_TEST(test_every_verdict_has_a_name_and_plain_copy);
    RUN_TEST(test_seen_keys_are_short_and_scoped);
    RUN_TEST(test_only_an_unanswering_host_falls_back);
    RUN_TEST(test_install_verifies_then_records_then_selects_boot);
    RUN_TEST(test_verification_refuses_bad_and_unknown_even_with_opt_in);
    RUN_TEST(test_unsigned_install_needs_opt_in);
    RUN_TEST(test_hash_mismatch_never_reaches_signature_check);
    RUN_TEST(test_hash_mismatch_aborts_before_finish);
    RUN_TEST(test_wrong_manifest_digest_aborts);
    RUN_TEST(test_short_and_long_streams_abort);
    RUN_TEST(test_each_target_failure_stops_before_boot_selection);
    RUN_TEST(test_abort_mid_stream_drops_the_image_once);
    RUN_TEST(test_install_refuses_a_bad_seen_key);
    RUN_TEST(test_pending_record_round_trip_and_refusals);
    RUN_TEST(test_version_match_rules);
    RUN_TEST(test_self_test_needs_every_check);
    RUN_TEST(test_boot_notice_after_a_normal_boot);
    RUN_TEST(test_nvs_keys_fit);
    RUN_TEST(test_time_limits_can_each_fire);
    RUN_TEST(test_self_test_deadline_expiry_rolls_back);
    RUN_TEST(test_confirm_only_after_the_first_frame);
    RUN_TEST(test_pending_sleep_waits_or_handles_critical_voltage);
    RUN_TEST(test_critical_power_abort_is_not_a_failed_boot_notice);
    RUN_TEST(test_a_failed_version_is_not_offered_automatically);
    return UNITY_END();
}
