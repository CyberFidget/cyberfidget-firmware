// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#ifndef USB_WIFI_H
#define USB_WIFI_H

// The USB serial WiFi setup verbs (every build): `wifi scan`, `wifi add`,
// `wifi try`, `wifi saved`. Pure: reading the framed `wifi add` payload,
// network names as hex, the scan list (deduped, strongest first, capped),
// who owns the radio, and every reply line. No radio, no storage; the
// driver is lib/SerialCli. Host-tested in test/test_sync_usbwifi.
//
// A password only ever passes through parseAddFrame() into the caller's
// buffer; no function here takes one to format. The caller wipes its frame,
// name and password buffers (wipe()) whatever the outcome.

#include <stddef.h>
#include <stdint.h>

#include "WifiList.h"

namespace UsbWifi {

/// The largest `wifi add` payload: a 32-byte name, its NUL, a 64-byte password.
constexpr size_t kFrameMax = WifiList::kNameMax + 1 + WifiList::kPassMax;
/// Scan lines sent at most.
constexpr int kScanMax = 20;
/// Hex of a network name, with its NUL.
constexpr size_t kHexMax = WifiList::kNameMax * 2 + 1;
/// Room for the longest reply line (a scan line), with its newline and NUL.
constexpr size_t kLineMax = 128;

// ---- wifi add ----------------------------------------------------------------------

enum class Frame : uint8_t { Ok, Invalid, Crc };
/// Reads `ssid\0pass` (exactly one NUL; name 1-32 bytes; password 0-64
/// bytes, empty for an open network). The CRC-32 of the whole payload is
/// checked first. Outputs are always NUL-terminated (empty unless Ok).
Frame parseAddFrame(const uint8_t* data, size_t len, uint32_t crc,
                    char name[WifiList::kNameMax + 1], char pass[WifiList::kPassMax + 1]);

/// A header the device can read the payload of: a length of 1..kFrameMax.
/// (A longer one is still drained, so the stream stays in frame.)
bool frameLengthOk(uint32_t len);

/// Zeroes `n` bytes in a way the compiler keeps.
void wipe(void* p, size_t n);

// ---- names as hex ------------------------------------------------------------------

/// Lowercase hex of `len` bytes; `out` holds 2*len+1. Returns the length.
size_t toHex(const uint8_t* data, size_t len, char* out, size_t cap);

// ---- wifi scan ---------------------------------------------------------------------

enum class Security : uint8_t { Open, Wpa, Wpa2, Wpa3, Enterprise };
const char* securityName(Security s);
/// From the radio's auth mode (wifi_auth_mode_t numbering, pinned by the
/// driver): OPEN/OWE need no password; WEP and WPA are "wpa"; unknown modes
/// read as "wpa2" (a password is asked for).
Security securityFromAuthMode(int authMode);

struct ScanEntry {
    char name[WifiList::kNameMax + 1];
    uint8_t nameLen;
    int8_t rssi;
    Security sec;
};

/// The strongest kScanMax different names seen; one access point record at
/// a time, so a busy place with dozens of records needs no more room.
struct ScanList {
    ScanEntry entries[kScanMax];
    int count = 0;
};
void scanBegin(ScanList& list);
/// One record. A hidden network (empty name) is skipped. A name seen again
/// keeps its strongest record (and that record's security).
void scanConsider(ScanList& list, const uint8_t* name, size_t nameLen, int rssi, int authMode);
/// Strongest first (equal strength: by name bytes, so the order is stable).
void scanSort(ScanList& list);

// ---- who owns the radio ------------------------------------------------------------

struct RadioOwners {
    bool usbJob = false;      ///< a `wifi scan` / `wifi try` (or a bench probe) is running
    bool update = false;      ///< a new image is still being checked
    bool ferry = false;       ///< a USB file transfer is open
    bool portal = false;      ///< the setup portal app is open
    bool music = false;       ///< the music player is open
    bool link = false;        ///< a link (or unlink) is under way
    bool checkin = false;     ///< a check-in (or dev mode listening) is running
    bool bluetooth = false;   ///< Bluetooth has started this power cycle (WiFi waits for a restart)
    bool radioOn = false;     ///< WiFi is on for any other reason
};
/// nullptr when the radio is free; otherwise the reason word for
/// `[err] wifi.busy reason=<word>`: wifi|update|ferry|portal|music|link|
/// checkin|bluetooth.
const char* busyReason(const RadioOwners& owners);

// ---- wifi try ----------------------------------------------------------------------

/// What `wifi try` reports for a failed join (`wifi.try=fail reason=<word>`).
const char* tryFailureName(WifiList::JoinFailure failure);

// ---- reply lines (each ends in "\n"; returns the length, 0 if it did not fit) -------

size_t formatBusy(char* out, size_t cap, const char* reason);
size_t formatScanLine(char* out, size_t cap, const ScanEntry& e);
size_t formatScanDone(char* out, size_t cap, int count);
/// The `wifi add` reply for a frame outcome and (when the frame was Ok) the
/// store's answer. `name` is printed as hex only after a save.
size_t formatAddReply(char* out, size_t cap, Frame frame, WifiList::AddResult saved,
                      const char* name);
size_t formatSavedCount(char* out, size_t cap, int count);
size_t formatSavedLine(char* out, size_t cap, const char* name);
size_t formatSavedDone(char* out, size_t cap, int count);
size_t formatTryStarted(char* out, size_t cap);
size_t formatTryOk(char* out, size_t cap, const char* name);
size_t formatTryFail(char* out, size_t cap, WifiList::JoinFailure failure);

} // namespace UsbWifi

#endif // USB_WIFI_H
