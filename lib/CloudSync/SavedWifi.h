// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#ifndef SAVED_WIFI_H
#define SAVED_WIFI_H

// Device side of the saved WiFi networks (rules: WifiList.h): reads and
// writes them in NVS, and joins one for any session that needs the
// station (every check-in session, linking, the update session, the bench
// probe).
//
// Join order: the network that worked last, at the channel and access point
// it was on then (no scan: a quick join); if that fails, one scan and the
// strongest saved network present, remembered for next time.

#include <stddef.h>
#include <stdint.h>

#include "WifiList.h"

namespace SavedWifi {

/// Reads the list (writing it back when it had to be migrated or repaired).
/// The caller wipes it (WifiList::wipe) when it holds passwords.
bool load(WifiList::List& out);

/// True when at least one network is saved (read-only, cheap).
bool anySaved();

/// Names only, in the order they are tried. Returns the count.
int names(char out[][WifiList::kNameMax + 1], int max);

WifiList::AddResult add(const char* name, const char* pass);
bool forget(const char* name);
bool useFirst(const char* name);

/// A join by someone else (the portal's own connection) worked: remember it.
void noteJoined(const char* name);

struct JoinOptions {
    uint32_t firstMs = 10000;      ///< the first attempt's budget
    uint32_t fallbackMs = 10000;   ///< the join after the scan
    bool scheduled = false;        ///< an automatic session: absent ends an attempt
    /// Polled every 100 ms; true stops the join (cancel, session deadline).
    bool (*stop)(void*) = nullptr;
    void* ctx = nullptr;
    /// Also polled every 100 ms (the update session feeds its watchdog).
    void (*tick)(void*) = nullptr;
    /// Only the first saved network, by a plain join (no remembered place,
    /// no scan for the others): `wifi try` reports on exactly that one.
    /// Ends early when it is not found or its password is refused twice.
    bool firstOnly = false;
#ifdef CF_TEST_CLI
    /// Bench: the first attempt looks for this name instead (not in range).
    const char* benchFirstName = nullptr;
#endif
};

struct JoinResult {
    bool ok = false;
    bool noneSaved = false;
    bool absent = false;       ///< no saved network was found
    bool stopped = false;
    bool hinted = false;       ///< the first attempt used the remembered place
    bool scanned = false;
    uint8_t scanStarts = 0;    ///< tries it took to start the scan
    bool scanFailed = false;   ///< it gave no result: a plain join was tried instead
    int8_t joined = -1;        ///< the list position that worked (before it moves first)
    uint32_t firstMs = 0;
    uint32_t scanMs = 0;
    uint32_t fallbackMs = 0;
    uint32_t totalMs = 0;
    /// Why it failed (None on success), from the station's disconnect reasons.
    WifiList::JoinFailure failure = WifiList::JoinFailure::None;
    uint8_t lastReason = 0;    ///< the last disconnect reason seen (0: none)
    /// The network that was joined (empty unless ok). A name, never a key.
    char name[WifiList::kNameMax + 1] = {0};
};

/// The station must be on (WIFI_STA or WIFI_AP_STA) and not connected.
/// Leaves it connected on success. Prints one `[wifi] join` line.
bool join(const JoinOptions& opt, JoinResult& out);

#ifdef CF_TEST_CLI
/// `wifi list|first|forget|hint-bad|clear` bench verbs.
void cliCommand(const char* args);
#endif

} // namespace SavedWifi

#endif // SAVED_WIFI_H
