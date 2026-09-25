// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

// Awake & dev mode: the stored setting and its migration, what each mode
// does, every way an awake mode ends, the screen's choices and copy, the
// Bluetooth-app restart, the status marker and the breathing indicator.

#include <unity.h>
#include <string.h>
#include <string>

#include "AwakePolicy.h"

using namespace AwakePolicy;

namespace {
Setting make(Mode m, Stop s = Stop::AfterIdle) {
    Setting out;
    out.mode = m;
    out.stop = s;
    return out;
}

Activity active() {
    Activity a;
    a.sinceUseMs = 1000;
    a.sincePressMs = 1000;
    a.lowBatteryForMs = 0;
    return a;
}
} // namespace

void setUp(void) {}
void tearDown(void) {}

// ---- storage --------------------------------------------------------------------

void test_factory_default_is_off(void) {
    Stored none;
    const Parsed p = parseStored(none);
    TEST_ASSERT_EQUAL_INT((int)Mode::Off, (int)p.setting.mode);
    TEST_ASSERT_EQUAL_INT((int)Stop::AfterIdle, (int)p.setting.stop);
    TEST_ASSERT_FALSE(p.migrate);
    TEST_ASSERT_FALSE(keepsAwake(p.setting));
    TEST_ASSERT_EQUAL_STRING("normal", wireMode(p.setting));
}

void test_stored_values_round_trip(void) {
    Stored st;
    st.hasMode = true;
    st.mode = 2;
    st.hasStop = true;
    st.stop = 1;
    Parsed p = parseStored(st);
    TEST_ASSERT_EQUAL_INT((int)Mode::Dev, (int)p.setting.mode);
    TEST_ASSERT_EQUAL_INT((int)Stop::UntilStopped, (int)p.setting.stop);
    st.mode = 1;
    st.hasStop = false;   // a missing stop key is the bounded default
    p = parseStored(st);
    TEST_ASSERT_EQUAL_INT((int)Mode::StayAwake, (int)p.setting.mode);
    TEST_ASSERT_EQUAL_INT((int)Stop::AfterIdle, (int)p.setting.stop);
    st.mode = 7;          // unknown values read as Off
    st.hasStop = true;
    st.stop = 9;
    p = parseStored(st);
    TEST_ASSERT_EQUAL_INT((int)Mode::Off, (int)p.setting.mode);
    TEST_ASSERT_EQUAL_INT((int)Stop::AfterIdle, (int)p.setting.stop);
}

void test_legacy_dev_row_migrates(void) {
    // The earlier Settings > Updates row stored dev 0/1/2 (+ dev_idle_min).
    Stored st;
    st.hasLegacyDev = true;
    st.hasLegacyIdle = true;
    st.legacyDev = 1;   // On (idle timeout)
    Parsed p = parseStored(st);
    TEST_ASSERT_TRUE(p.migrate);
    TEST_ASSERT_EQUAL_INT((int)Mode::Dev, (int)p.setting.mode);
    TEST_ASSERT_EQUAL_INT((int)Stop::AfterIdle, (int)p.setting.stop);
    st.legacyDev = 2;   // Always on
    p = parseStored(st);
    TEST_ASSERT_EQUAL_INT((int)Mode::Dev, (int)p.setting.mode);
    TEST_ASSERT_EQUAL_INT((int)Stop::UntilStopped, (int)p.setting.stop);
    st.legacyDev = 0;
    p = parseStored(st);
    TEST_ASSERT_EQUAL_INT((int)Mode::Off, (int)p.setting.mode);
    TEST_ASSERT_TRUE(p.migrate);
    // A new key wins over a leftover legacy one, and nothing is written
    // again (the legacy keys stay for an image that rolls back).
    st.legacyDev = 2;
    st.hasMode = true;
    st.mode = 1;
    p = parseStored(st);
    TEST_ASSERT_EQUAL_INT((int)Mode::StayAwake, (int)p.setting.mode);
    TEST_ASSERT_FALSE(p.migrate);
    // Only the idle key left over: nothing to adopt, nothing written.
    Stored idleOnly;
    idleOnly.hasLegacyIdle = true;
    p = parseStored(idleOnly);
    TEST_ASSERT_EQUAL_INT((int)Mode::Off, (int)p.setting.mode);
    TEST_ASSERT_FALSE(p.migrate);
}

