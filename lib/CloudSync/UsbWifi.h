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

// ---- the serial input: lines, and a quarantine after a bad `wifi add` ---------------

/// The serial byte stream (the UART on the device, a script in tests).
class Port {
public:
    virtual ~Port() = default;
    virtual int read() = 0;   ///< next byte, -1 when none is waiting
    virtual int peek() = 0;   ///< the same without taking it
    virtual uint32_t nowMs() = 0;
    virtual void write(const char* text, size_t len) = 0;
};

/// Silence after a `wifi add` payload, and before the line that ends a
/// quarantine.
constexpr uint32_t kQuietGapMs = 50;
/// Bytes a quarantine reads per call at most, so the loop keeps running
/// under a continuous stream.
constexpr size_t kQuarantineBytesPerCall = 1024;
/// The only line that ends a quarantine (any case; it is then answered as
/// usual). Every client already sends it to see whether the Fidget answers.
constexpr char kResyncLine[] = "version";

/// Line assembly for the serial command loop (SerialCli::poll): one line at
/// a time, LF or CRLF (a CR's paired LF is taken before the line is handed
/// on, so a payload read never starts with it), empty lines ignored, an
/// over-long line reported once and dropped.
///
/// Quarantine (after an ambiguous `wifi add`: its payload may still be
/// arriving, now or much later): every line is dropped - never dispatched,
/// never echoed - until a line that is exactly kResyncLine, begun after at
/// least kQuietGapMs of silence (blank lines just before it do not count as
/// a start), so a payload tail that happens to hold that word does not end
/// it. That line is handed on as a normal Line and reading resumes. There is
/// no time limit: only the client ends a quarantine.
class LineInput {
public:
    static constexpr size_t kCap = 160;   // SerialCli::kBufferSize
    enum class Next : uint8_t { None, Line, TooLong };
    /// Reads until a whole line (Line: see line()), the end of an over-long
    /// line (TooLong), or nothing more is waiting / a quarantine is still on
    /// (None).
    Next next(Port& port);
    const char* line() const { return buf_; }
    void quarantine(uint32_t nowMs);
    bool quarantined() const { return quarantine_; }
private:
    char buf_[kCap] = {0};
    size_t len_ = 0;
    bool overflow_ = false;
    bool quarantine_ = false;
    uint32_t lastByteMs_ = 0;
    bool quietBefore_ = false;   // silence before the current line (or its blank lead-in)
    bool lineQuiet_ = false;     // the current line began after silence
};

// ---- wifi add over the port ----------------------------------------------------------

/// Inter-byte gap and whole-read limit for the `wifi add` payload (it is
/// at most 97 bytes: under 2 ms of wire time).
constexpr uint32_t kAddGapMs = 1000;
constexpr uint32_t kAddTotalMs = 2000;

using SaveFn = WifiList::AddResult (*)(const char* name, const char* pass);

/// `wifi add <args>`, after its line: reads exactly <len> payload bytes,
/// then requires the input to stay quiet for kQuietGapMs (a sender that
/// sends more than it announced is refused, nothing saved). Every refusal
/// but Full (no readable length, a length over kFrameMax, a stalled or
/// over-long payload, a bad checksum or shape, `refuse` set) puts `input`
/// in quarantine until the client sends kResyncLine. A client must wait
/// for the add's reply before sending anything else. `refuse` (a reason
/// word) refuses before reading, e.g. while a `wifi try` runs. Deadlines
/// are checked on every byte. The reply line goes to the port. Frame, name
/// and password buffers are wiped on every path; `save` is called only for
/// a whole, quiet, valid frame.
void handleAdd(const char* args, Port& port, LineInput& input, SaveFn save, const char* refuse);

} // namespace UsbWifi

#endif // USB_WIFI_H
