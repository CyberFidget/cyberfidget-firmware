// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

// The update prompt and its policy: Install / Remind me later / Skip this
// version, Auto-check Off, the Bluetooth restart path for a manual check,
// and the app auto-apply setting kept apart from firmware offers.

#include <unity.h>
#include <string.h>

#include "CheckinPolicy.h"
#include "PromptPolicy.h"

using namespace PromptPolicy;
using CheckinPolicy::Policy;
using CheckinPolicy::Session;
using CheckinPolicy::Verdict;

namespace {
constexpr const char* kRunning = "1.2.2+8d08668";

// A prompt fixture: the offer and where it came from.
struct Fixture {
    const char* version;
    const char* source;
};
const Fixture kOffer = {"1.4.0", "cyberfidget.com"};
const Fixture kNewer = {"1.5.0", "cyberfidget.com"};

// Automatic-session inputs that pass everything but the policy.
CheckinPolicy::Inputs eligible(Policy p) {
    CheckinPolicy::Inputs in;
    in.policy = p;
    in.wifiSaved = true;
    in.linked = true;
    in.vbatMv = 3900;
    in.socPct = 70;
    in.due = true;
    in.atIdleMenu = true;
    return in;
}
} // namespace

void setUp(void) {}
void tearDown(void) {}

// ---- versions ------------------------------------------------------------------

void test_version_parse_whole_string(void) {
    Version v;
    TEST_ASSERT_TRUE(parseVersion("1.2.2+8d08668", v));
    TEST_ASSERT_EQUAL_UINT32(1, v.core[0]);
    TEST_ASSERT_EQUAL_UINT32(2, v.core[1]);
    TEST_ASSERT_EQUAL_UINT32(2, v.core[2]);
    TEST_ASSERT_EQUAL_STRING("", v.pre);
    TEST_ASSERT_TRUE(parseVersion("1.3.3+512a7ed.dirty", v));   // the device's own form
    TEST_ASSERT_TRUE(parseVersion("1.4.0-rc.1+build.7", v));
    TEST_ASSERT_EQUAL_STRING("rc.1", v.pre);
    TEST_ASSERT_TRUE(parseVersion("0.0.0", v));

    const char* bad[] = {
        "", "2", "1.2", "v1.0.0", "1.9.9garbage", "1.9.9 ", " 1.9.9", "1.2.3.4",
        "01.2.3", "1.02.3", "1.2.3-", "1.2.3+", "1.2.3-rc..1", "1.2.3-01", "1.2.3+a_b",
        "1.2.3-rc+", "1.2.3++x",
        "1.2.3-aaaaaaaaaaaaaaaaaaaaaaaaaa",   // 32 characters: longer than accepted
    };
    for (const char* b : bad) TEST_ASSERT_FALSE_MESSAGE(parseVersion(b, v), b);
    TEST_ASSERT_FALSE(parseVersion(nullptr, v));
    TEST_ASSERT_TRUE(parseVersion("1.2.3-aaaaaaaaaaaaaaaaaaaaaaaaa", v));   // exactly 31
    TEST_ASSERT_TRUE(parseVersion("1.2.3+001", v));   // build metadata may have zeros
}

