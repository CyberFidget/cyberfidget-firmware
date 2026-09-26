// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#ifndef AWAKE_POLICY_H
#define AWAKE_POLICY_H

// "Awake & dev mode": the rules behind the three modes. Pure rules over plain
// inputs (no Arduino headers, no clock, no storage), so the native
// test_upd_policy suite drives every decision. The device glue
// (lib/UpdatePrompt/AwakeMode) reads and writes NVS, runs the screen, keeps
// the Fidget awake and starts dev mode listening.
//
//   Off         sleeps after 60 s without use, checks in once a day (default)
//   Stay awake  never sleeps on its own; Bluetooth works; no network
//   Dev mode    stays awake and listens for sent apps; Bluetooth memory is
//               released at start-up, so a Bluetooth app needs a restart
//
// Both awake modes are a latch kept across restarts and power cycles. They
// end by themselves: after 30 min without use (unless "Until I stop it"),
// after 48 h without a button press either way, and at the battery floor.
//
// Stored settings (NVS namespace `upd`, keys at most 15 characters):
//   awake       u8  0 off, 1 stay awake, 2 dev mode
//   awake_stop  u8  0 after 30 min without use, 1 until I stop it
// Legacy (the earlier Settings > Updates "Dev mode" row), adopted once and
// left in place (an image that rolls back still reads its own key):
//   dev         u8  0 off, 1 on (idle timeout), 2 always on
//   dev_idle_min u32 (ignored: the idle period is fixed at 30 min)

#include <stddef.h>
#include <stdint.h>

