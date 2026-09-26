// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#ifndef PROMPT_POLICY_H
#define PROMPT_POLICY_H

// What the update prompt offers and what each answer does. Pure rules over
// plain inputs (no Arduino headers, no storage, no display), so the native
// test_upd_policy suite drives every transition. The device glue
// (lib/UpdatePrompt) reads NVS `upd`, opens the prompts and writes what an
// answer says to write.
//
// Stored settings (NVS namespace `upd`, keys at most 15 characters):
//   policy        "never" = automatic check-ins off; anything else = auto
//   rej           the firmware version the person chose to skip
//   avail         the newest firmware version known to be offered
//   avail_n       short notes for that version
//   src, chan     where updates come from, and the release channel
//   autoapply     app changes apply at check-in (default on)
// The Awake & dev mode keys (`awake`, `awake_stop`) are AwakePolicy's.

#include <stddef.h>
#include <stdint.h>

#include "AwakePolicy.h"
#include "CheckinPolicy.h"

namespace PromptPolicy {

// ---- NVS keys ---------------------------------------------------------------

constexpr const char* kNamespace   = "upd";
constexpr const char* kKeyPolicy   = "policy";
constexpr const char* kKeyRej      = "rej";
constexpr const char* kKeyAvail    = "avail";
constexpr const char* kKeyAvailN   = "avail_n";
constexpr const char* kKeySrc      = "src";
constexpr const char* kKeyChan     = "chan";
constexpr const char* kKeyAutoapply = "autoapply";

/// NVS keys are limited to 15 characters.
constexpr size_t kMaxKeyLen = 15;
constexpr size_t keyLen(const char* k) { return *k ? 1 + keyLen(k + 1) : 0; }
static_assert(keyLen(kKeyAutoapply) <= kMaxKeyLen, "NVS key too long");
static_assert(keyLen(AwakePolicy::kLegacyKeyDevIdle) <= kMaxKeyLen, "NVS key too long");

constexpr const char* kPolicyAuto  = "auto";
constexpr const char* kPolicyNever = "never";
constexpr const char* kDefaultSource  = "cyberfidget.com";
constexpr const char* kDefaultChannel = "stable";
/// Explicit selection wins; an unset selection follows a running prerelease.
const char* selectedChannel(const char* stored, const char* running);

// ---- Copy (what the screen says) ---------------------------------------------

constexpr const char* kFwOptions[3]  = {"Install now", "Remind me later", "Skip this version"};
constexpr const char* kAppOptions[2] = {"Get them now", "Later"};
constexpr const char* kOffExplanation =
    "Automatic check-ins are off. Remote changes wait for a manual check.";
/// Shown (scrolling) when "Check at start-up" is changed.
constexpr const char* kBootCheckExplanation =
    "Checks for updates when you wake your Fidget. Turning it off avoids a short "
    "restart when you open a new app.";
constexpr const char* kRestartingToCheck = "Restarting to check...";
constexpr const char* kChecking = "Checking for updates...";
constexpr const char* kAlreadyChecking = "A check is already running";
/// Install now while no update session exists on the device yet.
constexpr const char* kInstallComingSoon =
    "Installing on your Fidget is coming soon. Update it from the website for now.";
constexpr const char* kWebsiteUpdateCopy =
    "Update once on website for WiFi updates";
constexpr const char* kKeyWebsiteSeen = "web_seen";
static_assert(keyLen(kKeyWebsiteSeen) <= kMaxKeyLen, "NVS key too long");

// ---- Versions ------------------------------------------------------------------

/// Longest version text accepted (stored `avail`/`rej`, offers). Longer text
/// is refused, never truncated.
constexpr size_t kMaxVersionLen = 31;

/// A semantic version: MAJOR.MINOR.PATCH, optional -prerelease (dot-separated
/// [0-9A-Za-z-] identifiers, numeric ones without leading zeros), optional
/// +build (dot-separated [0-9A-Za-z-] identifiers, ignored for ordering).
struct Version {
    uint32_t core[3] = {0, 0, 0};
    char pre[kMaxVersionLen + 1] = {0};   ///< prerelease without the '-'; "" = final
};

/// Validates the WHOLE text (at most kMaxVersionLen characters). Trailing
/// garbage, missing parts, leading zeros and empty identifiers are refused.
bool parseVersion(const char* text, Version& out);

/// Semantic-version precedence: <0, 0, >0. A prerelease sorts below its final.
int compareVersions(const Version& a, const Version& b);

/// True when `offered` is a strictly newer version than `running`. An offer
/// that does not parse is never newer; a running version that does not parse
/// (a development build) accepts any valid offer.
bool isNewer(const char* offered, const char* running);

/// Exact text match of the skipped version (the whole stored string).
bool sameVersion(const char* a, const char* b);

// ---- Firmware offer ------------------------------------------------------------

/// Whether a firmware offer is shown: a known version (empty = no
/// notification, not an error), not the skipped one, newer than running.
bool offerEligible(const char* avail, const char* rej, const char* running);

enum class FwChoice : int8_t { None = -1, Install = 0, Later = 1, Skip = 2 };

struct FwEffect {
    bool writeRej = false;     ///< store the offered version in `rej`
    bool handoff = false;      ///< start the update session (one-shot + restart)
    bool comingSoon = false;   ///< no update session yet: say so, store nothing
    bool keepInBar = false;    ///< the offer stays in the status bar
};

/// What an answer to the firmware prompt does. `choice` is the prompt's
/// result index (ModalPrompt::kNoChoice = -1 for a timeout or teardown).
/// Remind me later and no answer store nothing. Firmware is never installed
/// without Install now.
FwEffect firmwareChoice(int choice, bool updateSessionAvailable);

// ---- App changes ---------------------------------------------------------------

enum class AppChoice : int8_t { None = -1, GetNow = 0, Later = 1 };

struct AppEffect {
    bool applyNow = false;     ///< run a check that applies the waiting changes
    bool keepInBar = false;
};

AppEffect appChoice(int choice);

/// Stored `upd.autoapply`: missing reads as on.
bool parseAutoapply(bool present, bool stored);

enum class AppBatch : uint8_t { Apply, LeavePending };

/// What a check-in does with waiting app changes. `applyOnce` is the
/// person's "Get them now" for this one session.
AppBatch appBatch(bool autoapply, bool applyOnce);

// ---- When a prompt appears -------------------------------------------------------

struct PromptPlan {
    bool firmware = false;     ///< show the firmware prompt
    bool website = false;      ///< show website instruction instead of install choices
    bool apps = false;         ///< show the app-changes prompt
    bool explainOff = false;   ///< say that automatic check-ins are off
};

/// After the start-up animation: Auto-check Off silences the popup.
PromptPlan bootPlan(CheckinPolicy::Policy policy, bool fwEligible, bool appsWaiting,
                    bool hasUpdateSlot = true, bool websiteSeen = false);

/// After a manual check: always shown, and explains Auto-check Off.
PromptPlan manualPlan(CheckinPolicy::Policy policy, bool fwEligible, bool appsWaiting,
                      bool hasUpdateSlot = true, bool websiteSeen = false);

/// Whether an automatic session (boot, daily, awake) may run at all.
bool automaticAllowed(CheckinPolicy::Policy policy);

/// What the screen says when a manual check is asked for, by the verdict of
/// CheckinPolicy::decide(Session::Manual, ...).
const char* manualCopy(CheckinPolicy::Verdict verdict);

// ---- A manual check that restarts first (after Bluetooth use) ------------------

/// The `bootcfg` one-shot written before that restart.
struct RestartOneShot {
    bool bootcloud = false;   ///< run the check (a recovery session) after the restart
    bool bootapply = false;   ///< that check applies waiting app changes ("Get them now")
    bool skipanim = false;
};
RestartOneShot restartForCheck(bool applyWaiting);

/// What a start that consumed the one-shot does. `otherAppFirst`: a one-shot
/// for the portal or Music Player was also set, and wins.
struct CheckResume {
    bool openCheckScreen = false;   ///< show the Check for updates screen
    bool runCheck = false;          ///< start the recovery session
    bool applyWaiting = false;      ///< ...applying waiting app changes
};
CheckResume resumeAfterRestart(bool bootcloud, bool bootapply, bool otherAppFirst);

/// What decides the first screen of a start, read before the filesystem is
/// mounted.
struct StartShots {
    bool imagePending = false;   ///< a just-installed update not yet kept
    bool timerWake = false;      ///< the battery timer wake (never draws)
    bool skipanim = false;
    bool portal = false;         ///< bootapp
    bool music = false;          ///< bootmusic
    bool link = false;           ///< bootlink
    bool wasmApp = false;        ///< a delivered app to reopen (wasmid set)
    bool bootcloud = false;      ///< a check resumed after a restart
};
/// True when this start opens on the start-up animation for certain, so its
/// first frame can be drawn before the slower start-up steps. Any restart
/// one-shot, a pending update or a timer wake says no (those starts draw
/// their own first screen, or none).
bool earlyAnimationFrame(const StartShots& s);

enum class CheckEntry : uint8_t { StartNew, WatchSession };

/// How the Check for updates screen begins its first pass. A screen resumed
/// after the restart whose check already started watches that session even
/// when it has already finished (its result is shown once; no second check
/// starts). Otherwise a running session is watched, and with none a new
/// check starts.
CheckEntry checkEntry(bool resumedSessionStarted, bool sessionBusy);

// ---- Titles --------------------------------------------------------------------

/// "Update 1.4.0 ready (cyberfidget.com)"; an empty source reads as the default.
void firmwareTitle(char* out, size_t len, const char* version, const char* source);

/// "2 app changes waiting", "1 app change waiting"; 0 (count unknown) gives
/// "App changes waiting".
void appsTitle(char* out, size_t len, uint32_t count);

// ---- Settings > Updates ---------------------------------------------------------

enum class Row : uint8_t {
    CheckNow, AutoCheck, BootCheck, ShareBattery, AutoApply, Channel, Source, Skip, Link, Awake, Status,
};

struct SettingsState {
    CheckinPolicy::Policy policy = CheckinPolicy::Policy::Auto;
    bool bootCheck = true;      ///< "Check at start-up"
    bool shareBattery = false;
    bool autoapply = true;
    const char* channel = "";   ///< empty reads as the default
    const char* source = "";    ///< empty reads as the default
    const char* avail = "";
    const char* rej = "";
    const char* running = "";
    bool linked = false;
    bool hasUpdateSlot = true;
    AwakePolicy::Setting awake;   ///< the Awake & dev mode setting (row opens its screen)
    const char* status = "";    ///< the status bar's current line, "" = none
};

constexpr int kSettingsRows = 11;
constexpr int kRowText = 96;

/// Row kinds in screen order (always kSettingsRows of them).
Row settingsRow(int index);

/// The label for one row.
void settingsLabel(Row row, const SettingsState& s, char* out, size_t len);

enum class SkipAction : uint8_t { Nothing, Skip, Unskip };

/// Skip/Unskip current: a stored skip is undone first; otherwise the
/// currently offered newer version can be skipped.
SkipAction skipAction(const SettingsState& s);

} // namespace PromptPolicy

#endif
