// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

// Saved WiFi networks: the stored form (and the older single-network keys),
// the cap of three, Forget / Use this first, the remembered place of the
// last network that worked, and the order a session tries them in.

#include <unity.h>
#include <map>
#include <string>
#include <stdio.h>
#include <string.h>

#include "WifiList.h"
#include "WifiRequest.h"

using namespace WifiList;

namespace {

class MemoryStore : public Store {
public:
    std::map<std::string, std::string> values;
    int writes = 0;
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
    bool putString(const char* key, const char* value) override {
        writes++;
        values[key] = value;
        return true;
    }
    bool putUInt(const char* key, uint32_t value) override {
        writes++;
        values[key] = std::to_string(value);
        return true;
    }
    bool remove(const char* key) override { values.erase(key); return true; }
};

const uint8_t kHere[6] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
const uint8_t kThere[6] = {0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};

List listOf(const char* a, const char* b = nullptr, const char* c = nullptr) {
    List l;
    if (c) add(l, c, "pc");
    if (b) add(l, b, "pb");
    if (a) add(l, a, "pa");
    return l;
}

} // namespace

void setUp(void) {}
void tearDown(void) {}

// ---- stored form -------------------------------------------------------------------

void test_nothing_saved_reads_empty(void) {
    MemoryStore st;
    List l;
    bool repair = true;
    load(st, l, repair);
    TEST_ASSERT_EQUAL_INT(0, l.count);
    TEST_ASSERT_FALSE(repair);
}

void test_single_network_migrates_into_first_place(void) {
    MemoryStore st;
    st.values["ssid"] = "Home";
    st.values["pass"] = "secret";
    List l;
    bool repair = false;
    load(st, l, repair);
    TEST_ASSERT_EQUAL_INT(1, l.count);
    TEST_ASSERT_EQUAL_STRING("Home", l.nets[0].name);
    TEST_ASSERT_EQUAL_STRING("secret", l.nets[0].pass);
    TEST_ASSERT_TRUE(repair);
    TEST_ASSERT_TRUE(save(st, l));
    TEST_ASSERT_EQUAL_STRING("1", st.values["bank"].c_str());
    TEST_ASSERT_EQUAL_STRING("1", st.values["an"].c_str());
    TEST_ASSERT_EQUAL_STRING("Home", st.values["as0"].c_str());
    TEST_ASSERT_EQUAL_STRING("secret", st.values["ap0"].c_str());
    TEST_ASSERT_EQUAL_INT(0, (int)st.values.count("lsync"));
    // The older keys stay, so the previous image still finds the network.
    TEST_ASSERT_EQUAL_STRING("Home", st.values["ssid"].c_str());
    TEST_ASSERT_EQUAL_STRING("secret", st.values["pass"].c_str());
    List again;
    load(st, again, repair);
    TEST_ASSERT_FALSE(repair);
    TEST_ASSERT_EQUAL_INT(1, again.count);
}

void test_older_keys_always_copy_the_first_network(void) {
    MemoryStore st;
    List l = listOf("Home", "Phone");
    save(st, l);
    TEST_ASSERT_EQUAL_STRING("Home", st.values["ssid"].c_str());
    useFirst(l, "Phone");
    save(st, l);
    TEST_ASSERT_EQUAL_STRING("Phone", st.values["ssid"].c_str());
    TEST_ASSERT_EQUAL_STRING("pb", st.values["pass"].c_str());
    forget(l, "Phone");
    forget(l, "Home");
    save(st, l);
    TEST_ASSERT_EQUAL_INT(0, (int)st.values.count("ssid"));
    TEST_ASSERT_EQUAL_INT(0, (int)st.values.count("pass"));
    List read;
    bool repair = true;
    load(st, read, repair);
    TEST_ASSERT_EQUAL_INT(0, read.count);
    TEST_ASSERT_FALSE(repair);
}

void test_network_saved_by_an_older_image_goes_first(void) {
    MemoryStore st;
    List l = listOf("Home", "Phone", "Office");
    save(st, l);
    // The previous image's portal replaced the single network.
    st.values["ssid"] = "Cafe";
    st.values["pass"] = "latte";
    List read;
    bool repair = false;
    load(st, read, repair);
    TEST_ASSERT_TRUE(repair);
    TEST_ASSERT_EQUAL_INT(3, read.count);
    TEST_ASSERT_EQUAL_STRING("Cafe", read.nets[0].name);
    TEST_ASSERT_EQUAL_STRING("latte", read.nets[0].pass);
    TEST_ASSERT_EQUAL_STRING("Home", read.nets[1].name);
    TEST_ASSERT_EQUAL_STRING("Phone", read.nets[2].name);   // the last one made room
}

