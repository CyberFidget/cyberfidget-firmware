// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#include "WifiList.h"

#include <stdio.h>
#include <string.h>

namespace WifiList {
namespace {

void copyText(char* out, size_t cap, const char* in) {
    // cap is the array size; the text is already length-checked.
    strncpy(out, in ? in : "", cap - 1);
    out[cap - 1] = '\0';
}

void wipeNetwork(Network& n) {
    memset(n.name, 0, sizeof(n.name));
    memset(n.pass, 0, sizeof(n.pass));
}

// Removes nets[at], closing the gap.
void removeAt(List& list, int at) {
    for (int i = at; i + 1 < list.count; i++) list.nets[i] = list.nets[i + 1];
    list.count--;
    wipeNetwork(list.nets[list.count]);
}

// Puts `n` at the front, moving the others down one place.
void insertFront(List& list, const Network& n) {
    for (int i = list.count; i > 0; i--) list.nets[i] = list.nets[i - 1];
    list.nets[0] = n;
    list.count++;
}

void moveFront(List& list, int at) {
    if (at <= 0) return;
    Network n = list.nets[at];
    removeAt(list, at);
    insertFront(list, n);
    wipeNetwork(n);
}

bool validText(const char* name, const char* pass) {
    return name && name[0] && strlen(name) <= kNameMax && (!pass || strlen(pass) <= kPassMax);
}

// ---- stored form: two whole copies of the list ------------------------------------

// Copy keys: "<b>n", "<b>s0".., "<b>p0".., "<b>h", "<b>c" with b = 'a' or 'b'.
void bankKey(char bank, char what, char out[4]) {
    out[0] = bank;
    out[1] = what;
    out[2] = '\0';
}

void slotKey(char bank, char kind, int i, char out[4]) {
    out[0] = bank;
    out[1] = kind;
    out[2] = (char)('0' + i);
    out[3] = '\0';
}

char bankLetter(uint32_t id) { return id == 1 ? 'a' : id == 2 ? 'b' : 0; }

void hintText(const Hint& hint, char out[16]) {
    if (!hint.valid) { out[0] = '\0'; return; }
    char hex[13];
    addressToHex(hint.address, hex);
    snprintf(out, 16, "%u:%s", (unsigned)hint.channel, hex);
}

bool hintFromText(const char* text, Hint& out) {
    out = Hint();
    const char* colon = strchr(text, ':');
    if (!colon || colon == text || colon - text > 2) return false;
    unsigned channel = 0;
    for (const char* p = text; p < colon; p++) {
        if (*p < '0' || *p > '9') return false;
        channel = channel * 10 + (unsigned)(*p - '0');
    }
    if (channel < 1 || channel > 14 || !addressFromHex(colon + 1, out.address)) return false;
    out.valid = true;
    out.channel = (uint8_t)channel;
    return true;
}

void mix(uint32_t& h, const char* text) {
    for (const char* p = text; *p; p++) { h ^= (uint8_t)*p; h *= 16777619u; }
    h ^= 0xffu;   // field end
    h *= 16777619u;
}

// FNV-1a over everything a copy holds: a copy is used only when every one
// of its keys was written by the same save.
uint32_t checksum(char bank, const List& list) {
    uint32_t h = 2166136261u;
    const char head[3] = {bank, (char)('0' + list.count), 0};
    mix(h, head);
    for (int i = 0; i < list.count; i++) {
        mix(h, list.nets[i].name);
        mix(h, list.nets[i].pass);
    }
    char hint[16];
    hintText(list.hint, hint);
    mix(h, hint);
    return h;
}

// Reads one copy; false unless it is whole (checksum) and well formed.
bool readBank(Store& store, char bank, List& out) {
    wipe(out);
    char key[4];
    bankKey(bank, 'n', key);
    if (!store.has(key)) return false;
    const uint32_t count = store.getUInt(key);
    if (count > (uint32_t)kMax) return false;
    for (int i = 0; i < (int)count; i++) {
        Network& n = out.nets[i];
        slotKey(bank, 's', i, key);
        if (!store.getString(key, n.name, sizeof(n.name)) || !n.name[0]) { wipe(out); return false; }
        slotKey(bank, 'p', i, key);
        if (!store.getString(key, n.pass, sizeof(n.pass))) n.pass[0] = '\0';
        for (int j = 0; j < i; j++)
            if (strcmp(out.nets[j].name, n.name) == 0) { wipe(out); return false; }
        out.count = i + 1;
    }
    char hint[16] = {0};
    bankKey(bank, 'h', key);
    if (store.getString(key, hint, sizeof(hint)) && hint[0] &&
        (count == 0 || !hintFromText(hint, out.hint))) {
        wipe(out);
        return false;
    }
    bankKey(bank, 'c', key);
    if (!store.has(key) || store.getUInt(key) != checksum(bank, out)) { wipe(out); return false; }
    return true;
}

bool writeBank(Store& store, char bank, const List& list) {
    bool ok = true;
    char key[4];
    // The checksum goes first too (removed), so a half-written copy never
    // passes with an old checksum that happens to match.
    bankKey(bank, 'c', key);
    ok = store.remove(key) && ok;
    for (int i = 0; i < kMax; i++) {
        slotKey(bank, 's', i, key);
        if (i < list.count) ok = store.putString(key, list.nets[i].name) && ok;
        else ok = store.remove(key) && ok;
        slotKey(bank, 'p', i, key);
        if (i < list.count) ok = store.putString(key, list.nets[i].pass) && ok;
        else ok = store.remove(key) && ok;
    }
    char hint[16];
    hintText(list.count > 0 ? list.hint : Hint(), hint);
    bankKey(bank, 'h', key);
    if (hint[0]) ok = store.putString(key, hint) && ok;
    else ok = store.remove(key) && ok;
    bankKey(bank, 'n', key);
    ok = store.putUInt(key, (uint32_t)list.count) && ok;
    bankKey(bank, 'c', key);
    ok = store.putUInt(key, checksum(bank, list)) && ok;   // last: the copy is whole
    return ok;
}

} // namespace

int find(const List& list, const char* name) {
    if (!name) return -1;
    for (int i = 0; i < list.count; i++)
        if (strcmp(list.nets[i].name, name) == 0) return i;
    return -1;
}

void wipe(List& list) {
    for (Network& n : list.nets) wipeNetwork(n);
    list.count = 0;
    list.hint = Hint();
}

void load(Store& store, List& out, bool& repair) {
    wipe(out);
    repair = false;
    Network legacy;
    const bool hasLegacy = store.getString(kKeyLegacyName, legacy.name, sizeof(legacy.name)) &&
                           legacy.name[0];
    if (hasLegacy && !store.getString(kKeyLegacyPass, legacy.pass, sizeof(legacy.pass)))
        legacy.pass[0] = '\0';
    // A save was cut short: the older keys may be half written, so the list
    // (whichever copy is current) wins and they are rebuilt from it.
    const bool interrupted = store.has(kKeySync);

    const char current = bankLetter(store.getUInt(kKeyBank));
    bool haveList = current && readBank(store, current, out);
    bool fellBack = false;
    if (current && !haveList) {
        // Not expected (the flip is the save's last list write), but never
        // use a mixed copy: fall back to the other one if it is whole. The
        // older keys then copy the damaged one, so the whole copy wins.
        haveList = readBank(store, current == 'a' ? 'b' : 'a', out);
        fellBack = haveList;
        repair = true;
    }

    if (!haveList) {
        // First read by this image (or nothing whole to read): the single
        // saved network becomes the list.
        if (hasLegacy) {
            out.nets[0] = legacy;
            out.count = 1;
            repair = true;
        }
        if (interrupted || current) repair = true;
        wipeNetwork(legacy);
        return;
    }

    if (interrupted || fellBack) {
        repair = true;
    } else if (hasLegacy) {
        if (out.count == 0 || strcmp(legacy.name, out.nets[0].name) != 0) {
            // An older image saved this network: it is the newest choice.
            // With three saved, the last one makes room for it.
            if (find(out, legacy.name) < 0 && out.count == kMax) {
                wipeNetwork(out.nets[kMax - 1]);
                out.count--;
            }
            add(out, legacy.name, legacy.pass);
            repair = true;
        } else if (strcmp(legacy.pass, out.nets[0].pass) != 0) {
            copyText(out.nets[0].pass, sizeof(out.nets[0].pass), legacy.pass);
            repair = true;
        }
    } else if (out.count > 0) {
        repair = true;   // the single-network copy is missing
    }
    wipeNetwork(legacy);
}

bool save(Store& store, const List& list) {
    // Set first, cleared last: while it is set the older keys are not trusted.
    bool ok = store.putUInt(kKeySync, 1);
    const char current = bankLetter(store.getUInt(kKeyBank));
    // Write over the copy that is not the good one: normally the one that
    // is not current, but when the current copy is damaged (the read fell
    // back to the other), that damaged copy - never the only whole one.
    List scratch;
    const bool currentWhole = current && readBank(store, current, scratch);
    wipe(scratch);
    const char target = currentWhole ? (current == 'a' ? 'b' : 'a') : (current ? current : 'a');
    ok = ok && writeBank(store, target, list);
    // One write switches to the new copy (no change when it was current).
    ok = ok && store.putUInt(kKeyBank, target == 'a' ? 1u : 2u);
    if (!ok) return false;   // the marker stays: the next read repairs
    if (list.count > 0) {
        ok = store.putString(kKeyLegacyName, list.nets[0].name) && ok;
        ok = store.putString(kKeyLegacyPass, list.nets[0].pass) && ok;
    } else {
        ok = store.remove(kKeyLegacyName) && ok;
        ok = store.remove(kKeyLegacyPass) && ok;
    }
    return ok && store.remove(kKeySync);
}

AddResult add(List& list, const char* name, const char* pass) {
    if (!validText(name, pass)) return AddResult::Invalid;
    if (!pass) pass = "";
    const int at = find(list, name);
    if (at < 0 && list.count >= kMax) return AddResult::Full;
    if (at > 0) list.hint = Hint();          // a different network goes first
    if (at < 0 && list.count > 0) list.hint = Hint();
    if (at >= 0) {
        copyText(list.nets[at].pass, sizeof(list.nets[at].pass), pass);
        moveFront(list, at);
        return AddResult::Updated;
    }
    Network n;
    copyText(n.name, sizeof(n.name), name);
    copyText(n.pass, sizeof(n.pass), pass);
    insertFront(list, n);
    wipeNetwork(n);
    return AddResult::Added;
}

bool forget(List& list, const char* name) {
    const int at = find(list, name);
    if (at < 0) return false;
    if (at == 0) list.hint = Hint();
    removeAt(list, at);
    return true;
}

bool useFirst(List& list, const char* name) {
    const int at = find(list, name);
    if (at <= 0) return false;
    list.hint = Hint();
    moveFront(list, at);
    return true;
}

bool markJoined(List& list, int index, uint8_t channel, const uint8_t address[6]) {
    if (index < 0 || index >= list.count) return false;
    bool changed = false;
    if (index > 0) {
        moveFront(list, index);
        list.hint = Hint();
        changed = true;
    }
    if (!address || channel < 1 || channel > 14) {
        if (list.hint.valid) { list.hint = Hint(); changed = true; }
        return changed;
    }
    if (!list.hint.valid || list.hint.channel != channel ||
        memcmp(list.hint.address, address, 6) != 0) {
        list.hint.valid = true;
        list.hint.channel = channel;
        memcpy(list.hint.address, address, 6);
        changed = true;
    }
    return changed;
}

bool firstEndsOnAbsent(int count, bool hinted, bool scheduled) {
    return scheduled || hinted || count >= 2;
}

bool fallbackScan(int count, bool hinted) {
    return count >= 2 || (count == 1 && hinted);
}

bool consider(const List& list, const char* name, int rssi, Pick& best) {
    const int at = find(list, name);
    if (at < 0 || (best.index >= 0 && rssi <= best.rssi)) return false;
    best.index = at;
    best.rssi = rssi;
    return true;
}

int pickFromScan(const List& list, const Seen* seen, int seenCount, int& seenIndex) {
    Pick best;
    seenIndex = -1;
    for (int i = 0; seen && i < seenCount; i++)
        if (consider(list, seen[i].name, seen[i].rssi, best)) seenIndex = i;
    return best.index;
}

bool isAuthReason(uint8_t reason) {
    switch (reason) {
        case 14:    // MIC_FAILURE
        case 15:    // 4WAY_HANDSHAKE_TIMEOUT (the usual answer to a wrong WPA2 password)
        case 23:    // 802_1X_AUTH_FAILED
        case 202:   // AUTH_FAIL
        case 204:   // HANDSHAKE_TIMEOUT
            return true;
        default:
            return false;
    }
}

bool isAbsentReason(uint8_t reason) {
    switch (reason) {
        case 201:   // NO_AP_FOUND
        case 210:   // NO_AP_FOUND_W_COMPATIBLE_SECURITY
        case 211:   // NO_AP_FOUND_IN_AUTHMODE_THRESHOLD
        case 212:   // NO_AP_FOUND_IN_RSSI_THRESHOLD
            return true;
        default:
            return false;
    }
}

JoinFailure joinFailure(bool ok, bool noneSaved, bool stopped, bool absent,
                        bool sawAuth, bool sawAbsent) {
    if (ok) return JoinFailure::None;
    if (noneSaved) return JoinFailure::NoneSaved;
    if (stopped) return JoinFailure::Stopped;
    if (sawAuth) return JoinFailure::Auth;
    if (absent || sawAbsent) return JoinFailure::Absent;
    return JoinFailure::Timeout;
}

void addressToHex(const uint8_t address[6], char out[13]) {
    static const char digits[] = "0123456789abcdef";
    for (int i = 0; i < 6; i++) {
        out[i * 2] = digits[address[i] >> 4];
        out[i * 2 + 1] = digits[address[i] & 15];
    }
    out[12] = '\0';
}

bool addressFromHex(const char* hex, uint8_t out[6]) {
    if (!hex || strlen(hex) != 12) return false;
    for (int i = 0; i < 12; i++) {
        const char c = hex[i];
        int v;
        if (c >= '0' && c <= '9') v = c - '0';
        else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
        else return false;
        if (i % 2 == 0) out[i / 2] = (uint8_t)(v << 4);
        else out[i / 2] |= (uint8_t)v;
    }
    return true;
}

} // namespace WifiList