void test_version_precedence(void) {
    TEST_ASSERT_TRUE(isNewer("1.4.0", kRunning));
    TEST_ASSERT_TRUE(isNewer("1.2.10", "1.2.9"));
    TEST_ASSERT_FALSE(isNewer("1.2.2", kRunning));   // build metadata ignored
    TEST_ASSERT_FALSE(isNewer("1.2.2+other", "1.2.2+8d08668"));
    TEST_ASSERT_FALSE(isNewer("1.1.9", kRunning));
    TEST_ASSERT_FALSE(isNewer("junk", kRunning));
    TEST_ASSERT_FALSE(isNewer("1.9.9garbage", kRunning));
    TEST_ASSERT_TRUE(isNewer("1.0.0", "dev-build"));   // unparsable running
    // A final release is newer than its own prerelease, not equal to it.
    TEST_ASSERT_TRUE(isNewer("1.4.0", "1.4.0-rc1"));
    TEST_ASSERT_FALSE(isNewer("1.4.0-rc1", "1.4.0"));
    TEST_ASSERT_TRUE(isNewer("1.4.0-rc1", "1.3.9"));
    // The semver.org ordering example, each strictly newer than the last.
    const char* chain[] = {"1.0.0-alpha", "1.0.0-alpha.1", "1.0.0-alpha.beta", "1.0.0-beta",
                           "1.0.0-beta.2", "1.0.0-beta.11", "1.0.0-rc.1", "1.0.0"};
    for (int i = 1; i < 8; i++) {
        TEST_ASSERT_TRUE_MESSAGE(isNewer(chain[i], chain[i - 1]), chain[i]);
        TEST_ASSERT_FALSE_MESSAGE(isNewer(chain[i - 1], chain[i]), chain[i - 1]);
    }
    // A running prerelease is offered its final.
    TEST_ASSERT_TRUE(offerEligible("1.4.0", "", "1.4.0-rc1+abc"));
}

// ---- A1: empty cached result, skip, newer, later -------------------------------------

void test_empty_cached_result_is_no_notification(void) {
    TEST_ASSERT_FALSE(offerEligible("", "", kRunning));
    TEST_ASSERT_FALSE(offerEligible(nullptr, nullptr, kRunning));
    const PromptPlan boot = bootPlan(Policy::Auto, offerEligible("", "", kRunning), false);
    TEST_ASSERT_FALSE(boot.firmware);
    TEST_ASSERT_FALSE(boot.apps);
}

void test_skip_records_the_version_and_suppresses_only_it(void) {
    TEST_ASSERT_TRUE(offerEligible(kOffer.version, "", kRunning));
    const FwEffect skip = firmwareChoice((int)FwChoice::Skip, false);
    TEST_ASSERT_TRUE(skip.writeRej);
    TEST_ASSERT_FALSE(skip.handoff);
    TEST_ASSERT_FALSE(skip.keepInBar);
    // After the skip is stored, that exact version is not offered again...
    TEST_ASSERT_FALSE(offerEligible(kOffer.version, kOffer.version, kRunning));
    // ...but a newer one prompts again.
    TEST_ASSERT_TRUE(offerEligible(kNewer.version, kOffer.version, kRunning));
    TEST_ASSERT_TRUE(bootPlan(Policy::Auto,
        offerEligible(kNewer.version, kOffer.version, kRunning), false).firmware);
}

void test_later_and_no_answer_store_nothing(void) {
    const FwEffect later = firmwareChoice((int)FwChoice::Later, true);
    TEST_ASSERT_FALSE(later.writeRej);
    TEST_ASSERT_FALSE(later.handoff);
    TEST_ASSERT_TRUE(later.keepInBar);
    const FwEffect none = firmwareChoice((int)FwChoice::None, true);
    TEST_ASSERT_FALSE(none.writeRej);
    TEST_ASSERT_FALSE(none.handoff);
    // Nothing stored: the same version is offered at the next boot or check.
    TEST_ASSERT_TRUE(offerEligible(kOffer.version, "", kRunning));
}

void test_install_hands_off_only_when_an_update_session_exists(void) {
    const FwEffect now = firmwareChoice((int)FwChoice::Install, true);
    TEST_ASSERT_TRUE(now.handoff);
    TEST_ASSERT_FALSE(now.writeRej);
    TEST_ASSERT_FALSE(now.comingSoon);
    const FwEffect soon = firmwareChoice((int)FwChoice::Install, false);
    TEST_ASSERT_FALSE(soon.handoff);
    TEST_ASSERT_FALSE(soon.writeRej);
    TEST_ASSERT_TRUE(soon.comingSoon);
}

