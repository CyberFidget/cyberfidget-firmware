// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#ifndef WIFI_LIST_H
#define WIFI_LIST_H

// The saved WiFi networks (up to three) and the order a session tries them
// in. Pure rules: no radio, no storage library; the device side is
// SavedWifi (lib/CloudSync/SavedWifi.h). Host-tested in test/test_sync_wifilist.
//
// Stored in NVS namespace `wificfg`. The list is the source of truth:
//   bank          which copy of the list is current: 1 = keys "a..", 2 = "b.."
//   an / bn       how many networks that copy holds
//   as0..as2      network names, in order; the first is tried first
//   ap0..ap2      their passwords
//   ah            where the first network was last joined: "<channel>:<access
//                 point address, 12 hex>" (absent when not known)
//   ac            checksum of that copy, written last
//   lsync         set while a save is under way (the older keys may be half
//                 written; the list is rebuilt into them at the next read)
//   ssid, pass    the older single-network keys. A copy of the first network,
//                 so an older image (after a return to the previous version)
//                 still finds one. A list is created from them the first time
//                 this image reads the namespace, and a network an older image
//                 saved there becomes first - only when no save was cut short.
//
// A save writes the copy that is NOT current, then flips `bank` (one write),
// so a power cut leaves either the whole earlier list or the whole new one.

#include <stddef.h>
#include <stdint.h>

namespace WifiList {

constexpr int kMax = 3;
constexpr size_t kNameMax = 32;   // the radio's own limit
constexpr size_t kPassMax = 64;

constexpr char kNamespace[] = "wificfg";
constexpr char kKeyBank[] = "bank";
constexpr char kKeySync[] = "lsync";
constexpr char kKeyLegacyName[] = "ssid";
constexpr char kKeyLegacyPass[] = "pass";

struct Network {
    char name[kNameMax + 1] = {0};
    char pass[kPassMax + 1] = {0};
};

// Where the first network was joined last time. Only ever describes
// nets[0]; any change of the first network drops it.
struct Hint {
    bool valid = false;
    uint8_t channel = 0;
    uint8_t address[6] = {0};
};

struct List {
    Network nets[kMax];
    int count = 0;
    Hint hint;
};

// Key/value storage (NVS on the device, a map in tests).
class Store {
public:
    virtual ~Store() = default;
    virtual bool has(const char* key) = 0;
    /// Copies the value (NUL-terminated, truncated to len-1); false if absent.
    virtual bool getString(const char* key, char* out, size_t len) = 0;
    virtual uint32_t getUInt(const char* key) = 0;
    virtual bool putString(const char* key, const char* value) = 0;
    virtual bool putUInt(const char* key, uint32_t value) = 0;
    virtual bool remove(const char* key) = 0;
};

/// Reads the list. Creates it from the single-network keys the first time,
/// and takes a network an older image saved there (it becomes first).
/// `repair` is set when what was read differs from what is stored (the
/// caller writes the list back with save()).
void load(Store& store, List& out, bool& repair);

/// Writes the whole list (see the stored form above).
bool save(Store& store, const List& list);

int find(const List& list, const char* name);

enum class AddResult : uint8_t { Added, Updated, Full, Invalid };
/// Saves a network as the first one to try. A saved name gets the new
/// password and moves first. A fourth network is refused (Full): nothing
/// is dropped without the person choosing which.
AddResult add(List& list, const char* name, const char* pass);

bool forget(List& list, const char* name);
/// "Use this first": moves a saved network to the front.
bool useFirst(List& list, const char* name);

/// A join worked on nets[index] at this channel and access point: it moves
/// first and remembers where it was. True when anything changed (only then
/// does the caller write - the common path writes nothing).
bool markJoined(List& list, int index, uint8_t channel, const uint8_t address[6]);

/// Wipes the passwords (and everything else) from memory.
void wipe(List& list);

// ---- join order -----------------------------------------------------------------

/// The first attempt stops as soon as its network is reported absent when
/// the session bails on absence anyway (scheduled sessions), when it was a
/// quick attempt at a remembered place, or when other networks are saved.
bool firstEndsOnAbsent(int count, bool hinted, bool scheduled);

/// After the first attempt fails: one scan, unless the only saved network
/// was just looked for by the connect's own full scan.
bool fallbackScan(int count, bool hinted);

struct Seen {
    const char* name;
    int rssi;
};
/// The strongest saved network among the scan's results; -1 when none is
/// there. `seenIndex` gets its position in `seen`.
int pickFromScan(const List& list, const Seen* seen, int seenCount, int& seenIndex);

/// One scan record at a time (the scan can return any number): true when
/// it is a saved network stronger than the best so far, which it becomes.
struct Pick {
    int index = -1;   ///< list position, -1 while none
    int rssi = 0;
};
bool consider(const List& list, const char* name, int rssi, Pick& best);

// ---- why a join failed -------------------------------------------------------------

enum class JoinFailure : uint8_t { None, NoneSaved, Stopped, Auth, Absent, Timeout };

/// The station's disconnect reasons (wifi_err_reason_t numbering): the
/// password or key exchange failed (MIC failure, 4-way / handshake timeout,
/// 802.1X failure, auth fail).
bool isAuthReason(uint8_t reason);
/// No access point of that name (or none it may join) was found.
bool isAbsentReason(uint8_t reason);

/// From how the join ended and the disconnect reasons seen while it ran: a
/// refused password wins over "not found" (an attempt can see both), and a
/// join that just ran out of time with neither is a timeout.
JoinFailure joinFailure(bool ok, bool noneSaved, bool stopped, bool absent,
                        bool sawAuth, bool sawAbsent);

// ---- stored form of the hint -------------------------------------------------------

void addressToHex(const uint8_t address[6], char out[13]);
bool addressFromHex(const char* hex, uint8_t out[6]);

} // namespace WifiList

#endif // WIFI_LIST_H