void test_nvs_keys_fit(void) {
    const char* keys[] = {kKeyMode, kKeyStop, kLegacyKeyDev, kLegacyKeyDevIdle};
    for (const char* k : keys) TEST_ASSERT_TRUE(strlen(k) <= 15);
}

// ---- what each mode does ---------------------------------------------------------

void test_wire_mode_follows_the_site_contract(void) {
    TEST_ASSERT_EQUAL_STRING("normal", wireMode(make(Mode::Off)));
    TEST_ASSERT_EQUAL_STRING("normal", wireMode(make(Mode::StayAwake)));
    TEST_ASSERT_EQUAL_STRING("normal", wireMode(make(Mode::StayAwake, Stop::UntilStopped)));
    TEST_ASSERT_EQUAL_STRING("dev", wireMode(make(Mode::Dev)));
    TEST_ASSERT_EQUAL_STRING("always", wireMode(make(Mode::Dev, Stop::UntilStopped)));
}

void test_awake_modes_keep_the_fidget_awake(void) {
    TEST_ASSERT_FALSE(keepsAwake(make(Mode::Off)));
    TEST_ASSERT_TRUE(keepsAwake(make(Mode::StayAwake)));
    TEST_ASSERT_TRUE(keepsAwake(make(Mode::Dev, Stop::UntilStopped)));
}

void test_dev_mode_listens_except_in_the_bluetooth_restart(void) {
    TEST_ASSERT_TRUE(listensThisBoot(make(Mode::Dev), false));
    TEST_ASSERT_TRUE(releasesBluetoothAtBoot(make(Mode::Dev), false));
    // The restart that runs the Music Player has no listening and keeps
    // Bluetooth; dev mode comes back at the next restart.
    TEST_ASSERT_FALSE(listensThisBoot(make(Mode::Dev), true));
    TEST_ASSERT_FALSE(releasesBluetoothAtBoot(make(Mode::Dev), true));
    // Stay awake never listens and never touches Bluetooth.
    TEST_ASSERT_FALSE(listensThisBoot(make(Mode::StayAwake), false));
    TEST_ASSERT_FALSE(releasesBluetoothAtBoot(make(Mode::StayAwake), false));
    TEST_ASSERT_FALSE(listensThisBoot(make(Mode::Off), false));
}

void test_bluetooth_app_restart_only_while_listening(void) {
    TEST_ASSERT_TRUE(bluetoothAppNeedsRestart(true));
    TEST_ASSERT_FALSE(bluetoothAppNeedsRestart(false));
    char title[160];
    bluetoothPromptTitle(title, sizeof(title), "Music");
    TEST_ASSERT_EQUAL_STRING(
        "Music uses Bluetooth. Restart without dev mode? Dev mode comes back next restart.", title);
    TEST_ASSERT_EQUAL_STRING("Restart", kBluetoothOptions[kBluetoothRestart]);
    TEST_ASSERT_EQUAL_STRING("Cancel", kBluetoothOptions[1]);
}

// ---- ends --------------------------------------------------------------------------

void test_off_never_ends(void) {
    Activity a;
    a.sinceUseMs = kIdleStopMs * 10;
    a.sincePressMs = kSafetyNetMs * 2;
    a.lowBatteryForMs = kLowBatteryHoldMs * 2;
    TEST_ASSERT_EQUAL_INT((int)End::None, (int)checkEnd(make(Mode::Off), a));
}