void test_password_changed_by_an_older_image_is_taken(void) {
    MemoryStore st;
    List l = listOf("Home");
    save(st, l);
    st.values["pass"] = "newpass";
    List read;
    bool repair = false;
    load(st, read, repair);
    TEST_ASSERT_TRUE(repair);
    TEST_ASSERT_EQUAL_STRING("newpass", read.nets[0].pass);
}

void test_older_image_forget_clears_everything(void) {
    MemoryStore st;
    List l = listOf("Home", "Phone");
    save(st, l);
    st.values.clear();   // the previous image's Forget clears the namespace
    List read;
    bool repair = false;
    load(st, read, repair);
    TEST_ASSERT_EQUAL_INT(0, read.count);
}

void test_missing_copy_is_repaired(void) {
    MemoryStore st;
    List l = listOf("Home");
    save(st, l);
    st.values.erase("ssid");
    List read;
    bool repair = false;
    load(st, read, repair);
    TEST_ASSERT_TRUE(repair);
    TEST_ASSERT_EQUAL_STRING("Home", read.nets[0].name);
}

void test_keys_fit_storage_limit(void) {
    const char* keys[] = {kKeyBank, kKeySync, kKeyLegacyName, kKeyLegacyPass, kNamespace,
                          "an", "as2", "ap2", "ah", "ac", "bn", "bs2", "bp2", "bh", "bc"};
    for (const char* k : keys) TEST_ASSERT_TRUE(strlen(k) <= 15);
}

// ---- cap, forget, first -------------------------------------------------------------

void test_cap_of_three_refuses_a_fourth(void) {
    List l = listOf("A", "B", "C");
    TEST_ASSERT_EQUAL_INT(3, l.count);
    TEST_ASSERT_EQUAL_INT((int)AddResult::Full, (int)add(l, "D", "x"));
    TEST_ASSERT_EQUAL_INT(3, l.count);
    TEST_ASSERT_EQUAL_INT(-1, find(l, "D"));
    // A saved one can still be updated when full.
    TEST_ASSERT_EQUAL_INT((int)AddResult::Updated, (int)add(l, "C", "new"));
    TEST_ASSERT_EQUAL_STRING("C", l.nets[0].name);
    TEST_ASSERT_EQUAL_STRING("new", l.nets[0].pass);
}

void test_add_puts_the_new_network_first(void) {
    List l = listOf("Home");
    TEST_ASSERT_EQUAL_INT((int)AddResult::Added, (int)add(l, "Phone", "p"));
    TEST_ASSERT_EQUAL_STRING("Phone", l.nets[0].name);
    TEST_ASSERT_EQUAL_STRING("Home", l.nets[1].name);
}

void test_add_refuses_bad_text(void) {
    List l;
    TEST_ASSERT_EQUAL_INT((int)AddResult::Invalid, (int)add(l, "", "x"));
    TEST_ASSERT_EQUAL_INT((int)AddResult::Invalid, (int)add(l, nullptr, "x"));
    std::string longName(33, 'n');
    TEST_ASSERT_EQUAL_INT((int)AddResult::Invalid, (int)add(l, longName.c_str(), "x"));
    std::string longPass(65, 'p');
    TEST_ASSERT_EQUAL_INT((int)AddResult::Invalid, (int)add(l, "ok", longPass.c_str()));
    std::string maxName(32, 'n');
    std::string maxPass(64, 'p');
    TEST_ASSERT_EQUAL_INT((int)AddResult::Added, (int)add(l, maxName.c_str(), maxPass.c_str()));
    TEST_ASSERT_EQUAL_INT((int)AddResult::Added, (int)add(l, "Open", ""));
}

void test_forget_removes_and_keeps_order(void) {
    List l = listOf("A", "B", "C");
    TEST_ASSERT_TRUE(forget(l, "B"));
    TEST_ASSERT_EQUAL_INT(2, l.count);
    TEST_ASSERT_EQUAL_STRING("A", l.nets[0].name);
    TEST_ASSERT_EQUAL_STRING("C", l.nets[1].name);
    TEST_ASSERT_FALSE(forget(l, "B"));
    TEST_ASSERT_EQUAL_STRING("", l.nets[2].pass);   // wiped
}

