// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#include "UsbWifi.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "SyncProtocol.h"   // crc32: the same checksum as every framed payload

namespace UsbWifi {
namespace {

// snprintf that reports 0 when the line did not fit (never a cut line).
size_t line(char* out, size_t cap, const char* fmt, ...) __attribute__((format(printf, 3, 4)));
size_t line(char* out, size_t cap, const char* fmt, ...) {
    if (!out || cap == 0) return 0;
    va_list args;
    va_start(args, fmt);
    const int n = vsnprintf(out, cap, fmt, args);
    va_end(args);
    if (n < 0 || (size_t)n >= cap) {
        out[0] = '\0';
        return 0;
    }
    return (size_t)n;
}

// Hex of a NUL-terminated name (at most kNameMax bytes are ever stored).
void nameHex(const char* name, char hex[kHexMax]) {
    size_t n = name ? strlen(name) : 0;
    if (n > WifiList::kNameMax) n = WifiList::kNameMax;
    toHex(reinterpret_cast<const uint8_t*>(name ? name : ""), n, hex, kHexMax);
}

// Stronger first; equal strength by name bytes, then length.
bool before(const ScanEntry& a, const ScanEntry& b) {
    if (a.rssi != b.rssi) return a.rssi > b.rssi;
    const size_t n = a.nameLen < b.nameLen ? a.nameLen : b.nameLen;
    const int c = memcmp(a.name, b.name, n);
    if (c != 0) return c < 0;
    return a.nameLen < b.nameLen;
}

} // namespace

void wipe(void* p, size_t n) {
    volatile uint8_t* b = static_cast<volatile uint8_t*>(p);
    while (n--) *b++ = 0;
}

bool frameLengthOk(uint32_t len) { return len >= 1 && len <= kFrameMax; }

Frame parseAddFrame(const uint8_t* data, size_t len, uint32_t crc,
                    char name[WifiList::kNameMax + 1], char pass[WifiList::kPassMax + 1]) {
    wipe(name, WifiList::kNameMax + 1);
    wipe(pass, WifiList::kPassMax + 1);
    if (!data || !frameLengthOk((uint32_t)len)) return Frame::Invalid;
    if (SyncProtocol::crc32(data, len) != crc) return Frame::Crc;
    const uint8_t* nul = static_cast<const uint8_t*>(memchr(data, 0, len));
    if (!nul) return Frame::Invalid;
    const size_t nameLen = (size_t)(nul - data);
    const size_t passLen = len - nameLen - 1;
    if (nameLen == 0 || nameLen > WifiList::kNameMax || passLen > WifiList::kPassMax)
        return Frame::Invalid;
    // Exactly one NUL: none inside the password.
    if (passLen > 0 && memchr(nul + 1, 0, passLen)) return Frame::Invalid;
    memcpy(name, data, nameLen);
    memcpy(pass, nul + 1, passLen);
    return Frame::Ok;
}

size_t toHex(const uint8_t* data, size_t len, char* out, size_t cap) {
    static const char kDigits[] = "0123456789abcdef";
    if (!out || cap == 0) return 0;
    if (len > (cap - 1) / 2) len = (cap - 1) / 2;
    for (size_t i = 0; i < len; i++) {
        out[2 * i] = kDigits[data[i] >> 4];
        out[2 * i + 1] = kDigits[data[i] & 0x0f];
    }
    out[2 * len] = '\0';
    return 2 * len;
}

const char* securityName(Security s) {
    switch (s) {
        case Security::Open:       return "open";
        case Security::Wpa:        return "wpa";
        case Security::Wpa2:       return "wpa2";
        case Security::Wpa3:       return "wpa3";
        case Security::Enterprise: return "ent";
    }
    return "wpa2";
}

Security securityFromAuthMode(int authMode) {
    switch (authMode) {
        case 0:  return Security::Open;         // OPEN
        case 1:  return Security::Wpa;          // WEP (a key is asked for)
        case 2:  return Security::Wpa;          // WPA_PSK
        case 3:  return Security::Wpa2;         // WPA2_PSK
        case 4:  return Security::Wpa2;         // WPA_WPA2_PSK
        case 5:  return Security::Enterprise;   // ENTERPRISE / WPA2_ENTERPRISE
        case 6:  return Security::Wpa3;         // WPA3_PSK
        case 7:  return Security::Wpa3;         // WPA2_WPA3_PSK
        case 8:  return Security::Wpa2;         // WAPI_PSK
        case 9:  return Security::Open;         // OWE: encrypted, no password
        case 10: return Security::Enterprise;   // WPA3_ENT_192
        default: return Security::Wpa2;
    }
}

void scanBegin(ScanList& list) {
    wipe(&list, sizeof(list));
    list.count = 0;
}

void scanConsider(ScanList& list, const uint8_t* name, size_t nameLen, int rssi, int authMode) {
    if (!name || nameLen == 0) return;   // hidden network: nothing to pick
    if (nameLen > WifiList::kNameMax) nameLen = WifiList::kNameMax;
    // Names are bytes up to the first NUL (the radio pads with NULs).
    const uint8_t* nul = static_cast<const uint8_t*>(memchr(name, 0, nameLen));
    if (nul) nameLen = (size_t)(nul - name);
    if (nameLen == 0) return;
    if (rssi > 127) rssi = 127;
    if (rssi < -128) rssi = -128;
    for (int i = 0; i < list.count; i++) {
        ScanEntry& e = list.entries[i];
        if (e.nameLen == nameLen && memcmp(e.name, name, nameLen) == 0) {
            if (rssi > e.rssi) {
                e.rssi = (int8_t)rssi;
                e.sec = securityFromAuthMode(authMode);
            }
            return;
        }
    }
    int at = list.count;
    if (list.count == kScanMax) {
        // Full: it replaces the weakest, when stronger.
        int weakest = 0;
        for (int i = 1; i < list.count; i++)
            if (list.entries[i].rssi < list.entries[weakest].rssi) weakest = i;
        if (rssi <= list.entries[weakest].rssi) return;
        at = weakest;
    } else {
        list.count++;
    }
    ScanEntry& e = list.entries[at];
    memset(e.name, 0, sizeof(e.name));
    memcpy(e.name, name, nameLen);
    e.nameLen = (uint8_t)nameLen;
    e.rssi = (int8_t)rssi;
    e.sec = securityFromAuthMode(authMode);
}

void scanSort(ScanList& list) {
    // At most 20 entries: insertion sort.
    for (int i = 1; i < list.count; i++) {
        ScanEntry e = list.entries[i];
        int j = i - 1;
        while (j >= 0 && before(e, list.entries[j])) {
            list.entries[j + 1] = list.entries[j];
            j--;
        }
        list.entries[j + 1] = e;
    }
}

const char* busyReason(const RadioOwners& o) {
    if (o.usbJob) return "wifi";
    if (o.update) return "update";
    if (o.ferry) return "ferry";
    if (o.portal) return "portal";
    if (o.music) return "music";
    if (o.link) return "link";
    if (o.checkin) return "checkin";
    if (o.bluetooth) return "bluetooth";
    if (o.radioOn) return "wifi";
    return nullptr;
}

const char* tryFailureName(WifiList::JoinFailure failure) {
    switch (failure) {
        case WifiList::JoinFailure::Auth:      return "auth";
        case WifiList::JoinFailure::Absent:    return "absent";
        case WifiList::JoinFailure::NoneSaved: return "absent";   // nothing to look for
        case WifiList::JoinFailure::Stopped:   return "busy";     // something else took the radio
        case WifiList::JoinFailure::Timeout:   return "timeout";
        case WifiList::JoinFailure::None:      return "timeout";  // not a failure: never asked
    }
    return "timeout";
}

size_t formatBusy(char* out, size_t cap, const char* reason) {
    return line(out, cap, "[err] wifi.busy reason=%s\n", reason ? reason : "wifi");
}

size_t formatScanLine(char* out, size_t cap, const ScanEntry& e) {
    char hex[kHexMax];
    toHex(reinterpret_cast<const uint8_t*>(e.name), e.nameLen, hex, sizeof(hex));
    return line(out, cap, "[cmd] wifi.net=rssi=%d sec=%s ssid_hex=%s\n", (int)e.rssi,
                securityName(e.sec), hex);
}

size_t formatScanDone(char* out, size_t cap, int count) {
    return line(out, cap, "[cmd] wifi.scan.done=%d\n", count);
}

size_t formatAddReply(char* out, size_t cap, Frame frame, WifiList::AddResult saved,
                      const char* name) {
    if (frame == Frame::Crc) return line(out, cap, "[err] wifi.crc\n");
    if (frame != Frame::Ok) return line(out, cap, "[err] wifi.invalid\n");
    if (saved == WifiList::AddResult::Full) return line(out, cap, "[err] wifi.full=1\n");
    if (saved == WifiList::AddResult::Invalid) return line(out, cap, "[err] wifi.invalid\n");
    char hex[kHexMax];
    nameHex(name, hex);
    // Added or Updated: either way it is now the first network tried.
    return line(out, cap, "[cmd] wifi.saved=ssid_hex=%s position=1\n", hex);
}

size_t formatSavedCount(char* out, size_t cap, int count) {
    return line(out, cap, "[cmd] wifi.saved.n=%d\n", count);
}

size_t formatSavedLine(char* out, size_t cap, const char* name) {
    char hex[kHexMax];
    nameHex(name, hex);
    return line(out, cap, "[cmd] wifi.saved.net=ssid_hex=%s\n", hex);
}

size_t formatSavedDone(char* out, size_t cap, int count) {
    return line(out, cap, "[cmd] wifi.saved.done=%d\n", count);
}

size_t formatTryStarted(char* out, size_t cap) {
    return line(out, cap, "[cmd] wifi.try=started\n");
}

size_t formatTryOk(char* out, size_t cap, const char* name) {
    char hex[kHexMax];
    nameHex(name, hex);
    return line(out, cap, "[cmd] wifi.try=ok ssid_hex=%s\n", hex);
}

size_t formatTryFail(char* out, size_t cap, WifiList::JoinFailure failure) {
    return line(out, cap, "[cmd] wifi.try=fail reason=%s\n", tryFailureName(failure));
}

// ---- the serial input ------------------------------------------------------------------

namespace {
// A CRLF host's '\n' is ~11 us behind its '\r' at 921600 baud; wait this long
// for it before handing the line on (a lone-'\r' host falls through).
constexpr uint32_t kCrlfPairWaitMs = 4;

void takePairedLf(Port& port) {
    const uint32_t start = port.nowMs();
    for (;;) {
        const int b = port.peek();
        if (b >= 0) {
            if (b == '\n') port.read();
            return;
        }
        if (port.nowMs() - start > kCrlfPairWaitMs) return;
    }
}
} // namespace

void LineInput::quarantine(uint32_t nowMs, uint32_t owedBytes) {
    quarantine_ = true;
    lastByteMs_ = nowMs;
    startMs_ = nowMs;
    owed_ = owedBytes;
}

LineInput::Next LineInput::next(Port& port) {
    if (quarantine_) {
        size_t dropped = 0;
        for (;;) {
            const uint32_t now = port.nowMs();
            if (dropped >= kQuarantineBytesPerCall) return Next::None;   // still flowing: next pass
            if (port.read() >= 0) {
                lastByteMs_ = now;
                if (owed_) owed_--;
                dropped++;
                continue;
            }
            if (now - lastByteMs_ < kQuietGapMs) return Next::None;
            if (owed_ && now - startMs_ < kOwedWindowMs) return Next::None;
            break;
        }
        quarantine_ = false;
        wipe(buf_, sizeof(buf_));
        len_ = 0;
        overflow_ = false;
    }
    for (;;) {
        const int b = port.read();
        if (b < 0) return Next::None;
        const char c = static_cast<char>(b);
        if (c == '\n' || c == '\r') {
            if (overflow_) {
                overflow_ = false;
                len_ = 0;
                return Next::TooLong;
            }
            if (len_ == 0) continue;   // empty line, or the LF of a CRLF
            buf_[len_] = '\0';
            len_ = 0;
            // Taken before dispatch, so a framed payload never starts with it.
            if (c == '\r') takePairedLf(port);
            return Next::Line;
        }
        if (len_ + 1 >= kCap) {
            overflow_ = true;   // dropped up to the end of the line
            continue;
        }
        buf_[len_++] = c;
    }
}

// ---- wifi add over the port -------------------------------------------------------------

namespace {
// Exactly n bytes, the gap and the whole-read limit checked on every pass
// (a continuous stream cannot hold it past kAddTotalMs). `got` says how
// many arrived.
bool readFrame(Port& port, uint8_t* buf, size_t n, size_t& got) {
    const uint32_t start = port.nowMs();
    uint32_t last = start;
    got = 0;
    while (got < n) {
        const uint32_t now = port.nowMs();
        if (now - start > kAddTotalMs || now - last > kAddGapMs) return false;
        const int b = port.read();
        if (b < 0) continue;
        buf[got++] = static_cast<uint8_t>(b);
        last = now;
    }
    return true;
}

// Nothing more arrives for kQuietGapMs after the payload.
bool staysQuiet(Port& port) {
    const uint32_t start = port.nowMs();
    while (port.nowMs() - start < kQuietGapMs) {
        if (port.read() >= 0) return false;
    }
    return true;
}
} // namespace

void handleAdd(const char* args, Port& port, LineInput& input, SaveFn save, const char* refuse) {
    char out[kLineMax];
    uint32_t len = 0, crc = 0;
    const bool header = args && SyncProtocol::parseApplyHeader(args, len, crc);
    if (refuse) {
        // Its payload is still coming: dropped, as owed bytes.
        input.quarantine(port.nowMs(), header ? len : 0);
        const size_t n = formatBusy(out, sizeof(out), refuse);
        if (n) port.write(out, n);
        return;
    }
    if (!header || !frameLengthOk(len)) {
        // No usable length: whatever follows is dropped until the input is
        // quiet (and an over-long payload's announced bytes have passed).
        input.quarantine(port.nowMs(), header ? len : 0);
        const size_t n = formatAddReply(out, sizeof(out), Frame::Invalid, WifiList::AddResult::Invalid, nullptr);
        if (n) port.write(out, n);
        return;
    }
    uint8_t frame[kFrameMax];
    char name[WifiList::kNameMax + 1];
    char pass[WifiList::kPassMax + 1];
    wipe(name, sizeof(name));
    wipe(pass, sizeof(pass));
    size_t got = 0;
    const bool read = readFrame(port, frame, len, got);
    const bool whole = read && staysQuiet(port);
    Frame parsed = Frame::Invalid;
    if (whole) parsed = parseAddFrame(frame, len, crc, name, pass);
    // Stalled (the rest may still come) or more than announced: drop the rest.
    else input.quarantine(port.nowMs(), read ? 0 : (uint32_t)(len - got));
    wipe(frame, sizeof(frame));
    WifiList::AddResult saved = WifiList::AddResult::Invalid;
    if (parsed == Frame::Ok && save) saved = save(name, pass);
    wipe(pass, sizeof(pass));
    const size_t n = formatAddReply(out, sizeof(out), parsed, saved, name);
    wipe(name, sizeof(name));
    if (n) port.write(out, n);
}

} // namespace UsbWifi