void test_prompt_option_indexes(void) {
    TEST_ASSERT_EQUAL_STRING("Install now", kFwOptions[(int)FwChoice::Install]);
    TEST_ASSERT_EQUAL_STRING("Remind me later", kFwOptions[(int)FwChoice::Later]);
    TEST_ASSERT_EQUAL_STRING("Skip this version", kFwOptions[(int)FwChoice::Skip]);
    TEST_ASSERT_EQUAL_STRING("Get them now", kAppOptions[(int)AppChoice::GetNow]);
    TEST_ASSERT_EQUAL_STRING("Later", kAppOptions[(int)AppChoice::Later]);
}

void test_missing_update_space_offers_website_once_per_version(void) {
    PromptPlan p = bootPlan(Policy::Auto, true, false, false, false);
    TEST_ASSERT_TRUE(p.firmware);
    TEST_ASSERT_TRUE(p.website);
    p = bootPlan(Policy::Auto, true, false, false, true);
    TEST_ASSERT_FALSE(p.firmware);
    TEST_ASSERT_FALSE(p.website);
    p = manualPlan(Policy::Auto, true, true, false, false);
    TEST_ASSERT_TRUE(p.firmware);
    TEST_ASSERT_TRUE(p.website);
    TEST_ASSERT_TRUE(p.apps);
    p = manualPlan(Policy::Auto, true, false, false, true);
    TEST_ASSERT_FALSE(p.firmware);
    p = manualPlan(Policy::Auto, true, false, true, true);
    TEST_ASSERT_TRUE(p.firmware);
    TEST_ASSERT_FALSE(p.website);
    TEST_ASSERT_FALSE(bootPlan(Policy::Never, true, false, false, false).firmware);
    SettingsState s;
    s.hasUpdateSlot = false;
    s.avail = "1.4.0";
    s.running = "1.3.0";
    char label[kRowText];
    settingsLabel(Row::Status, s, label, sizeof(label));
    TEST_ASSERT_EQUAL_STRING(kWebsiteUpdateCopy, label);
    TEST_ASSERT_NULL(strstr(label, "slot"));
    TEST_ASSERT_NULL(strstr(label, "partition"));
    TEST_ASSERT_NULL(strstr(label, "OTA"));
    s.rej = s.avail;
    settingsLabel(Row::Status, s, label, sizeof(label));
    TEST_ASSERT_EQUAL_STRING("Status: nothing waiting", label);
}

void test_prompt_title_names_the_source(void) {
    char title[64];
    firmwareTitle(title, sizeof(title), kOffer.version, kOffer.source);
    TEST_ASSERT_EQUAL_STRING("Update 1.4.0 ready (cyberfidget.com)", title);
    firmwareTitle(title, sizeof(title), "1.4.0", "github.com");
    TEST_ASSERT_EQUAL_STRING("Update 1.4.0 ready (github.com)", title);
    firmwareTitle(title, sizeof(title), "1.4.0", "");
    TEST_ASSERT_EQUAL_STRING("Update 1.4.0 ready (cyberfidget.com)", title);
}

void test_app_changes_prompt(void) {
    char title[40];
    appsTitle(title, sizeof(title), 2);
    TEST_ASSERT_EQUAL_STRING("2 app changes waiting", title);
    appsTitle(title, sizeof(title), 1);
    TEST_ASSERT_EQUAL_STRING("1 app change waiting", title);
    appsTitle(title, sizeof(title), 0);
    TEST_ASSERT_EQUAL_STRING("App changes waiting", title);
    TEST_ASSERT_TRUE(appChoice((int)AppChoice::GetNow).applyNow);
    TEST_ASSERT_FALSE(appChoice((int)AppChoice::Later).applyNow);
    TEST_ASSERT_TRUE(appChoice((int)AppChoice::Later).keepInBar);
    TEST_ASSERT_FALSE(appChoice(-1).applyNow);
}