void test_idle_stop_after_30_min_without_use(void) {
    Activity a = active();
    a.sinceUseMs = kIdleStopMs - 1;
    TEST_ASSERT_EQUAL_INT((int)End::None, (int)checkEnd(make(Mode::Dev), a));
    a.sinceUseMs = kIdleStopMs;
    TEST_ASSERT_EQUAL_INT((int)End::Idle, (int)checkEnd(make(Mode::Dev), a));
    TEST_ASSERT_EQUAL_INT((int)End::Idle, (int)checkEnd(make(Mode::StayAwake), a));
    // "Until I stop it" never idles out.
    TEST_ASSERT_EQUAL_INT((int)End::None, (int)checkEnd(make(Mode::Dev, Stop::UntilStopped), a));
    TEST_ASSERT_EQUAL_INT((int)End::None,
                          (int)checkEnd(make(Mode::StayAwake, Stop::UntilStopped), a));
    TEST_ASSERT_EQUAL_UINT32(30u * 60u * 1000u, kIdleStopMs);
}

void test_48h_safety_net_ends_either_stop_setting(void) {
    Activity a = active();
    a.sincePressMs = kSafetyNetMs;
    a.sinceUseMs = 0;   // apps keep arriving, but nobody pressed a button
    TEST_ASSERT_EQUAL_INT((int)End::SafetyNet, (int)checkEnd(make(Mode::Dev, Stop::UntilStopped), a));
    TEST_ASSERT_EQUAL_INT((int)End::SafetyNet,
                          (int)checkEnd(make(Mode::StayAwake, Stop::UntilStopped), a));
    TEST_ASSERT_EQUAL_INT((int)End::SafetyNet, (int)checkEnd(make(Mode::Dev), a));
    a.sincePressMs = kSafetyNetMs - 1;
    TEST_ASSERT_EQUAL_INT((int)End::None, (int)checkEnd(make(Mode::Dev, Stop::UntilStopped), a));
    TEST_ASSERT_EQUAL_UINT32(48u * 3600u * 1000u, kSafetyNetMs);
}

void test_battery_floor_ends_both_and_wins(void) {
    Activity a = active();
    a.lowBatteryForMs = kLowBatteryHoldMs;
    a.sincePressMs = kSafetyNetMs;   // also due: the battery is named
    a.sinceUseMs = kIdleStopMs;
    TEST_ASSERT_EQUAL_INT((int)End::Battery, (int)checkEnd(make(Mode::Dev, Stop::UntilStopped), a));
    TEST_ASSERT_EQUAL_INT((int)End::Battery, (int)checkEnd(make(Mode::StayAwake), a));
    // One short dip is not the floor.
    a = active();
    a.lowBatteryForMs = kLowBatteryHoldMs - 1;
    TEST_ASSERT_EQUAL_INT((int)End::None, (int)checkEnd(make(Mode::Dev), a));
}

void test_bench_timers_override_the_built_in_ones(void) {
    Activity a = active();
    a.sinceUseMs = 20000;
    a.sincePressMs = 20000;
    TEST_ASSERT_EQUAL_INT((int)End::Idle, (int)checkEnd(make(Mode::Dev), a, 15000, kSafetyNetMs));
    TEST_ASSERT_EQUAL_INT((int)End::SafetyNet,
                          (int)checkEnd(make(Mode::Dev, Stop::UntilStopped), a, kIdleStopMs, 15000));
}

void test_battery_low_reading(void) {
    TEST_ASSERT_FALSE(batteryLow(3900, 60, false));
    TEST_ASSERT_TRUE(batteryLow(3550, 60, false));    // voltage under the floor
    TEST_ASSERT_TRUE(batteryLow(3800, 19, false));    // charge under the floor
    TEST_ASSERT_FALSE(batteryLow(3550, 10, true));    // charging: not ending
    TEST_ASSERT_FALSE(batteryLow(-1, -1, false));     // unreadable is not low
    TEST_ASSERT_TRUE(batteryLow(-1, 5, false));
    TEST_ASSERT_FALSE(batteryLow(kFloorVbatMv, kFloorSocPct, false));
}

void test_end_copy_names_why(void) {
    TEST_ASSERT_EQUAL_STRING("Dev mode is off", endTitle(Mode::Dev));
    TEST_ASSERT_EQUAL_STRING("Stay awake is off", endTitle(Mode::StayAwake));
    TEST_ASSERT_EQUAL_STRING("Battery low", endReason(End::Battery));
    TEST_ASSERT_EQUAL_STRING("Not used for 30 min", endReason(End::Idle));
    TEST_ASSERT_EQUAL_STRING("No button press for 2 days", endReason(End::SafetyNet));
    TEST_ASSERT_EQUAL_STRING("battery", endName(End::Battery));
    TEST_ASSERT_EQUAL_STRING("safety-net", endName(End::SafetyNet));
    TEST_ASSERT_EQUAL_STRING("idle", endName(End::Idle));
}