void test_use_first_moves_to_front(void) {
    List l = listOf("A", "B", "C");
    TEST_ASSERT_TRUE(useFirst(l, "C"));
    TEST_ASSERT_EQUAL_STRING("C", l.nets[0].name);
    TEST_ASSERT_EQUAL_STRING("A", l.nets[1].name);
    TEST_ASSERT_EQUAL_STRING("B", l.nets[2].name);
    TEST_ASSERT_EQUAL_STRING("pc", l.nets[0].pass);
    TEST_ASSERT_FALSE(useFirst(l, "C"));      // already first
    TEST_ASSERT_FALSE(useFirst(l, "Nope"));
}

// ---- last network that worked ----------------------------------------------------------

void test_join_is_remembered_and_stored(void) {
    MemoryStore st;
    List l = listOf("Home", "Phone");
    TEST_ASSERT_TRUE(markJoined(l, 0, 6, kHere));
    TEST_ASSERT_TRUE(l.hint.valid);
    save(st, l);
    TEST_ASSERT_EQUAL_STRING("6:001122334455", st.values["ah"].c_str());
    List read;
    bool repair = true;
    load(st, read, repair);
    TEST_ASSERT_FALSE(repair);
    TEST_ASSERT_TRUE(read.hint.valid);
    TEST_ASSERT_EQUAL_UINT8(6, read.hint.channel);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(kHere, read.hint.address, 6);
}

void test_same_place_again_writes_nothing(void) {
    List l = listOf("Home");
    markJoined(l, 0, 6, kHere);
    TEST_ASSERT_FALSE(markJoined(l, 0, 6, kHere));
    TEST_ASSERT_TRUE(markJoined(l, 0, 11, kHere));     // moved channel
    TEST_ASSERT_TRUE(markJoined(l, 0, 11, kThere));    // another access point
}

void test_fallback_join_moves_that_network_first(void) {
    List l = listOf("Home", "Phone");
    markJoined(l, 0, 6, kHere);
    TEST_ASSERT_TRUE(markJoined(l, 1, 1, kThere));
    TEST_ASSERT_EQUAL_STRING("Phone", l.nets[0].name);
    TEST_ASSERT_EQUAL_STRING("pb", l.nets[0].pass);
    TEST_ASSERT_TRUE(l.hint.valid);
    TEST_ASSERT_EQUAL_UINT8(1, l.hint.channel);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(kThere, l.hint.address, 6);
}

void test_changing_the_first_network_drops_the_place(void) {
    List l = listOf("Home", "Phone");
    markJoined(l, 0, 6, kHere);
    useFirst(l, "Phone");
    TEST_ASSERT_FALSE(l.hint.valid);

    l = listOf("Home", "Phone");
    markJoined(l, 0, 6, kHere);
    forget(l, "Home");
    TEST_ASSERT_FALSE(l.hint.valid);

    l = listOf("Home", "Phone");
    markJoined(l, 0, 6, kHere);
    forget(l, "Phone");                 // not the first: the place stays
    TEST_ASSERT_TRUE(l.hint.valid);
    add(l, "Cafe", "x");                // new network goes first
    TEST_ASSERT_FALSE(l.hint.valid);

    l = listOf("Home");
    markJoined(l, 0, 6, kHere);
    add(l, "Home", "changed");          // same network, new password
    TEST_ASSERT_TRUE(l.hint.valid);
}

// A copy with any key out of step with its checksum is never used: the
// other (earlier) copy is, or failing that the older keys.
void test_damaged_copy_is_not_used(void) {
    MemoryStore st;
    List l = listOf("Home", "Phone");
    save(st, l);                        // copy a: no place
    markJoined(l, 0, 6, kHere);
    save(st, l);                        // copy b (current): with place
    TEST_ASSERT_EQUAL_STRING("2", st.values["bank"].c_str());
    const char* damage[] = {"6:zz1122334455", "0:001122334455", "7:001122334455"};
    for (const char* d : damage) {
        MemoryStore copy = st;
        copy.values["bh"] = d;
        List read;
        bool repair = false;
        load(copy, read, repair);
        TEST_ASSERT_TRUE(repair);
        TEST_ASSERT_EQUAL_INT(2, read.count);
        TEST_ASSERT_FALSE(read.hint.valid);   // copy a, the earlier state
    }
    MemoryStore copy = st;
    copy.values["bp1"] = "changed";           // a password out of step
    copy.values["as0"] = "";                  // and copy a unusable too
    List read;
    bool repair = false;
    load(copy, read, repair);
    TEST_ASSERT_TRUE(repair);
    TEST_ASSERT_EQUAL_INT(1, read.count);     // the older keys: the first network
    TEST_ASSERT_EQUAL_STRING("Home", read.nets[0].name);
}