// ---- A1: Auto-check Off ------------------------------------------------------------

void test_policy_never_blocks_automatic_sessions_not_manual(void) {
    TEST_ASSERT_FALSE(automaticAllowed(Policy::Never));
    TEST_ASSERT_TRUE(automaticAllowed(Policy::Auto));
    const CheckinPolicy::Inputs off = eligible(Policy::Never);
    TEST_ASSERT_EQUAL_INT((int)Verdict::PolicyOff, (int)CheckinPolicy::decide(Session::Boot, off));
    TEST_ASSERT_EQUAL_INT((int)Verdict::PolicyOff, (int)CheckinPolicy::decide(Session::Daily, off));
    TEST_ASSERT_EQUAL_INT((int)Verdict::PolicyOff, (int)CheckinPolicy::decide(Session::Awake, off));
    TEST_ASSERT_EQUAL_INT((int)Verdict::Start, (int)CheckinPolicy::decide(Session::Manual, off));
}

void test_policy_never_silences_popup_manual_explains(void) {
    const PromptPlan boot = bootPlan(Policy::Never, true, true);
    TEST_ASSERT_FALSE(boot.firmware);
    TEST_ASSERT_FALSE(boot.apps);
    const PromptPlan manual = manualPlan(Policy::Never, true, true);
    TEST_ASSERT_TRUE(manual.firmware);
    TEST_ASSERT_TRUE(manual.apps);
    TEST_ASSERT_TRUE(manual.explainOff);
    TEST_ASSERT_FALSE(manualPlan(Policy::Auto, false, false).explainOff);
    TEST_ASSERT_EQUAL_STRING(
        "Automatic check-ins are off. Remote changes wait for a manual check.",
        kOffExplanation);
}

// ---- A4: a manual check after Bluetooth use restarts first ---------------------------

void test_bt_tainted_manual_check_takes_reboot_path(void) {
    CheckinPolicy::Inputs in;
    in.btIdle = false;
    const Verdict tainted = CheckinPolicy::decide(Session::Manual, in);
    TEST_ASSERT_EQUAL_INT((int)Verdict::RebootFirst, (int)tainted);
    TEST_ASSERT_EQUAL_STRING("Restarting to check...", manualCopy(tainted));
    in.btIdle = true;
    const Verdict idle = CheckinPolicy::decide(Session::Manual, in);
    TEST_ASSERT_EQUAL_INT((int)Verdict::Start, (int)idle);
    TEST_ASSERT_EQUAL_STRING("Checking for updates...", manualCopy(idle));
    in.sessionRunning = true;
    TEST_ASSERT_EQUAL_STRING(kAlreadyChecking,
                             manualCopy(CheckinPolicy::decide(Session::Manual, in)));
}

// A manual check after Bluetooth use goes through a restart. The one-shot
// written before it, and what the next start does with it, as the device
// uses them (CloudSync::runSession writes restartForCheck(); AppManager's
// setup reads it back through resumeAfterRestart(); the Check for updates
// screen enters through checkEntry()).
void test_bt_restart_handoff_carries_get_them_now(void) {
    const RestartOneShot shot = restartForCheck(true);
    TEST_ASSERT_TRUE(shot.bootcloud);
    TEST_ASSERT_TRUE(shot.skipanim);
    TEST_ASSERT_TRUE(shot.bootapply);   // "Get them now" survives the restart
    const CheckResume resume = resumeAfterRestart(shot.bootcloud, shot.bootapply, false);
    TEST_ASSERT_TRUE(resume.openCheckScreen);
    TEST_ASSERT_TRUE(resume.runCheck);
    TEST_ASSERT_TRUE(resume.applyWaiting);

    // A plain check does not apply waiting changes after the restart.
    const RestartOneShot plain = restartForCheck(false);
    TEST_ASSERT_FALSE(plain.bootapply);
    TEST_ASSERT_FALSE(resumeAfterRestart(plain.bootcloud, plain.bootapply, false).applyWaiting);

    // A stray apply flag alone does nothing; a portal or Music Player
    // relaunch wins over the check.
    TEST_ASSERT_FALSE(resumeAfterRestart(false, true, false).runCheck);
    TEST_ASSERT_FALSE(resumeAfterRestart(true, true, true).runCheck);
    TEST_ASSERT_FALSE(resumeAfterRestart(true, true, true).openCheckScreen);
}