// ---- changing the mode --------------------------------------------------------------

void test_entering_or_leaving_dev_mode_restarts(void) {
    TEST_ASSERT_EQUAL_INT((int)Apply::Nothing, (int)applyEffect(make(Mode::Dev), make(Mode::Dev)));
    TEST_ASSERT_EQUAL_INT((int)Apply::SaveAndRestart, (int)applyEffect(make(Mode::Off), make(Mode::Dev)));
    TEST_ASSERT_EQUAL_INT((int)Apply::SaveAndRestart, (int)applyEffect(make(Mode::Dev), make(Mode::Off)));
    TEST_ASSERT_EQUAL_INT((int)Apply::SaveAndRestart,
                          (int)applyEffect(make(Mode::StayAwake), make(Mode::Dev)));
    // A new stop setting inside Dev mode restarts too: the listening worker
    // reports "dev" / "always" to the site when it starts.
    TEST_ASSERT_EQUAL_INT((int)Apply::SaveAndRestart,
                          (int)applyEffect(make(Mode::Dev), make(Mode::Dev, Stop::UntilStopped)));
    TEST_ASSERT_EQUAL_INT((int)Apply::SaveAndRestart,
                          (int)applyEffect(make(Mode::Dev, Stop::UntilStopped), make(Mode::Dev)));
    // No restart without Dev mode on either side.
    TEST_ASSERT_EQUAL_INT((int)Apply::Save, (int)applyEffect(make(Mode::Off), make(Mode::StayAwake)));
    TEST_ASSERT_EQUAL_INT((int)Apply::Save, (int)applyEffect(make(Mode::StayAwake), make(Mode::Off)));
    TEST_ASSERT_EQUAL_INT((int)Apply::Save,
                          (int)applyEffect(make(Mode::StayAwake), make(Mode::StayAwake, Stop::UntilStopped)));
}

// ---- the screen ------------------------------------------------------------------------

void test_screen_choices_in_order(void) {
    TEST_ASSERT_EQUAL_INT(5, kChoices);
    TEST_ASSERT_EQUAL_STRING("Off", choice(0).name);
    TEST_ASSERT_EQUAL_STRING("", choice(0).stopLine);
    TEST_ASSERT_EQUAL_STRING("Stay awake", choice(1).name);
    TEST_ASSERT_EQUAL_STRING("After 30 min without use", choice(1).stopLine);
    TEST_ASSERT_EQUAL_STRING("Stay awake", choice(2).name);
    TEST_ASSERT_EQUAL_STRING("Until I stop it", choice(2).stopLine);
    TEST_ASSERT_EQUAL_STRING("Dev mode", choice(3).name);
    TEST_ASSERT_EQUAL_STRING("After 30 min without use", choice(3).stopLine);
    TEST_ASSERT_EQUAL_STRING("Dev mode", choice(4).name);
    TEST_ASSERT_EQUAL_STRING("Until I stop it", choice(4).stopLine);
    for (int i = 0; i < kChoices; i++)
        TEST_ASSERT_EQUAL_INT(i, choiceIndex(choice(i).setting));
    // Off shows as Off whatever stop value was stored with it.
    TEST_ASSERT_EQUAL_INT(0, choiceIndex(make(Mode::Off, Stop::UntilStopped)));
}

void test_selector_wraps_like_the_menu(void) {
    TEST_ASSERT_EQUAL_INT(1, stepChoice(0, 1));
    TEST_ASSERT_EQUAL_INT(4, stepChoice(0, -1));
    TEST_ASSERT_EQUAL_INT(0, stepChoice(4, 1));
    TEST_ASSERT_EQUAL_INT(3, stepChoice(4, -1));
    TEST_ASSERT_EQUAL_INT(1, stepChoice(99, 1));   // out of range starts from Off
}

