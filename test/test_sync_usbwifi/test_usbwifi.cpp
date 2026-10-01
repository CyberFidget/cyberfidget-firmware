// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

// The USB serial WiFi setup verbs (`wifi scan|add|try|saved`): the framed
// `wifi add` payload, names as hex, the scan list, who owns the radio, the
// reply lines - and that no reply ever carries a password. Also pins, from
// the serial driver's source, which verbs a release build answers and where
// `syncinfo.setup` sits.

#include <unity.h>
#include <algorithm>
#include <map>
#include <string>
#include <vector>
#include <stdio.h>
#include <string.h>

#include "SyncProtocol.h"
#include "UsbWifi.h"
#include "WifiList.h"

using namespace UsbWifi;

namespace {

std::vector<uint8_t> frameOf(const std::string& name, const std::string& pass) {
    std::vector<uint8_t> f(name.begin(), name.end());
    f.push_back(0);
    f.insert(f.end(), pass.begin(), pass.end());
    return f;
}

uint32_t crcOf(const std::vector<uint8_t>& f) { return SyncProtocol::crc32(f.data(), f.size()); }

std::string hexOf(const std::string& s) {
    char out[256];
    toHex(reinterpret_cast<const uint8_t*>(s.data()), s.size(), out, sizeof(out));
    return out;
}

struct Parsed {
    Frame frame;
    std::string name;
    std::string pass;
};

Parsed parse(const std::vector<uint8_t>& f, uint32_t crc) {
    char name[WifiList::kNameMax + 1];
    char pass[WifiList::kPassMax + 1];
    memset(name, 'x', sizeof(name));
    memset(pass, 'x', sizeof(pass));
    Parsed p;
    p.frame = parseAddFrame(f.data(), f.size(), crc, name, pass);
    // Always NUL-terminated.
    TEST_ASSERT_EQUAL_CHAR('\0', name[WifiList::kNameMax]);
    TEST_ASSERT_EQUAL_CHAR('\0', pass[WifiList::kPassMax]);
    p.name = name;
    p.pass = pass;
    return p;
}

Parsed parse(const std::vector<uint8_t>& f) { return parse(f, crcOf(f)); }

std::string addReply(Frame frame, WifiList::AddResult saved, const char* name) {
    char out[kLineMax];
    const size_t n = formatAddReply(out, sizeof(out), frame, saved, name);
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_EQUAL_size_t(strlen(out), n);
    return out;
}

// The bytes of `secret`, raw or as hex in either case, are nowhere in `text`.
void assertNoSecret(const std::string& text, const std::string& secret) {
    if (secret.empty()) return;
    TEST_ASSERT_TRUE_MESSAGE(text.find(secret) == std::string::npos, text.c_str());
    std::string hex = hexOf(secret);
    TEST_ASSERT_TRUE_MESSAGE(text.find(hex) == std::string::npos, text.c_str());
    for (char& c : hex) if (c >= 'a' && c <= 'f') c = (char)(c - 'a' + 'A');
    TEST_ASSERT_TRUE_MESSAGE(text.find(hex) == std::string::npos, text.c_str());
}

// ---- the stored list, as the portal and `wifi add` both use it ------------------

class MemoryStore : public WifiList::Store {
public:
    std::map<std::string, std::string> values;
    bool has(const char* key) override { return values.count(key) != 0; }
    bool getString(const char* key, char* out, size_t len) override {
        out[0] = '\0';
        auto it = values.find(key);
        if (it == values.end()) return false;
        strncpy(out, it->second.c_str(), len - 1);
        out[len - 1] = '\0';
        return true;
    }
    uint32_t getUInt(const char* key) override {
        auto it = values.find(key);
        return it == values.end() ? 0 : (uint32_t)std::stoul(it->second);
    }
    bool putString(const char* key, const char* value) override { values[key] = value; return true; }
    bool putUInt(const char* key, uint32_t value) override { values[key] = std::to_string(value); return true; }
    bool remove(const char* key) override { values.erase(key); return true; }
};

// What the device does for one `wifi add` frame (SerialCli::cmdWifiAdd with
// SavedWifi::add): parse, save through the stored list, reply.
std::string usbAdd(MemoryStore& store, const std::string& name, const std::string& pass) {
    const std::vector<uint8_t> f = frameOf(name, pass);
    char n[WifiList::kNameMax + 1];
    char p[WifiList::kPassMax + 1];
    const Frame frame = parseAddFrame(f.data(), f.size(), crcOf(f), n, p);
    WifiList::AddResult saved = WifiList::AddResult::Invalid;
    if (frame == Frame::Ok) {
        WifiList::List list;
        bool repair = false;
        WifiList::load(store, list, repair);
        saved = WifiList::add(list, n, p);
        if (saved == WifiList::AddResult::Added || saved == WifiList::AddResult::Updated)
            TEST_ASSERT_TRUE(WifiList::save(store, list));
        WifiList::wipe(list);
    }
    wipe(p, sizeof(p));
    std::string reply = addReply(frame, saved, n);
    wipe(n, sizeof(n));
    return reply;
}

std::vector<std::string> savedNames(MemoryStore& store) {
    WifiList::List list;
    bool repair = false;
    WifiList::load(store, list, repair);
    std::vector<std::string> out;
    for (int i = 0; i < list.count; i++) out.push_back(list.nets[i].name);
    WifiList::wipe(list);
    return out;
}

// ---- the serial driver's source --------------------------------------------------

std::string readSource(const char* rel) {
    std::vector<std::string> tries;
    tries.push_back(rel);
    // From this file's own path, in case the runner starts elsewhere.
    std::string here = __FILE__;
    const size_t at = here.find("test");
    if (at != std::string::npos) tries.push_back(here.substr(0, at) + rel);
    for (const std::string& path : tries) {
        FILE* f = fopen(path.c_str(), "rb");
        if (!f) continue;
        std::string text;
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
        fclose(f);
        return text;
    }
    TEST_FAIL_MESSAGE(rel);
    return "";
}

// Each line of the source, with whether it is compiled only with CF_TEST_CLI.
struct SourceLine {
    std::string text;
    bool testOnly;
};

std::vector<SourceLine> classify(const std::string& source) {
    struct Cond { int kind; bool inElse; };   // kind: 1 ifdef CF_TEST_CLI, -1 ifndef, 0 other
    std::vector<Cond> stack;
    std::vector<SourceLine> out;
    size_t pos = 0;
    while (pos <= source.size()) {
        size_t end = source.find('\n', pos);
        if (end == std::string::npos) end = source.size();
        std::string line = source.substr(pos, end - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t i = line.find_first_not_of(" \t");
        const std::string t = i == std::string::npos ? "" : line.substr(i);
        auto starts = [&](const char* p) { return t.compare(0, strlen(p), p) == 0; };
        if (starts("#ifdef")) {
            stack.push_back({t.find("CF_TEST_CLI") != std::string::npos ? 1 : 0, false});
        } else if (starts("#ifndef")) {
            stack.push_back({t.find("CF_TEST_CLI") != std::string::npos ? -1 : 0, false});
        } else if (starts("#if")) {
            stack.push_back({t.find("defined(CF_TEST_CLI)") != std::string::npos ? 1 : 0, false});
        } else if (starts("#else") || starts("#elif")) {
            if (!stack.empty()) stack.back().inElse = true;
        } else if (starts("#endif")) {
            if (!stack.empty()) stack.pop_back();
        }
        bool testOnly = false;
        for (const Cond& c : stack)
            if ((c.kind == 1 && !c.inElse) || (c.kind == -1 && c.inElse)) testOnly = true;
        out.push_back({line, testOnly});
        if (end == source.size()) break;
        pos = end + 1;
    }
    return out;
}

int countWhere(const std::vector<SourceLine>& lines, const char* needle, bool testOnly) {
    int n = 0;
    for (const SourceLine& l : lines)
        if (l.testOnly == testOnly && l.text.find(needle) != std::string::npos) n++;
    return n;
}

// The body of `void SerialCli::<name>(` up to its closing brace at column 0.
std::string functionBody(const std::string& source, const char* signature) {
    const size_t at = source.find(signature);
    TEST_ASSERT_TRUE_MESSAGE(at != std::string::npos, signature);
    size_t end = source.find("\n}", at);
    TEST_ASSERT_TRUE(end != std::string::npos);
    return source.substr(at, end - at);
}

} // namespace

void setUp() {}
void tearDown() {}

// ---- wifi add: the frame -------------------------------------------------------------

void test_add_frame_good() {
    const Parsed p = parse(frameOf("Home Net", "correct horse"));
    TEST_ASSERT_EQUAL_INT((int)Frame::Ok, (int)(p.frame));
    TEST_ASSERT_EQUAL_STRING("Home Net", p.name.c_str());
    TEST_ASSERT_EQUAL_STRING("correct horse", p.pass.c_str());
}

void test_add_frame_open_network_has_empty_password() {
    const Parsed p = parse(frameOf("Cafe", ""));
    TEST_ASSERT_EQUAL_INT((int)Frame::Ok, (int)(p.frame));
    TEST_ASSERT_EQUAL_STRING("Cafe", p.name.c_str());
    TEST_ASSERT_EQUAL_STRING("", p.pass.c_str());
}

void test_add_frame_bad_crc() {
    const std::vector<uint8_t> f = frameOf("Home", "secret99");
    const Parsed p = parse(f, crcOf(f) ^ 1u);
    TEST_ASSERT_EQUAL_INT((int)Frame::Crc, (int)(p.frame));
    TEST_ASSERT_EQUAL_STRING("", p.name.c_str());
    TEST_ASSERT_EQUAL_STRING("", p.pass.c_str());
}

void test_add_frame_too_long() {
    // 32 + 1 + 64 fits; one more byte does not.
    const std::vector<uint8_t> fits = frameOf(std::string(32, 'n'), std::string(64, 'p'));
    TEST_ASSERT_EQUAL_size_t(kFrameMax, fits.size());
    TEST_ASSERT_TRUE(frameLengthOk((uint32_t)fits.size()));
    TEST_ASSERT_EQUAL_INT((int)Frame::Ok, (int)(parse(fits).frame));
    const std::vector<uint8_t> over = frameOf(std::string(32, 'n'), std::string(65, 'p'));
    TEST_ASSERT_FALSE(frameLengthOk((uint32_t)over.size()));
    TEST_ASSERT_EQUAL_INT((int)Frame::Invalid, (int)(parse(over).frame));
    TEST_ASSERT_FALSE(frameLengthOk(0));
    TEST_ASSERT_FALSE(frameLengthOk(100000));
    // Within the frame size, a name or password over its own limit.
    TEST_ASSERT_EQUAL_INT((int)Frame::Invalid, (int)(parse(frameOf(std::string(33, 'n'), "pw")).frame));
    TEST_ASSERT_EQUAL_INT((int)Frame::Invalid, (int)(parse(frameOf("n", std::string(65, 'p'))).frame));
}

void test_add_frame_missing_nul() {
    const std::string raw = "HomeNetsecret99";
    const std::vector<uint8_t> f(raw.begin(), raw.end());
    TEST_ASSERT_EQUAL_INT((int)Frame::Invalid, (int)(parse(f).frame));
}

void test_add_frame_second_nul_is_invalid() {
    std::vector<uint8_t> f = frameOf("Home", "sec");
    f.push_back(0);
    f.push_back('x');
    TEST_ASSERT_EQUAL_INT((int)Frame::Invalid, (int)(parse(f).frame));
    // A trailing NUL after the password is a second NUL too.
    std::vector<uint8_t> g = frameOf("Home", "secret99");
    g.push_back(0);
    TEST_ASSERT_EQUAL_INT((int)Frame::Invalid, (int)(parse(g).frame));
}

void test_add_frame_empty_ssid() {
    TEST_ASSERT_EQUAL_INT((int)Frame::Invalid, (int)(parse(frameOf("", "secret99")).frame));
    const std::vector<uint8_t> nulOnly(1, 0);
    TEST_ASSERT_EQUAL_INT((int)Frame::Invalid, (int)(parse(nulOnly).frame));
}

void test_add_frame_keeps_any_name_bytes() {
    // Spaces, '|', UTF-8: the name is bytes, not a line.
    const std::string name = "My Net | Caf\xc3\xa9";
    const Parsed p = parse(frameOf(name, "pw pw pw"));
    TEST_ASSERT_EQUAL_INT((int)Frame::Ok, (int)(p.frame));
    TEST_ASSERT_EQUAL_STRING(name.c_str(), p.name.c_str());
    TEST_ASSERT_EQUAL_STRING("pw pw pw", p.pass.c_str());
}

// ---- names as hex --------------------------------------------------------------------

void test_hex_lowercase_spaces_pipe_utf8() {
    TEST_ASSERT_EQUAL_STRING("486f6d65204e6574", hexOf("Home Net").c_str());
    TEST_ASSERT_EQUAL_STRING("617c62", hexOf("a|b").c_str());
    TEST_ASSERT_EQUAL_STRING("436166c3a9", hexOf("Caf\xc3\xa9").c_str());
    TEST_ASSERT_EQUAL_STRING("00ff", hexOf(std::string("\x00\xff", 2)).c_str());
    // 32 bytes -> 64 digits.
    TEST_ASSERT_EQUAL_size_t(64, hexOf(std::string(32, '\xab')).size());
}

void test_hex_never_overruns() {
    char out[5];
    const uint8_t data[] = {1, 2, 3, 4};
    TEST_ASSERT_EQUAL_size_t(4, toHex(data, sizeof(data), out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("0102", out);
}

// ---- wifi scan -----------------------------------------------------------------------

void consider(ScanList& l, const std::string& name, int rssi, int auth = 3) {
    scanConsider(l, reinterpret_cast<const uint8_t*>(name.data()), name.size(), rssi, auth);
}

std::string scanLine(const ScanEntry& e) {
    char out[kLineMax];
    TEST_ASSERT_TRUE(formatScanLine(out, sizeof(out), e) > 0);
    return out;
}

void test_scan_dedupes_and_sorts_strongest_first() {
    ScanList l;
    scanBegin(l);
    consider(l, "Office", -70, 3);
    consider(l, "Home", -60, 3);
    consider(l, "Office", -40, 7);   // the same name stronger, WPA2/WPA3
    consider(l, "Home", -80, 0);     // weaker: ignored, security unchanged
    consider(l, "Cafe", -50, 0);
    scanSort(l);
    TEST_ASSERT_EQUAL(3, l.count);
    TEST_ASSERT_EQUAL_STRING("[cmd] wifi.net=rssi=-40 sec=wpa3 ssid_hex=4f6666696365\n",
                             scanLine(l.entries[0]).c_str());
    TEST_ASSERT_EQUAL_STRING("[cmd] wifi.net=rssi=-50 sec=open ssid_hex=43616665\n",
                             scanLine(l.entries[1]).c_str());
    TEST_ASSERT_EQUAL_STRING("[cmd] wifi.net=rssi=-60 sec=wpa2 ssid_hex=486f6d65\n",
                             scanLine(l.entries[2]).c_str());
    char out[kLineMax];
    formatScanDone(out, sizeof(out), l.count);
    TEST_ASSERT_EQUAL_STRING("[cmd] wifi.scan.done=3\n", out);
}

void test_scan_caps_at_twenty_strongest() {
    ScanList l;
    scanBegin(l);
    // 40 names, weakest and strongest interleaved.
    for (int i = 0; i < 40; i++) {
        char name[8];
        snprintf(name, sizeof(name), "n%02d", i);
        consider(l, name, (i % 2) ? -90 + i : -30 - i);
    }
    scanSort(l);
    TEST_ASSERT_EQUAL(kScanMax, l.count);
    // Strongest first, and every kept one is at least as strong as any dropped.
    for (int i = 1; i < l.count; i++) TEST_ASSERT_TRUE(l.entries[i - 1].rssi >= l.entries[i].rssi);
    std::vector<int> all;
    for (int i = 0; i < 40; i++) all.push_back((i % 2) ? -90 + i : -30 - i);
    std::sort(all.begin(), all.end(), [](int a, int b) { return a > b; });
    TEST_ASSERT_EQUAL(all[kScanMax - 1], l.entries[kScanMax - 1].rssi);
    // A dropped name seen again stronger comes back in.
    consider(l, "late", -1);
    scanSort(l);
    TEST_ASSERT_EQUAL(kScanMax, l.count);
    TEST_ASSERT_EQUAL_STRING("late", l.entries[0].name);
}

void test_scan_skips_hidden_and_reads_padded_names() {
    ScanList l;
    scanBegin(l);
    uint8_t padded[33] = {0};
    memcpy(padded, "Pad", 3);
    scanConsider(l, padded, sizeof(padded), -55, 3);
    uint8_t hidden[33] = {0};
    scanConsider(l, hidden, sizeof(hidden), -20, 3);
    scanConsider(l, nullptr, 0, -20, 3);
    TEST_ASSERT_EQUAL(1, l.count);
    TEST_ASSERT_EQUAL(3, l.entries[0].nameLen);
    // A full 32-byte name (no NUL) is kept whole.
    uint8_t full[33];
    memset(full, 'z', 32);
    full[32] = 0;
    scanConsider(l, full, sizeof(full), -60, 3);
    TEST_ASSERT_EQUAL(2, l.count);
    TEST_ASSERT_EQUAL(32, l.entries[1].nameLen);
}

void test_scan_security_names() {
    TEST_ASSERT_EQUAL_STRING("open", securityName(securityFromAuthMode(0)));
    TEST_ASSERT_EQUAL_STRING("wpa", securityName(securityFromAuthMode(1)));
    TEST_ASSERT_EQUAL_STRING("wpa", securityName(securityFromAuthMode(2)));
    TEST_ASSERT_EQUAL_STRING("wpa2", securityName(securityFromAuthMode(3)));
    TEST_ASSERT_EQUAL_STRING("wpa2", securityName(securityFromAuthMode(4)));
    TEST_ASSERT_EQUAL_STRING("ent", securityName(securityFromAuthMode(5)));
    TEST_ASSERT_EQUAL_STRING("wpa3", securityName(securityFromAuthMode(6)));
    TEST_ASSERT_EQUAL_STRING("wpa3", securityName(securityFromAuthMode(7)));
    TEST_ASSERT_EQUAL_STRING("open", securityName(securityFromAuthMode(9)));
    TEST_ASSERT_EQUAL_STRING("ent", securityName(securityFromAuthMode(10)));
    TEST_ASSERT_EQUAL_STRING("wpa2", securityName(securityFromAuthMode(99)));
}

// ---- wifi saved / the three-network limit -----------------------------------------------

void test_add_saves_first_and_limits_to_three() {
    MemoryStore store;
    TEST_ASSERT_EQUAL_STRING("[cmd] wifi.saved=ssid_hex=41 position=1\n",
                             usbAdd(store, "A", "passA111").c_str());
    usbAdd(store, "B", "passB222");
    usbAdd(store, "C", "passC333");
    std::vector<std::string> names = savedNames(store);
    TEST_ASSERT_EQUAL(3, (int)names.size());
    TEST_ASSERT_EQUAL_STRING("C", names[0].c_str());   // the newest is tried first
    TEST_ASSERT_EQUAL_STRING("B", names[1].c_str());
    TEST_ASSERT_EQUAL_STRING("A", names[2].c_str());
    // A fourth is refused; nothing is dropped.
    TEST_ASSERT_EQUAL_STRING("[err] wifi.full=1\n", usbAdd(store, "D", "passD444").c_str());
    TEST_ASSERT_EQUAL(3, (int)savedNames(store).size());
    // A saved name takes the new password and moves first (still three).
    TEST_ASSERT_EQUAL_STRING("[cmd] wifi.saved=ssid_hex=41 position=1\n",
                             usbAdd(store, "A", "newpassA").c_str());
    names = savedNames(store);
    TEST_ASSERT_EQUAL(3, (int)names.size());
    TEST_ASSERT_EQUAL_STRING("A", names[0].c_str());
    // The older single-network keys mirror the first network (REQ-125).
    TEST_ASSERT_EQUAL_STRING("A", store.values[WifiList::kKeyLegacyName].c_str());
}

void test_saved_list_names_only() {
    MemoryStore store;
    usbAdd(store, "Home Net", "hunter2hunter2");
    usbAdd(store, "a|b", "pipe|pass|word");
    std::string all;
    char out[kLineMax];
    const std::vector<std::string> names = savedNames(store);
    formatSavedCount(out, sizeof(out), (int)names.size());
    all += out;
    for (const std::string& n : names) {
        formatSavedLine(out, sizeof(out), n.c_str());
        all += out;
    }
    formatSavedDone(out, sizeof(out), (int)names.size());
    all += out;
    TEST_ASSERT_EQUAL_STRING("[cmd] wifi.saved.n=2\n"
                             "[cmd] wifi.saved.net=ssid_hex=617c62\n"
                             "[cmd] wifi.saved.net=ssid_hex=486f6d65204e6574\n"
                             "[cmd] wifi.saved.done=2\n",
                             all.c_str());
    assertNoSecret(all, "hunter2hunter2");
    assertNoSecret(all, "pipe|pass|word");
}

// ---- A3: no reply line of `wifi add` carries the password ------------------------------

void test_add_replies_never_carry_the_password() {
    const char* const passwords[] = {"hunter2hunter2", "Home Net", "4142", "|",
                                     "\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9",
                                     "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"};
    for (const char* pw : passwords) {
        const std::string pass = pw;
        // Every outcome: saved, updated, full, store failure, bad crc,
        // invalid frame. The name never equals the password here (a reply
        // naming the network may of course carry the name's own bytes).
        const std::string name = "Net";
        MemoryStore store;
        std::string all;
        all += usbAdd(store, name, pass);                 // Added
        all += usbAdd(store, name, pass);                 // Updated
        usbAdd(store, "B", "bbbbbbbb");
        usbAdd(store, "C", "cccccccc");
        all += usbAdd(store, "D", pass);                  // Full
        const std::vector<uint8_t> f = frameOf(name, pass);
        all += addReply(parse(f, crcOf(f) ^ 0x80000000u).frame, WifiList::AddResult::Invalid, "");
        std::vector<uint8_t> broken = f;
        broken.push_back(0);
        all += addReply(parse(broken).frame, WifiList::AddResult::Invalid, "");
        all += addReply(Frame::Ok, WifiList::AddResult::Invalid, name.c_str());   // store failed
        all += addReply(Frame::Ok, WifiList::AddResult::Full, name.c_str());
        all += addReply(Frame::Invalid, WifiList::AddResult::Invalid, nullptr);
        assertNoSecret(all, pass);
        // And only the documented lines came out.
        size_t at = 0;
        while (at < all.size()) {
            const size_t end = all.find('\n', at);
            const std::string line = all.substr(at, end - at);
            const bool known = line.rfind("[cmd] wifi.saved=ssid_hex=4e6574 position=1", 0) == 0 ||
                               line == "[err] wifi.full=1" || line == "[err] wifi.crc" ||
                               line == "[err] wifi.invalid";
            TEST_ASSERT_TRUE_MESSAGE(known, line.c_str());
            at = end + 1;
        }
    }
}

void test_parse_failure_leaves_no_password_in_outputs() {
    const std::vector<uint8_t> f = frameOf(std::string(33, 'n'), "topsecret");
    char name[WifiList::kNameMax + 1];
    char pass[WifiList::kPassMax + 1];
    memset(pass, 'q', sizeof(pass));
    TEST_ASSERT_EQUAL_INT((int)Frame::Invalid, (int)(parseAddFrame(f.data(), f.size(), crcOf(f), name, pass)));
    for (size_t i = 0; i < sizeof(pass); i++) TEST_ASSERT_EQUAL_UINT8(0, (uint8_t)pass[i]);
    for (size_t i = 0; i < sizeof(name); i++) TEST_ASSERT_EQUAL_UINT8(0, (uint8_t)name[i]);
}

void test_wipe_zeroes() {
    char buf[16];
    memset(buf, 'k', sizeof(buf));
    wipe(buf, sizeof(buf));
    for (char c : buf) TEST_ASSERT_EQUAL_CHAR(0, c);
}

// The driver prints only these formatters' lines: no print in the add path
// names the password or the raw frame, and the join never logs a key.
void test_driver_add_path_prints_no_password() {
    const std::string cli = readSource("lib/SerialCli/SerialCli.cpp");
    const std::string add = functionBody(cli, "void SerialCli::cmdWifiAdd(");
    size_t at = 0;
    while (at < add.size()) {
        size_t end = add.find('\n', at);
        if (end == std::string::npos) end = add.size();
        const std::string line = add.substr(at, end - at);
        if (line.find("Serial.") != std::string::npos || line.find("writeLine(") != std::string::npos) {
            TEST_ASSERT_TRUE_MESSAGE(line.find("pass") == std::string::npos, line.c_str());
            TEST_ASSERT_TRUE_MESSAGE(line.find("frame,") == std::string::npos, line.c_str());
        }
        at = end + 1;
    }
    // Every wipe is there: frame, password (twice: read failed / after save), name.
    TEST_ASSERT_TRUE(add.find("UsbWifi::wipe(frame, sizeof(frame))") != std::string::npos);
    TEST_ASSERT_TRUE(add.find("UsbWifi::wipe(pass, sizeof(pass))") != std::string::npos);
    TEST_ASSERT_TRUE(add.find("UsbWifi::wipe(name, sizeof(name))") != std::string::npos);
    const std::string saved = readSource("lib/CloudSync/SavedWifi.cpp");
    for (const SourceLine& l : classify(saved)) {
        if (l.text.find("Serial.") == std::string::npos) continue;
        TEST_ASSERT_TRUE_MESSAGE(l.text.find(".pass") == std::string::npos, l.text.c_str());
    }
}

// ---- who owns the radio ----------------------------------------------------------------

void test_busy_reasons() {
    RadioOwners none;
    TEST_ASSERT_NULL(busyReason(none));
    struct Case { bool RadioOwners::*field; const char* reason; };
    const Case cases[] = {
        {&RadioOwners::usbJob, "wifi"},     {&RadioOwners::update, "update"},
        {&RadioOwners::ferry, "ferry"},     {&RadioOwners::portal, "portal"},
        {&RadioOwners::music, "music"},     {&RadioOwners::link, "link"},
        {&RadioOwners::checkin, "checkin"}, {&RadioOwners::bluetooth, "bluetooth"},
        {&RadioOwners::radioOn, "wifi"},
    };
    for (const Case& c : cases) {
        RadioOwners o;
        o.*(c.field) = true;
        TEST_ASSERT_EQUAL_STRING(c.reason, busyReason(o));
        char out[kLineMax];
        formatBusy(out, sizeof(out), busyReason(o));
        TEST_ASSERT_EQUAL_STRING((std::string("[err] wifi.busy reason=") + c.reason + "\n").c_str(), out);
    }
    // The portal or music app name the owner even with the radio on.
    RadioOwners portal;
    portal.portal = true;
    portal.radioOn = true;
    TEST_ASSERT_EQUAL_STRING("portal", busyReason(portal));
    RadioOwners music;
    music.music = true;
    music.bluetooth = true;
    TEST_ASSERT_EQUAL_STRING("music", busyReason(music));
    RadioOwners link;
    link.link = true;
    link.radioOn = true;
    TEST_ASSERT_EQUAL_STRING("link", busyReason(link));
}

// ---- wifi try ------------------------------------------------------------------------

void test_join_failure_from_disconnect_reasons() {
    using WifiList::JoinFailure;
    TEST_ASSERT_TRUE(WifiList::isAuthReason(15));    // 4-way handshake timeout: wrong WPA2 password
    TEST_ASSERT_TRUE(WifiList::isAuthReason(202));
    TEST_ASSERT_TRUE(WifiList::isAuthReason(204));
    TEST_ASSERT_FALSE(WifiList::isAuthReason(201));
    TEST_ASSERT_FALSE(WifiList::isAuthReason(200));  // beacon timeout: out of reach, not a password
    TEST_ASSERT_TRUE(WifiList::isAbsentReason(201));
    TEST_ASSERT_FALSE(WifiList::isAbsentReason(15));
    TEST_ASSERT_EQUAL_INT((int)JoinFailure::None, (int)(WifiList::joinFailure(true, false, false, false, true, true)));
    TEST_ASSERT_EQUAL_INT((int)JoinFailure::NoneSaved, (int)(WifiList::joinFailure(false, true, false, false, false, false)));
    TEST_ASSERT_EQUAL_INT((int)JoinFailure::Stopped, (int)(WifiList::joinFailure(false, false, true, false, true, false)));
    TEST_ASSERT_EQUAL_INT((int)JoinFailure::Auth, (int)(WifiList::joinFailure(false, false, false, true, true, true)));
    TEST_ASSERT_EQUAL_INT((int)JoinFailure::Absent, (int)(WifiList::joinFailure(false, false, false, true, false, false)));
    TEST_ASSERT_EQUAL_INT((int)JoinFailure::Absent, (int)(WifiList::joinFailure(false, false, false, false, false, true)));
    TEST_ASSERT_EQUAL_INT((int)JoinFailure::Timeout, (int)(WifiList::joinFailure(false, false, false, false, false, false)));
}

void test_try_replies() {
    using WifiList::JoinFailure;
    char out[kLineMax];
    formatTryStarted(out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("[cmd] wifi.try=started\n", out);
    formatTryOk(out, sizeof(out), "Home Net");
    TEST_ASSERT_EQUAL_STRING("[cmd] wifi.try=ok ssid_hex=486f6d65204e6574\n", out);
    formatTryFail(out, sizeof(out), JoinFailure::Auth);
    TEST_ASSERT_EQUAL_STRING("[cmd] wifi.try=fail reason=auth\n", out);
    formatTryFail(out, sizeof(out), JoinFailure::Absent);
    TEST_ASSERT_EQUAL_STRING("[cmd] wifi.try=fail reason=absent\n", out);
    formatTryFail(out, sizeof(out), JoinFailure::NoneSaved);
    TEST_ASSERT_EQUAL_STRING("[cmd] wifi.try=fail reason=absent\n", out);
    formatTryFail(out, sizeof(out), JoinFailure::Timeout);
    TEST_ASSERT_EQUAL_STRING("[cmd] wifi.try=fail reason=timeout\n", out);
    formatTryFail(out, sizeof(out), JoinFailure::Stopped);
    TEST_ASSERT_EQUAL_STRING("[cmd] wifi.try=fail reason=busy\n", out);
}

void test_lines_never_cut() {
    char tiny[8];
    TEST_ASSERT_EQUAL_size_t(0, formatTryOk(tiny, sizeof(tiny), "Home"));
    TEST_ASSERT_EQUAL_STRING("", tiny);
    // The longest scan line fits kLineMax.
    ScanEntry e;
    memset(e.name, 0xff, WifiList::kNameMax);
    e.name[WifiList::kNameMax] = 0;
    e.nameLen = WifiList::kNameMax;
    e.rssi = -128;
    e.sec = Security::Enterprise;
    char out[kLineMax];
    TEST_ASSERT_TRUE(formatScanLine(out, sizeof(out), e) > 0);
}

// ---- A4 + REQ-120: what a release build answers, from the driver's source --------------

void test_release_build_verb_allowlist() {
    const std::vector<SourceLine> lines = classify(readSource("lib/SerialCli/SerialCli.cpp"));
    // The setup verbs and their announcement are compiled into every build.
    const char* const release[] = {"\"wifi scan\"", "\"wifi try\"", "\"wifi saved\"", "\"wifi add\"",
                                   "syncinfo.setup=1"};
    for (const char* v : release)
        TEST_ASSERT_TRUE_MESSAGE(countWhere(lines, v, false) >= 1, v);
    // Test-only, never in a release build: the OK press, the cloud verbs,
    // the older wifi forms.
    const char* const testOnly[] = {
        "\"link ok\"", "\"link no\"", "\"link clear\"", "\"link keep\"", "\"link unlink\"",
        "\"link forget\"", "\"link start\"", "\"link status\"", "\"cloud check\"",
        "verbWithArg(line, \"cloud\"", "cmdWifi(arg)", "SavedWifi::cliCommand",
        "verbWithArg(line, \"wifi\", &arg)",
    };
    for (const char* v : testOnly) {
        TEST_ASSERT_TRUE_MESSAGE(countWhere(lines, v, true) >= 1, v);
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, countWhere(lines, v, false), v);
    }
}

void test_syncinfo_setup_before_fw_terminator() {
    const std::string cli = readSource("lib/SerialCli/SerialCli.cpp");
    const std::string body = functionBody(cli, "void SerialCli::cmdSyncinfo(");
    const size_t setup = body.find("[cmd] syncinfo.setup=1");
    const size_t fw = body.find("[cmd] syncinfo.fw=");
    TEST_ASSERT_TRUE(setup != std::string::npos);
    TEST_ASSERT_TRUE(fw != std::string::npos);
    TEST_ASSERT_TRUE(setup < fw);
    // The fw line stays the last syncinfo line.
    TEST_ASSERT_EQUAL_size_t(fw, body.rfind("[cmd] syncinfo."));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_add_frame_good);
    RUN_TEST(test_add_frame_open_network_has_empty_password);
    RUN_TEST(test_add_frame_bad_crc);
    RUN_TEST(test_add_frame_too_long);
    RUN_TEST(test_add_frame_missing_nul);
    RUN_TEST(test_add_frame_second_nul_is_invalid);
    RUN_TEST(test_add_frame_empty_ssid);
    RUN_TEST(test_add_frame_keeps_any_name_bytes);
    RUN_TEST(test_hex_lowercase_spaces_pipe_utf8);
    RUN_TEST(test_hex_never_overruns);
    RUN_TEST(test_scan_dedupes_and_sorts_strongest_first);
    RUN_TEST(test_scan_caps_at_twenty_strongest);
    RUN_TEST(test_scan_skips_hidden_and_reads_padded_names);
    RUN_TEST(test_scan_security_names);
    RUN_TEST(test_add_saves_first_and_limits_to_three);
    RUN_TEST(test_saved_list_names_only);
    RUN_TEST(test_add_replies_never_carry_the_password);
    RUN_TEST(test_parse_failure_leaves_no_password_in_outputs);
    RUN_TEST(test_wipe_zeroes);
    RUN_TEST(test_driver_add_path_prints_no_password);
    RUN_TEST(test_busy_reasons);
    RUN_TEST(test_join_failure_from_disconnect_reasons);
    RUN_TEST(test_try_replies);
    RUN_TEST(test_lines_never_cut);
    RUN_TEST(test_release_build_verb_allowlist);
    RUN_TEST(test_syncinfo_setup_before_fw_terminator);
    return UNITY_END();
}