void test_resumed_check_screen_shows_one_result(void) {
    // The recovery check started and already finished (a fast failure such as
    // not linked) before the screen's first pass: the screen shows that
    // result and never starts a second check.
    TEST_ASSERT_EQUAL_INT((int)CheckEntry::WatchSession, (int)checkEntry(true, false));
    TEST_ASSERT_EQUAL_INT((int)CheckEntry::WatchSession, (int)checkEntry(true, true));
    // The recovery session could not start: the screen starts its own check.
    TEST_ASSERT_EQUAL_INT((int)CheckEntry::StartNew, (int)checkEntry(false, false));
    // Opened from the menu while the start-up check runs: watch it.
    TEST_ASSERT_EQUAL_INT((int)CheckEntry::WatchSession, (int)checkEntry(false, true));
}

// ---- The start-up animation's first frame comes before the slow start-up steps ---------

void test_early_animation_frame_only_on_an_ordinary_start(void) {
    StartShots plain;
    TEST_ASSERT_TRUE(earlyAnimationFrame(plain));
    // Each of these starts opens on something else (or draws nothing).
    StartShots s;
    s = plain; s.imagePending = true; TEST_ASSERT_FALSE(earlyAnimationFrame(s));
    s = plain; s.timerWake = true;    TEST_ASSERT_FALSE(earlyAnimationFrame(s));
    s = plain; s.skipanim = true;     TEST_ASSERT_FALSE(earlyAnimationFrame(s));
    s = plain; s.portal = true;       TEST_ASSERT_FALSE(earlyAnimationFrame(s));
    s = plain; s.music = true;        TEST_ASSERT_FALSE(earlyAnimationFrame(s));
    s = plain; s.link = true;         TEST_ASSERT_FALSE(earlyAnimationFrame(s));
    s = plain; s.wasmApp = true;      TEST_ASSERT_FALSE(earlyAnimationFrame(s));
    s = plain; s.bootcloud = true;    TEST_ASSERT_FALSE(earlyAnimationFrame(s));
    // A resumed check behind a portal one-shot still opens the portal: no frame.
    s = plain; s.bootcloud = true; s.portal = true; TEST_ASSERT_FALSE(earlyAnimationFrame(s));
}

// ---- A5: app auto-apply is independent of Auto-check and of firmware --------------------

void test_autoapply_defaults_on_and_off_leaves_batches_pending(void) {
    TEST_ASSERT_TRUE(parseAutoapply(false, false));   // missing key = on
    TEST_ASSERT_FALSE(parseAutoapply(true, false));
    TEST_ASSERT_EQUAL_INT((int)AppBatch::Apply, (int)appBatch(true, false));
    TEST_ASSERT_EQUAL_INT((int)AppBatch::LeavePending, (int)appBatch(false, false));
    TEST_ASSERT_EQUAL_INT((int)AppBatch::Apply, (int)appBatch(false, true));   // Get them now
}