void test_copy_names_the_trade_offs(void) {
    // Dev mode: the approved entry copy, battery, and the Bluetooth restart.
    TEST_ASSERT_NOT_NULL(strstr(choice(3).description,
                                "Dev mode keeps your Fidget awake and connected"));
    for (int i = 1; i < kChoices; i++) {
        TEST_ASSERT_NOT_NULL_MESSAGE(strstr(choice(i).description, "uses more battery"),
                                     choice(i).description);
        TEST_ASSERT_NOT_NULL(strstr(choice(i).description, "battery is low"));
    }
    TEST_ASSERT_NOT_NULL(strstr(choice(3).description, "Bluetooth apps need a restart"));
    TEST_ASSERT_NOT_NULL(strstr(choice(4).description, "Bluetooth apps need a restart"));
    TEST_ASSERT_NOT_NULL(strstr(choice(1).description, "Bluetooth apps work as usual"));
    TEST_ASSERT_NOT_NULL(strstr(choice(1).description, "checks for changes only when you ask"));
    TEST_ASSERT_NOT_NULL(strstr(choice(2).description, "checks for changes only when you ask"));
    TEST_ASSERT_NOT_NULL(strstr(choice(2).description, "2 days without a button press"));
    TEST_ASSERT_NOT_NULL(strstr(choice(4).description, "2 days without a button press"));
}

void test_user_copy_has_no_jargon(void) {
    std::string shown[40];
    int n = 0;
    for (int i = 0; i < kChoices; i++) {
        shown[n++] = choice(i).name;
        shown[n++] = choice(i).stopLine;
        shown[n++] = choice(i).description;
    }
    shown[n++] = endTitle(Mode::Dev);
    shown[n++] = endTitle(Mode::StayAwake);
    shown[n++] = endReason(End::Battery);
    shown[n++] = endReason(End::Idle);
    shown[n++] = endReason(End::SafetyNet);
    shown[n++] = endReason(End::RestartLoop);
    char title[160];
    bluetoothPromptTitle(title, sizeof(title), "Music");
    shown[n++] = title;
    shown[n++] = modeName(Mode::Dev);
    const char* banned[] = {"WiFi", "Wi-Fi", "OTA", "token", "server", "firmware", "poll",
                            "listen", "network", "internet"};
    for (int i = 0; i < n; i++) {
        for (const char* b : banned) TEST_ASSERT_NULL_MESSAGE(strstr(shown[i].c_str(), b),
                                                                shown[i].c_str());
    }
}

// ---- visible state ---------------------------------------------------------------------

void test_marker_for_each_awake_mode(void) {
    TEST_ASSERT_EQUAL_INT((int)Marker::None, (int)marker(make(Mode::Off), false));
    TEST_ASSERT_EQUAL_INT((int)Marker::Awake, (int)marker(make(Mode::StayAwake), false));
    TEST_ASSERT_EQUAL_INT((int)Marker::Listening, (int)marker(make(Mode::Dev), true));
    // Dev mode that is not listening in this power cycle (the Bluetooth
    // restart, or not connected) shows as awake, never as listening.
    TEST_ASSERT_EQUAL_INT((int)Marker::Awake, (int)marker(make(Mode::Dev), false));
    TEST_ASSERT_EQUAL_INT((int)Marker::Awake, (int)marker(make(Mode::StayAwake), true));
}

void test_breathing_indicator(void) {
    TEST_ASSERT_EQUAL_UINT8(0, breathLevel(0));
    TEST_ASSERT_EQUAL_UINT8(kBreathMax, breathLevel(kBreathPeriodMs / 2));
    TEST_ASSERT_EQUAL_UINT8(kBreathMax / 2, breathLevel(kBreathPeriodMs / 4));
    TEST_ASSERT_EQUAL_UINT8(breathLevel(1000), breathLevel(1000 + kBreathPeriodMs));
    for (uint32_t t = 0; t < kBreathPeriodMs; t += 37) TEST_ASSERT_TRUE(breathLevel(t) <= kBreathMax);
}