namespace AwakePolicy {

enum class Mode : uint8_t { Off = 0, StayAwake = 1, Dev = 2 };
enum class Stop : uint8_t { AfterIdle = 0, UntilStopped = 1 };

struct Setting {
    Mode mode = Mode::Off;
    Stop stop = Stop::AfterIdle;
};

bool sameSetting(const Setting& a, const Setting& b);

// ---- Storage -------------------------------------------------------------------

constexpr const char* kKeyMode = "awake";
constexpr const char* kKeyStop = "awake_stop";
constexpr const char* kLegacyKeyDev = "dev";
constexpr const char* kLegacyKeyDevIdle = "dev_idle_min";

/// What NVS holds (missing keys have `has...` false).
struct Stored {
    bool hasMode = false;
    uint8_t mode = 0;
    bool hasStop = false;
    uint8_t stop = 0;
    bool hasLegacyDev = false;
    uint8_t legacyDev = 0;
    bool hasLegacyIdle = false;
};

struct Parsed {
    Setting setting;
    /// Only a legacy `dev` key exists: write `setting` under the new keys
    /// (the legacy keys stay).
    bool migrate = false;
};

/// Unknown values read as Off / AfterIdle. The legacy `dev` value is used
/// only when no `awake` key exists: 1 (idle timeout) -> Dev mode after 30 min
/// without use, 2 (always on) -> Dev mode until I stop it, else Off.
Parsed parseStored(const Stored& stored);

// ---- What each mode does ------------------------------------------------------

/// Idle sleep is off in both awake modes.
bool keepsAwake(const Setting& s);

/// The check-in `mode` the site records (case-sensitive): "normal" for Off
/// and Stay awake, "dev" for Dev mode after 30 min without use, "always"
/// for Dev mode until I stop it.
const char* wireMode(const Setting& s);

/// Dev mode listens in this power cycle unless the start is the restart
/// that runs a Bluetooth app (Music Player): that one power cycle has no
/// listening, and dev mode comes back at the next restart.
bool listensThisBoot(const Setting& s, bool bluetoothAppBoot);

/// Dev mode frees the Bluetooth memory at start-up (as the setup portal
/// does), in every power cycle it listens in.
bool releasesBluetoothAtBoot(const Setting& s, bool bluetoothAppBoot);

/// A Bluetooth app launched while dev mode listens asks to restart first.
bool bluetoothAppNeedsRestart(bool listening);

// ---- Ends --------------------------------------------------------------------------

constexpr uint32_t kIdleStopMs = 30u * 60u * 1000u;      // "After 30 min without use"
constexpr uint32_t kSafetyNetMs = 48u * 3600u * 1000u;   // no button press for 48 h
/// A low reading must last this long before the mode ends (one bad sample
/// is not a flat battery).
constexpr uint32_t kLowBatteryHoldMs = 60000;
/// The battery floor: the same floor automatic check-ins use.
constexpr int32_t kFloorVbatMv = 3600;
constexpr int32_t kFloorSocPct = 20;

enum class End : uint8_t { None, Idle, SafetyNet, Battery, RestartLoop };

/// Below the floor (either value) while not charging. An unreadable value
/// (-1) never counts as low: the runtime battery guard still protects a
/// Fidget whose gauge cannot be read.
bool batteryLow(int32_t vbatMv, int32_t socPct, bool charging);

struct Activity {
    uint32_t sinceUseMs = 0;       ///< since the last button press or delivered app
    uint32_t sincePressMs = 0;     ///< since the last button press
    uint32_t lowBatteryForMs = 0;  ///< how long batteryLow() has held (0 = not low)
};

/// Whether an awake mode ends now, and why. The battery floor wins, then the
/// 48 h safety net, then (After 30 min without use only) the idle stop.
/// `idleStopMs` / `safetyNetMs` are the built-in values except on the bench.
End checkEnd(const Setting& s, const Activity& a,
             uint32_t idleStopMs = kIdleStopMs, uint32_t safetyNetMs = kSafetyNetMs);

/// "Dev mode is off" / "Stay awake is off".
const char* endTitle(Mode mode);
/// "Battery low", "Not used for 30 min", "No button press for 2 days".
const char* endReason(End end);
/// Short name for the serial log.
const char* endName(End end);

// ---- Changing the mode ------------------------------------------------------------

enum class Apply : uint8_t { Nothing, Save, SaveAndRestart };

/// Entering or leaving Dev mode restarts (Bluetooth memory is only freed or
/// available again across a restart), and so does a new stop setting inside
/// Dev mode (the listening worker reports the mode to the site when it
/// starts). Off <-> Stay awake and a Stay awake stop change only save.
Apply applyEffect(const Setting& before, const Setting& after);

// ---- Listening beside an app --------------------------------------------------------

/// Whether dev mode keeps listening while this app is in front, by its
/// manifest enum name ("APP_MENU", ...). An allow-list of what was measured
/// at or above the internal-heap floor for a network session (24 KB free at
/// the trough): the menu and its system screens, Particle Sim, Snake. Every
/// other app, and anything unknown, pauses listening until it ends.
bool listensDuring(const char* appEnumName);

/// Whether opening this app first stops a running automatic check-in (start,
/// awake or recovery), by the same allow-list: an app not measured safe
/// beside a network session never starts beside one. A check the person
/// asked for (and dev mode, which has its own pause) is not stopped here.
bool stopsAutomaticSession(const char* appEnumName, bool automaticSessionRunning);

/// A delivered (WASM) app is not on that list: whether listening continues
/// beside it depends on the heap. Its interpreter stack is in PSRAM, so the
/// app itself holds only a few KB of internal RAM. Internal-heap floor at a
/// check-in's trough (the network session budget, as for the allow-list).
constexpr uint32_t kListenTroughFloor = 24u * 1024u;
/// What one check-in takes from the internal heap below the steady listening
/// level at its trough (bench: 17.5 KB with an HTTPS handshake), rounded up.
constexpr uint32_t kListenPollCost = 18u * 1024u;
/// Internal RAM a delivered app holds while it runs (bench: its task and
/// runtime objects, ~5 KB), rounded up.
constexpr uint32_t kDeliveredAppCost = 8u * 1024u;
/// Decided when the app opens: listening continues when the internal heap
/// free right then (listening already running) leaves the app its share and
/// the next check-in's trough above the floor.
bool listensBesideDeliveredApp(uint32_t freeInternal);
/// Checked after each check-in while the app runs: a trough below the floor
/// pauses listening until the app ends.
bool troughBelowFloor(uint32_t trough);

// ---- A restart loop ends the mode -------------------------------------------------

/// Consecutive abnormal resets (panic, watchdog, brownout) while an awake
/// mode is latched. A normal start (power-on, a software restart) clears it.
uint8_t nextLoopCount(uint8_t previous, bool abnormalReset, bool awakeLatched);
/// At this many, the mode turns itself off before anything else starts.
constexpr uint8_t kLoopLimit = 3;
bool loopEndsMode(uint8_t count);
/// A clean stretch clears the count: 5 min of uptime in this power cycle,
/// or dev mode's first successful check-in, whichever comes first.
constexpr uint32_t kLoopCleanMs = 5u * 60u * 1000u;
bool loopCountClears(uint32_t uptimeMs, bool devCheckedIn);

// ---- The "Awake & dev mode" screen -------------------------------------------------

constexpr int kChoices = 5;

struct Choice {
    Setting setting;
    const char* name;         ///< the `< Mode >` selector text
    const char* stopLine;     ///< "" for Off
    const char* description;  ///< scrolls under the selector
};

/// Screen order: Off, Stay awake (after 30 min), Stay awake (until I stop
/// it), Dev mode (after 30 min), Dev mode (until I stop it).
const Choice& choice(int index);
/// The choice showing `s` (Off for anything unknown).
int choiceIndex(const Setting& s);
/// Left (-1) / Right (+1), wrapping like the menu.
int stepChoice(int index, int direction);

/// "Off", "Stay awake", "Dev mode" (Settings > Updates row, status lines).
const char* modeName(Mode mode);

/// The Bluetooth-app prompt title, e.g. "Music uses Bluetooth. Restart
/// without dev mode? Dev mode comes back next restart."
void bluetoothPromptTitle(char* out, size_t len, const char* appName);
constexpr const char* kBluetoothOptions[2] = {"Restart", "Cancel"};
constexpr int kBluetoothRestart = 0;

// ---- Visible state ------------------------------------------------------------------

/// The status bar's marker: none, awake (Stay awake, or Dev mode that is not
/// listening this power cycle) or listening.
enum class Marker : uint8_t { None, Awake, Listening };
Marker marker(const Setting& s, bool listening);

/// Dev mode's breathing indicator on the back LED: a slow triangle from 0 to
/// kBreathMax and back over kBreathPeriodMs.
constexpr uint32_t kBreathPeriodMs = 4000;
constexpr uint8_t kBreathMax = 40;
uint8_t breathLevel(uint32_t nowMs);

} // namespace AwakePolicy

#endif