void test_older_image_change_drops_the_place(void) {
    MemoryStore st;
    List l = listOf("Home", "Phone");
    markJoined(l, 0, 6, kHere);
    save(st, l);
    st.values["ssid"] = "Phone";
    st.values["pass"] = "pb";
    List read;
    bool repair = false;
    load(st, read, repair);
    TEST_ASSERT_EQUAL_STRING("Phone", read.nets[0].name);
    TEST_ASSERT_FALSE(read.hint.valid);
}

// ---- join order ----------------------------------------------------------------------

void test_first_attempt_end_on_absent(void) {
    // One network, no remembered place, manual check: keeps trying (as before).
    TEST_ASSERT_FALSE(firstEndsOnAbsent(1, false, false));
    TEST_ASSERT_TRUE(firstEndsOnAbsent(1, false, true));
    TEST_ASSERT_TRUE(firstEndsOnAbsent(1, true, false));
    TEST_ASSERT_TRUE(firstEndsOnAbsent(2, false, false));
}

void test_fallback_scan_only_when_it_can_help(void) {
    TEST_ASSERT_FALSE(fallbackScan(0, false));
    // The connect's own scan already looked for the only network everywhere.
    TEST_ASSERT_FALSE(fallbackScan(1, false));
    // A quick look in the remembered place only: it may have moved.
    TEST_ASSERT_TRUE(fallbackScan(1, true));
    TEST_ASSERT_TRUE(fallbackScan(2, false));
    TEST_ASSERT_TRUE(fallbackScan(3, true));
}

void test_scan_picks_the_strongest_saved_network(void) {
    List l = listOf("Home", "Phone", "Office");
    const Seen seen[] = {{"Neighbour", -30}, {"Office", -70}, {"Phone", -55}, {"Home", -80}};
    int at = -1;
    TEST_ASSERT_EQUAL_INT(1, pickFromScan(l, seen, 4, at));
    TEST_ASSERT_EQUAL_INT(2, at);
}

void test_scan_with_no_saved_network_picks_none(void) {
    List l = listOf("Home");
    const Seen seen[] = {{"Neighbour", -30}, {"", -40}};
    int at = 7;
    TEST_ASSERT_EQUAL_INT(-1, pickFromScan(l, seen, 2, at));
    TEST_ASSERT_EQUAL_INT(-1, at);
    TEST_ASSERT_EQUAL_INT(-1, pickFromScan(l, nullptr, 0, at));
}

void test_scan_duplicate_names_take_the_strongest(void) {
    List l = listOf("Home");
    const Seen seen[] = {{"Home", -75}, {"Home", -40}};
    int at = -1;
    TEST_ASSERT_EQUAL_INT(0, pickFromScan(l, seen, 2, at));
    TEST_ASSERT_EQUAL_INT(1, at);
}

void test_address_hex_round_trip(void) {
    char hex[13];
    addressToHex(kThere, hex);
    TEST_ASSERT_EQUAL_STRING("aabbccddeeff", hex);
    uint8_t back[6] = {0};
    TEST_ASSERT_TRUE(addressFromHex("AABBCCDDEEFF", back));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(kThere, back, 6);
    TEST_ASSERT_FALSE(addressFromHex("aabb", back));
    TEST_ASSERT_FALSE(addressFromHex(nullptr, back));
}

void test_wipe_clears_passwords(void) {
    List l = listOf("A", "B");
    wipe(l);
    TEST_ASSERT_EQUAL_INT(0, l.count);
    for (const Network& n : l.nets) {
        TEST_ASSERT_EQUAL_STRING("", n.name);
        TEST_ASSERT_EQUAL_STRING("", n.pass);
    }
}

// ---- a power cut after any single storage write ----------------------------------