void test_settings_are_independent_and_firmware_stays_an_offer(void) {
    // Every combination of the two settings.
    const Policy policies[2] = {Policy::Auto, Policy::Never};
    for (int p = 0; p < 2; p++) {
        for (int a = 0; a < 2; a++) {
            // App auto-apply does not change whether automatic sessions run.
            TEST_ASSERT_EQUAL(p == 0, automaticAllowed(policies[p]));
            // Manual checks run under every combination.
            CheckinPolicy::Inputs in = eligible(policies[p]);
            TEST_ASSERT_EQUAL_INT((int)Verdict::Start,
                                  (int)CheckinPolicy::decide(Session::Manual, in));
        }
    }
    // Only Install now leads to an update session.
    for (int c = -1; c <= 2; c++) {
        TEST_ASSERT_EQUAL(c == (int)FwChoice::Install, firmwareChoice(c, true).handoff);
    }
    // With Auto-check Off the required explanation is shown.
    SettingsState s;
    s.policy = Policy::Never;
    char label[kRowText];
    settingsLabel(Row::Status, s, label, sizeof(label));
    TEST_ASSERT_EQUAL_STRING(kOffExplanation, label);
    // The app-change setting says nothing about firmware.
    settingsLabel(Row::AutoApply, s, label, sizeof(label));
    TEST_ASSERT_EQUAL_STRING("Apply app changes automatically: On", label);
}

// ---- dev mode --------------------------------------------------------------------

// Dev mode moved to its own "Awake & dev mode" screen (AwakePolicy, tested in
// test_upd_policy_awake); Settings > Updates keeps a row that opens it.
void test_dev_mode_values(void) {
    SettingsState s;
    char label[kRowText];
    settingsLabel(Row::Awake, s, label, sizeof(label));
    TEST_ASSERT_EQUAL_STRING("Awake & dev mode: Off", label);
    s.awake.mode = AwakePolicy::Mode::Dev;
    s.awake.stop = AwakePolicy::Stop::UntilStopped;
    settingsLabel(Row::Awake, s, label, sizeof(label));
    TEST_ASSERT_EQUAL_STRING("Awake & dev mode: Dev mode", label);
    s.awake.mode = AwakePolicy::Mode::StayAwake;
    settingsLabel(Row::Awake, s, label, sizeof(label));
    TEST_ASSERT_EQUAL_STRING("Awake & dev mode: Stay awake", label);
}

void test_nvs_keys_fit(void) {
    const char* keys[] = {kKeyPolicy, kKeyRej, kKeyAvail, kKeyAvailN, kKeySrc, kKeyChan,
                          kKeyWebsiteSeen,
                          kKeyAutoapply, AwakePolicy::kKeyMode, AwakePolicy::kKeyStop,
                          AwakePolicy::kLegacyKeyDev, AwakePolicy::kLegacyKeyDevIdle,
                          "interval_h", "last_chk",
                          "seen_ts", "pend"};
    for (const char* k : keys) TEST_ASSERT_TRUE(strlen(k) <= kMaxKeyLen);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_version_parse_whole_string);
    RUN_TEST(test_version_precedence);
    RUN_TEST(test_empty_cached_result_is_no_notification);
    RUN_TEST(test_skip_records_the_version_and_suppresses_only_it);
    RUN_TEST(test_later_and_no_answer_store_nothing);
    RUN_TEST(test_install_hands_off_only_when_an_update_session_exists);
    RUN_TEST(test_prompt_option_indexes);
    RUN_TEST(test_missing_update_space_offers_website_once_per_version);
    RUN_TEST(test_prompt_title_names_the_source);
    RUN_TEST(test_app_changes_prompt);
    RUN_TEST(test_policy_never_blocks_automatic_sessions_not_manual);
    RUN_TEST(test_policy_never_silences_popup_manual_explains);
    RUN_TEST(test_bt_tainted_manual_check_takes_reboot_path);
    RUN_TEST(test_bt_restart_handoff_carries_get_them_now);
    RUN_TEST(test_resumed_check_screen_shows_one_result);
    RUN_TEST(test_early_animation_frame_only_on_an_ordinary_start);
    RUN_TEST(test_autoapply_defaults_on_and_off_leaves_batches_pending);
    RUN_TEST(test_settings_are_independent_and_firmware_stays_an_offer);
    RUN_TEST(test_dev_mode_values);
    RUN_TEST(test_nvs_keys_fit);
    return UNITY_END();
}