void test_listening_allow_list(void) {
    // Measured at or above the floor: the menu, its system screens, and two
    // apps. Everything else pauses listening.
    const char* allowed[] = {"APP_MENU", "APP_BOOT_ANIMATION", "APP_STATUS", "APP_UPDATES",
                             "APP_CHECK_UPDATES", "APP_AWAKE", "APP_SPH_FLUID_GAME", "APP_SNAKE"};
    for (const char* a : allowed) TEST_ASSERT_TRUE_MESSAGE(listensDuring(a), a);
    const char* paused[] = {"APP_VOICE_RECORDER", "APP_WASM_HOST", "APP_MUSIC_PLAYER",
                            "APP_WEB_PORTAL", "APP_LINK", "APP_BREAKOUT_GAME", "APP_TIMERS",
                            "APP_UNKNOWN", ""};
    for (const char* p : paused) TEST_ASSERT_FALSE_MESSAGE(listensDuring(p), p);
    TEST_ASSERT_FALSE(listensDuring(nullptr));
}

void test_restart_loop_ends_the_mode(void) {
    uint8_t n = 0;
    n = nextLoopCount(n, true, true);
    n = nextLoopCount(n, true, true);
    TEST_ASSERT_FALSE(loopEndsMode(n));
    n = nextLoopCount(n, true, true);
    TEST_ASSERT_EQUAL_UINT8(3, n);
    TEST_ASSERT_TRUE(loopEndsMode(n));
    // A normal start in between clears it; so does the mode being off.
    TEST_ASSERT_EQUAL_UINT8(0, nextLoopCount(2, false, true));
    TEST_ASSERT_EQUAL_UINT8(0, nextLoopCount(2, true, false));
    TEST_ASSERT_EQUAL_UINT8(0xFF, nextLoopCount(0xFF, true, true));
    TEST_ASSERT_EQUAL_STRING("It kept restarting", endReason(End::RestartLoop));
    TEST_ASSERT_EQUAL_STRING("restart-loop", endName(End::RestartLoop));
}

void test_clean_stretch_clears_the_loop_count(void) {
    TEST_ASSERT_FALSE(loopCountClears(0, false));
    TEST_ASSERT_FALSE(loopCountClears(kLoopCleanMs - 1, false));
    TEST_ASSERT_TRUE(loopCountClears(kLoopCleanMs, false));
    TEST_ASSERT_TRUE(loopCountClears(1000, true));   // first good check-in
    TEST_ASSERT_EQUAL_UINT32(5u * 60u * 1000u, kLoopCleanMs);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_factory_default_is_off);
    RUN_TEST(test_stored_values_round_trip);
    RUN_TEST(test_legacy_dev_row_migrates);
    RUN_TEST(test_nvs_keys_fit);
    RUN_TEST(test_wire_mode_follows_the_site_contract);
    RUN_TEST(test_awake_modes_keep_the_fidget_awake);
    RUN_TEST(test_dev_mode_listens_except_in_the_bluetooth_restart);
    RUN_TEST(test_bluetooth_app_restart_only_while_listening);
    RUN_TEST(test_off_never_ends);
    RUN_TEST(test_idle_stop_after_30_min_without_use);
    RUN_TEST(test_48h_safety_net_ends_either_stop_setting);
    RUN_TEST(test_battery_floor_ends_both_and_wins);
    RUN_TEST(test_bench_timers_override_the_built_in_ones);
    RUN_TEST(test_battery_low_reading);
    RUN_TEST(test_end_copy_names_why);
    RUN_TEST(test_entering_or_leaving_dev_mode_restarts);
    RUN_TEST(test_screen_choices_in_order);
    RUN_TEST(test_selector_wraps_like_the_menu);
    RUN_TEST(test_copy_names_the_trade_offs);
    RUN_TEST(test_user_copy_has_no_jargon);
    RUN_TEST(test_marker_for_each_awake_mode);
    RUN_TEST(test_breathing_indicator);
    RUN_TEST(test_listening_allow_list);
    RUN_TEST(test_restart_loop_ends_the_mode);
    RUN_TEST(test_clean_stretch_clears_the_loop_count);
    return UNITY_END();
}