namespace {

// Passes reads through; after `budget` writes, drops every later write (the
// power went; nothing after that runs).
class CutStore : public Store {
public:
    CutStore(MemoryStore& inner, int budget) : in_(inner), budget_(budget) {}
    int used = 0;
    bool has(const char* key) override { return in_.has(key); }
    bool getString(const char* key, char* out, size_t len) override { return in_.getString(key, out, len); }
    uint32_t getUInt(const char* key) override { return in_.getUInt(key); }
    bool putString(const char* key, const char* value) override {
        return write() ? in_.putString(key, value) : true;
    }
    bool putUInt(const char* key, uint32_t value) override {
        return write() ? in_.putUInt(key, value) : true;
    }
    bool remove(const char* key) override { return write() ? in_.remove(key) : true; }
private:
    bool write() {
        used++;
        if (budget_ < 0) return true;
        if (budget_ == 0) return false;
        budget_--;
        return true;
    }
    MemoryStore& in_;
    int budget_;
};

bool sameState(const List& a, const List& b) {
    if (a.count != b.count || a.hint.valid != b.hint.valid) return false;
    if (a.hint.valid && (a.hint.channel != b.hint.channel ||
                         memcmp(a.hint.address, b.hint.address, 6) != 0)) return false;
    for (int i = 0; i < a.count; i++)
        if (strcmp(a.nets[i].name, b.nets[i].name) != 0 ||
            strcmp(a.nets[i].pass, b.nets[i].pass) != 0) return false;
    return true;
}

// `base` holds `before` (and an unrelated older list in the other copy, so
// reading the wrong copy would show). Saving `after` is cut after every
// write in turn; each time the next read must give `before` or `after`,
// whole, and after its repair the older keys must copy its first network.
void cutEverywhere(const MemoryStore& base, const List& before, const List& after) {
    MemoryStore probe = base;
    CutStore counter(probe, -1);
    TEST_ASSERT_TRUE(save(counter, after));
    const int writes = counter.used;
    TEST_ASSERT_TRUE(writes > 3);
    for (int k = 0; k <= writes; k++) {
        MemoryStore st = base;
        CutStore cut(st, k);
        save(cut, after);
        List read;
        bool repair = false;
        load(st, read, repair);
        char msg[48];
        snprintf(msg, sizeof(msg), "cut after write %d of %d", k, writes);
        const bool isBefore = sameState(read, before);
        const bool isAfter = sameState(read, after);
        TEST_ASSERT_TRUE_MESSAGE(isBefore || isAfter, msg);
        if (k == 0) TEST_ASSERT_TRUE_MESSAGE(isBefore, msg);
        if (k == writes) TEST_ASSERT_TRUE_MESSAGE(isAfter, msg);
        if (repair) TEST_ASSERT_TRUE(save(st, read));
        List again;
        bool againRepair = true;
        load(st, again, againRepair);
        TEST_ASSERT_FALSE_MESSAGE(againRepair, msg);
        TEST_ASSERT_TRUE_MESSAGE(sameState(again, read), msg);
        if (read.count > 0) {
            TEST_ASSERT_EQUAL_STRING_MESSAGE(read.nets[0].name, st.values["ssid"].c_str(), msg);
            TEST_ASSERT_EQUAL_STRING_MESSAGE(read.nets[0].pass, st.values["pass"].c_str(), msg);
        } else {
            TEST_ASSERT_EQUAL_INT_MESSAGE(0, (int)st.values.count("ssid"), msg);
        }
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, (int)st.values.count("lsync"), msg);
    }
}

MemoryStore storeWith(const List& before) {
    MemoryStore st;
    List older = listOf("Older", "Other");
    save(st, older);
    save(st, before);
    return st;
}

List placed(List l) {
    markJoined(l, 0, 6, kHere);
    return l;
}

} // namespace

void test_cut_during_add(void) {
    const List before = placed(listOf("Home", "Phone"));
    List after = before;
    TEST_ASSERT_EQUAL_INT((int)AddResult::Added, (int)add(after, "Cafe", "latte"));
    cutEverywhere(storeWith(before), before, after);
}

void test_cut_during_password_change(void) {
    const List before = placed(listOf("Home", "Phone"));
    List after = before;
    add(after, "Home", "newsecret");
    cutEverywhere(storeWith(before), before, after);
}

void test_cut_during_use_first(void) {
    const List before = placed(listOf("Home", "Phone", "Office"));
    List after = before;
    TEST_ASSERT_TRUE(useFirst(after, "Office"));
    cutEverywhere(storeWith(before), before, after);
}

void test_cut_during_forget(void) {
    const List before = placed(listOf("Home", "Phone", "Office"));
    List first = before;
    forget(first, "Home");
    cutEverywhere(storeWith(before), before, first);
    List last = before;
    forget(last, "Office");
    cutEverywhere(storeWith(before), before, last);
    const List only = placed(listOf("Home"));
    List none = only;
    forget(none, "Home");
    cutEverywhere(storeWith(only), only, none);
}

void test_cut_during_remember(void) {
    const List before = placed(listOf("Home", "Phone"));
    List after = before;
    markJoined(after, 1, 11, kThere);
    cutEverywhere(storeWith(before), before, after);
}

void test_cut_during_migration(void) {
    MemoryStore base;
    base.values["ssid"] = "Home";
    base.values["pass"] = "secret";
    List migrated;
    bool repair = false;
    load(base, migrated, repair);
    TEST_ASSERT_TRUE(repair);
    cutEverywhere(base, migrated, migrated);
}

// A save that starts with one copy damaged (either copy, current or not)
// writes over the damaged one: the whole copy survives every cut.
namespace {
// Three saves from empty leave copy a current (Z, G, X -> a=X, b=G); two
// leave copy b current (G, X -> a=G, b=X). `damageCurrent` breaks X, the
// current copy, so the good one is G; otherwise the other copy is broken
// and the good one is X.
void cutFromDamaged(bool currentIsA, bool damageCurrent) {
    const List g = placed(listOf("Home", "Phone"));
    const List x = listOf("Cafe", "Home");
    MemoryStore base;
    if (currentIsA) save(base, listOf("Zed"));
    save(base, g);
    save(base, x);
    TEST_ASSERT_EQUAL_STRING(currentIsA ? "1" : "2", base.values["bank"].c_str());
    const char current = currentIsA ? 'a' : 'b';
    const char other = currentIsA ? 'b' : 'a';
    const std::string broken = std::string(1, damageCurrent ? current : other) + "c";
    base.values[broken] = "1";                          // checksum no longer matches
    const List good = damageCurrent ? g : x;
    List read;
    bool repair = false;
    load(base, read, repair);
    TEST_ASSERT_TRUE(sameState(read, good));
    // The repair save itself, and a change saved on top of the good copy.
    cutEverywhere(base, good, good);
    List changed = good;
    add(changed, "Office", "desk");
    cutEverywhere(base, good, changed);
}
} // namespace

void test_cut_from_damaged_current_copy_a(void) { cutFromDamaged(true, true); }
void test_cut_from_damaged_current_copy_b(void) { cutFromDamaged(false, true); }
void test_cut_from_damaged_other_copy_a(void) { cutFromDamaged(false, false); }
void test_cut_from_damaged_other_copy_b(void) { cutFromDamaged(true, false); }

// The older keys count as an older image's write only when no save was cut
// short; otherwise the list wins and they are rebuilt from it.
void test_half_written_older_keys_never_override_the_list(void) {
    MemoryStore st;
    List l = listOf("Home", "Phone");
    save(st, l);
    st.values["lsync"] = "1";
    st.values["pass"] = "half";          // pass written, the name not yet
    st.values["ssid"] = "Phone";
    List read;
    bool repair = false;
    load(st, read, repair);
    TEST_ASSERT_TRUE(repair);
    TEST_ASSERT_TRUE(sameState(read, l));
    save(st, read);
    TEST_ASSERT_EQUAL_STRING("Home", st.values["ssid"].c_str());
    TEST_ASSERT_EQUAL_STRING("pa", st.values["pass"].c_str());
}

// ---- the portal's requests -----------------------------------------------------------

namespace {
bool parseText(const char* text, WifiRequest::Kind kind, char* name, char* pass) {
    return WifiRequest::parse(text, strlen(text), kind, name, pass);
}
} // namespace

void test_request_connect_forms(void) {
    char name[33], pass[65];
    using WifiRequest::Kind;
    TEST_ASSERT_TRUE(parseText("{\"ssid\":\"Guest\",\"pass\":\"\"}", Kind::Connect, name, pass));
    TEST_ASSERT_EQUAL_STRING("Guest", name);
    TEST_ASSERT_EQUAL_STRING("", pass);
    TEST_ASSERT_TRUE(parseText(" {\"pass\":\"p w\",\"ssid\":\"O'Brien \\\"x\\\"\"} ", Kind::Connect, name, pass));
    TEST_ASSERT_EQUAL_STRING("O'Brien \"x\"", name);
    TEST_ASSERT_EQUAL_STRING("p w", pass);
    TEST_ASSERT_TRUE(parseText("{\"ssid\":\"caf\\u00e9\",\"pass\":\"x\"}", Kind::Connect, name, pass));
    TEST_ASSERT_EQUAL_STRING("caf\xc3\xa9", name);
}

void test_request_missing_or_null_password_is_refused(void) {
    char name[33], pass[65];
    using WifiRequest::Kind;
    const char* bad[] = {
        "{\"ssid\":\"Guest\"}",
        "{\"ssid\":\"Guest\",\"pass\":null}",
        "{\"ssid\":\"Guest\",\"pass\":5}",
    };
    for (const char* b : bad) {
        memset(name, 'X', sizeof(name));
        memset(pass, 'X', sizeof(pass));
        TEST_ASSERT_FALSE_MESSAGE(parseText(b, Kind::Connect, name, pass), b);
        // Never left unset for a caller that reads it anyway.
        TEST_ASSERT_EQUAL_STRING("", name);
        TEST_ASSERT_EQUAL_STRING("", pass);
    }
}

void test_request_strict_json(void) {
    char name[33], pass[65];
    using WifiRequest::Kind;
    const char* bad[] = {
        "{\"ssid\":\"a\",\"pass\":\"\"}x",                   // trailing data
        "{\"ssid\":\"a\",\"pass\":\"\"}{}",
        "{\"ssid\":\"a\",\"ssid\":\"b\",\"pass\":\"\"}",      // twice
        "{\"ssid\":\"a\",\"pass\":\"\",\"x\":1}",             // a field it does not take
        "{\"ssid\":\"a\\u0000b\",\"pass\":\"\"}",             // escaped NUL
        "{\"ssid\":\"a\",\"pass\":\"\\u0000\"}",
        "[\"a\"]",
        "{\"ssid\":\"\",\"pass\":\"\"}",                     // empty name
        "{\"ssid\":1,\"pass\":\"\"}",
        "{\"ssid\":\"a\",\"pass\":\"\"",                      // cut short
        "",
    };
    for (const char* b : bad) TEST_ASSERT_FALSE_MESSAGE(parseText(b, Kind::Connect, name, pass), b);
    // A raw NUL inside the measured body.
    const char raw[] = "{\"ssid\":\"a\0b\",\"pass\":\"\"}";
    TEST_ASSERT_FALSE(WifiRequest::parse(raw, sizeof(raw) - 1, Kind::Connect, name, pass));
    // Not terminated where the length says.
    const char longer[] = "{\"ssid\":\"a\",\"pass\":\"\"}zz";
    TEST_ASSERT_FALSE(WifiRequest::parse(longer, sizeof(longer) - 3, Kind::Connect, name, pass));
    // A backslash then "u0000" as text is not a NUL.
    TEST_ASSERT_TRUE(parseText("{\"ssid\":\"a\\\\u0000\",\"pass\":\"\"}", Kind::Connect, name, pass));
    TEST_ASSERT_EQUAL_STRING("a\\u0000", name);
}

void test_request_lengths(void) {
    char name[33], pass[65];
    using WifiRequest::Kind;
    char text[300];
    snprintf(text, sizeof(text), "{\"ssid\":\"%s\",\"pass\":\"%s\"}",
             std::string(32, 'n').c_str(), std::string(64, 'p').c_str());
    TEST_ASSERT_TRUE(parseText(text, Kind::Connect, name, pass));
    snprintf(text, sizeof(text), "{\"ssid\":\"%s\",\"pass\":\"\"}", std::string(33, 'n').c_str());
    TEST_ASSERT_FALSE(parseText(text, Kind::Connect, name, pass));
    snprintf(text, sizeof(text), "{\"ssid\":\"a\",\"pass\":\"%s\"}", std::string(65, 'p').c_str());
    TEST_ASSERT_FALSE(parseText(text, Kind::Connect, name, pass));
    const std::string huge = "{\"ssid\":\"a\",\"pass\":\"\"}" + std::string(300, ' ');
    TEST_ASSERT_FALSE(parseText(huge.c_str(), Kind::Connect, name, pass));
}

void test_request_name_only_routes(void) {
    char name[33];
    using WifiRequest::Kind;
    TEST_ASSERT_TRUE(parseText("{\"ssid\":\"Home\"}", Kind::NameOnly, name, nullptr));
    TEST_ASSERT_EQUAL_STRING("Home", name);
    TEST_ASSERT_FALSE(parseText("{\"ssid\":\"Home\",\"pass\":\"\"}", Kind::NameOnly, name, nullptr));
    TEST_ASSERT_FALSE(parseText("{}", Kind::NameOnly, name, nullptr));
}

void test_request_body_pieces(void) {
    using WifiRequest::Collect;
    const char* text = "{\"ssid\":\"Home\"}";
    const size_t n = strlen(text);
    const uint8_t* d = reinterpret_cast<const uint8_t*>(text);
    WifiRequest::Body b;
    WifiRequest::begin(b);
    TEST_ASSERT_EQUAL_INT((int)Collect::More, (int)WifiRequest::collect(b, d, 5, 0, n));
    TEST_ASSERT_EQUAL_INT((int)Collect::Done, (int)WifiRequest::collect(b, d + 5, n - 5, 5, n));
    TEST_ASSERT_EQUAL_STRING(text, b.text);

    struct Case { size_t len, index, total; };
    // Out of order, a changed total, running past the end.
    const Case second[] = {{3, 7, n}, {n - 5, 5, n + 1}, {n, 5, n}};
    for (const Case& c : second) {
        WifiRequest::begin(b);
        WifiRequest::collect(b, d, 5, 0, n);
        TEST_ASSERT_EQUAL_INT((int)Collect::Invalid, (int)WifiRequest::collect(b, d, c.len, c.index, c.total));
        TEST_ASSERT_EQUAL_STRING("", b.text);    // wiped
        // Failed for good.
        TEST_ASSERT_EQUAL_INT((int)Collect::Invalid, (int)WifiRequest::collect(b, d, n, 0, n));
    }
    WifiRequest::begin(b);   // no start seen
    TEST_ASSERT_EQUAL_INT((int)Collect::Invalid, (int)WifiRequest::collect(b, d, 3, 4, n));
    WifiRequest::begin(b);   // too big
    TEST_ASSERT_EQUAL_INT((int)Collect::Invalid,
                          (int)WifiRequest::collect(b, d, n, 0, WifiRequest::kBodyMax + 1));
}

void test_scan_looks_at_every_record(void) {
    List l = listOf("Home");
    Pick best;
    // Far more records than any fixed table would hold; the saved one last.
    for (int i = 0; i < 200; i++) {
        char other[16];
        snprintf(other, sizeof(other), "Busy%d", i);
        TEST_ASSERT_FALSE(consider(l, other, -20, best));
    }
    TEST_ASSERT_TRUE(consider(l, "Home", -80, best));
    TEST_ASSERT_EQUAL_INT(0, best.index);
    TEST_ASSERT_FALSE(consider(l, "Home", -90, best));   // weaker duplicate
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_nothing_saved_reads_empty);
    RUN_TEST(test_single_network_migrates_into_first_place);
    RUN_TEST(test_older_keys_always_copy_the_first_network);
    RUN_TEST(test_network_saved_by_an_older_image_goes_first);
    RUN_TEST(test_password_changed_by_an_older_image_is_taken);
    RUN_TEST(test_older_image_forget_clears_everything);
    RUN_TEST(test_missing_copy_is_repaired);
    RUN_TEST(test_keys_fit_storage_limit);
    RUN_TEST(test_cap_of_three_refuses_a_fourth);
    RUN_TEST(test_add_puts_the_new_network_first);
    RUN_TEST(test_add_refuses_bad_text);
    RUN_TEST(test_forget_removes_and_keeps_order);
    RUN_TEST(test_use_first_moves_to_front);
    RUN_TEST(test_join_is_remembered_and_stored);
    RUN_TEST(test_same_place_again_writes_nothing);
    RUN_TEST(test_fallback_join_moves_that_network_first);
    RUN_TEST(test_changing_the_first_network_drops_the_place);
    RUN_TEST(test_damaged_copy_is_not_used);
    RUN_TEST(test_cut_during_add);
    RUN_TEST(test_cut_during_password_change);
    RUN_TEST(test_cut_during_use_first);
    RUN_TEST(test_cut_during_forget);
    RUN_TEST(test_cut_during_remember);
    RUN_TEST(test_cut_during_migration);
    RUN_TEST(test_half_written_older_keys_never_override_the_list);
    RUN_TEST(test_cut_from_damaged_current_copy_a);
    RUN_TEST(test_cut_from_damaged_current_copy_b);
    RUN_TEST(test_cut_from_damaged_other_copy_a);
    RUN_TEST(test_cut_from_damaged_other_copy_b);
    RUN_TEST(test_request_connect_forms);
    RUN_TEST(test_request_missing_or_null_password_is_refused);
    RUN_TEST(test_request_strict_json);
    RUN_TEST(test_request_lengths);
    RUN_TEST(test_request_name_only_routes);
    RUN_TEST(test_request_body_pieces);
    RUN_TEST(test_scan_looks_at_every_record);
    RUN_TEST(test_older_image_change_drops_the_place);
    RUN_TEST(test_first_attempt_end_on_absent);
    RUN_TEST(test_fallback_scan_only_when_it_can_help);
    RUN_TEST(test_scan_picks_the_strongest_saved_network);
    RUN_TEST(test_scan_with_no_saved_network_picks_none);
    RUN_TEST(test_scan_duplicate_names_take_the_strongest);
    RUN_TEST(test_address_hex_round_trip);
    RUN_TEST(test_wipe_clears_passwords);
    return UNITY_END();
}
