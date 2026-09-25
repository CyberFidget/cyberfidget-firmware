// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

// test/test_core_status/test_status.cpp
//
// Pure status / notification store behind the main-menu status bar:
// posting and clearing, priority order for the bar line, badge state, the
// popup ignore routing (never dropped), the four update states, late and
// cached flags, transient expiry, and last-check-in age buckets feeding the
// WiFi glyph. No display; the bar drawing and the Status screen are not
// compiled here.

#include <unity.h>
#include <stdint.h>
#include <string.h>

#include "../../lib/StatusService/StatusService.h"

static StatusService svc;

void setUp(void) { svc.reset(); }
void tearDown(void) {}

// ---- posting / clearing ----

void test_empty_store_has_no_line_and_no_badge(void) {
    TEST_ASSERT_NULL(svc.current());
    TEST_ASSERT_EQUAL_INT(0, svc.count());
    TEST_ASSERT_FALSE(svc.badge());
}

void test_post_then_read_back(void) {
    TEST_ASSERT_TRUE(svc.post(StatusKind::Info, "Saved", StatusPriority::Normal, true, 100));
    const StatusEntry *e = svc.current();
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_STRING("Saved", e->text);
    TEST_ASSERT_TRUE(e->kind == StatusKind::Info);
    TEST_ASSERT_EQUAL_INT(1, svc.count());
}

void test_info_needs_text(void) {
    TEST_ASSERT_FALSE(svc.post(StatusKind::Info, "", StatusPriority::Normal, true, 0));
    TEST_ASSERT_FALSE(svc.post(StatusKind::Warning, nullptr, StatusPriority::Normal, true, 0));
    TEST_ASSERT_EQUAL_INT(0, svc.count());
}

void test_long_text_is_truncated(void) {
    char longText[100];
    memset(longText, 'x', sizeof(longText) - 1);
    longText[sizeof(longText) - 1] = '\0';
    TEST_ASSERT_TRUE(svc.post(StatusKind::Info, longText, StatusPriority::Normal, true, 0));
    TEST_ASSERT_EQUAL_size_t(StatusEntry::kMaxText - 1, strlen(svc.current()->text));
}

void test_clear_kind_and_clear_all(void) {
    svc.post(StatusKind::Info, "a", StatusPriority::Normal, true, 0);
    svc.post(StatusKind::Info, "b", StatusPriority::Normal, true, 0);
    svc.post(StatusKind::Checking, nullptr, StatusPriority::Normal, true, 0);
    TEST_ASSERT_EQUAL_INT(3, svc.count());
    TEST_ASSERT_EQUAL_INT(2, svc.clear(StatusKind::Info));
    TEST_ASSERT_EQUAL_INT(1, svc.count());
    TEST_ASSERT_TRUE(svc.has(StatusKind::Checking));
    svc.clearAll();
    TEST_ASSERT_EQUAL_INT(0, svc.count());
    TEST_ASSERT_NULL(svc.current());
}

void test_info_lines_from_different_posters_coexist(void) {
    svc.post(StatusKind::Info, "Battery low", StatusPriority::Normal, true, 0);
    svc.post(StatusKind::Info, "Apps synced", StatusPriority::Normal, true, 0);
    svc.post(StatusKind::Info, "Apps synced", StatusPriority::Normal, true, 5);  // refresh, not a copy
    TEST_ASSERT_EQUAL_INT(2, svc.count());
}

// ---- priority / bar line ----

void test_highest_priority_wins_the_bar(void) {
    svc.post(StatusKind::Info, "low", StatusPriority::Low, true, 0);
    svc.post(StatusKind::Warning, "high", StatusPriority::High, true, 1);
    svc.post(StatusKind::Info, "normal", StatusPriority::Normal, true, 2);
    TEST_ASSERT_EQUAL_STRING("high", svc.current()->text);
}

void test_equal_priority_most_recent_wins(void) {
    svc.post(StatusKind::Info, "first", StatusPriority::Normal, true, 0);
    svc.post(StatusKind::Info, "second", StatusPriority::Normal, true, 1);
    TEST_ASSERT_EQUAL_STRING("second", svc.current()->text);
    svc.post(StatusKind::Info, "first", StatusPriority::Normal, true, 2);  // refreshed
    TEST_ASSERT_EQUAL_STRING("first", svc.current()->text);
}

void test_pending_lists_in_bar_order(void) {
    svc.post(StatusKind::Info, "low", StatusPriority::Low, true, 0);
    svc.post(StatusKind::Info, "normal-old", StatusPriority::Normal, true, 1);
    svc.post(StatusKind::Warning, "high", StatusPriority::High, true, 2);
    svc.post(StatusKind::Info, "normal-new", StatusPriority::Normal, true, 3);
    const StatusEntry *list[StatusService::kMaxEntries];
    const int n = svc.pending(list, StatusService::kMaxEntries);
    TEST_ASSERT_EQUAL_INT(4, n);
    TEST_ASSERT_EQUAL_STRING("high", list[0]->text);
    TEST_ASSERT_EQUAL_STRING("normal-new", list[1]->text);
    TEST_ASSERT_EQUAL_STRING("normal-old", list[2]->text);
    TEST_ASSERT_EQUAL_STRING("low", list[3]->text);
    TEST_ASSERT_EQUAL_INT(2, svc.pending(list, 2));   // respects max
}

void test_full_store_evicts_lowest_and_refuses_when_outranked(void) {
    char t[8];
    for (int i = 0; i < StatusService::kMaxEntries; i++) {
        t[0] = (char)('a' + i); t[1] = '\0';
        TEST_ASSERT_TRUE(svc.post(StatusKind::Info, t, StatusPriority::Normal, true, (uint32_t)i));
    }
    // A low-priority line cannot push out normal ones.
    TEST_ASSERT_FALSE(svc.post(StatusKind::Info, "low", StatusPriority::Low, true, 20));
    // A high one evicts the oldest normal ("a").
    TEST_ASSERT_TRUE(svc.post(StatusKind::Warning, "urgent", StatusPriority::High, true, 21));
    TEST_ASSERT_EQUAL_INT(StatusService::kMaxEntries, svc.count());
    const StatusEntry *list[StatusService::kMaxEntries];
    const int n = svc.pending(list, StatusService::kMaxEntries);
    for (int i = 0; i < n; i++) TEST_ASSERT_NOT_EQUAL(0, strcmp("a", list[i]->text));
    TEST_ASSERT_EQUAL_STRING("urgent", svc.current()->text);
}

// ---- sticky / transient ----

void test_transient_entry_expires_sticky_stays(void) {
    svc.post(StatusKind::Info, "brief", StatusPriority::Normal, false, 1000);
    svc.post(StatusKind::Info, "stays", StatusPriority::Normal, true, 1000);
    svc.expire(1000 + StatusService::kTransientMs - 1);
    TEST_ASSERT_EQUAL_INT(2, svc.count());
    svc.expire(1000 + StatusService::kTransientMs);
    TEST_ASSERT_EQUAL_INT(1, svc.count());
    TEST_ASSERT_EQUAL_STRING("stays", svc.current()->text);
}

void test_transient_repost_restarts_its_clock(void) {
    svc.post(StatusKind::Checking, nullptr, StatusPriority::Normal, false, 0);
    svc.post(StatusKind::Checking, nullptr, StatusPriority::Normal, false, 8000);
    svc.expire(12000);
    TEST_ASSERT_TRUE(svc.has(StatusKind::Checking));
    svc.expire(18000);
    TEST_ASSERT_FALSE(svc.has(StatusKind::Checking));
}

void test_transient_expiry_survives_clock_wrap(void) {
    svc.post(StatusKind::Info, "wrap", StatusPriority::Normal, false, 0xFFFFF000u);
    svc.expire(0x00000100u);   // 0x1100 ms later
    TEST_ASSERT_EQUAL_INT(1, svc.count());
    svc.expire(0xFFFFF000u + StatusService::kTransientMs);
    TEST_ASSERT_EQUAL_INT(0, svc.count());
}

// ---- the four update states ----

void test_update_states_use_plain_default_copy(void) {
    TEST_ASSERT_TRUE(svc.post(StatusKind::Checking, nullptr, StatusPriority::Normal, true, 0));
    TEST_ASSERT_EQUAL_STRING("Checking for updates...", svc.current()->text);
    svc.clearAll();
    TEST_ASSERT_TRUE(svc.post(StatusKind::UpdateReady, "", StatusPriority::High, true, 0));
    TEST_ASSERT_EQUAL_STRING("Update ready", svc.current()->text);
    svc.clearAll();
    TEST_ASSERT_TRUE(svc.post(StatusKind::ChangesWaiting, nullptr, StatusPriority::High, true, 0));
    TEST_ASSERT_EQUAL_STRING("App changes waiting", svc.current()->text);
    svc.clearAll();
    TEST_ASSERT_TRUE(svc.post(StatusKind::Listening, nullptr, StatusPriority::Normal, true, 0));
    TEST_ASSERT_EQUAL_STRING("Dev mode", svc.current()->text);
}

void test_update_states_hold_one_entry_each(void) {
    svc.post(StatusKind::UpdateReady, "Update 1.4.0 ready", StatusPriority::High, true, 0);
    svc.post(StatusKind::UpdateReady, "Update 1.5.0 ready", StatusPriority::High, true, 1);
    TEST_ASSERT_EQUAL_INT(1, svc.count());
    TEST_ASSERT_EQUAL_STRING("Update 1.5.0 ready", svc.current()->text);
    svc.post(StatusKind::Checking, nullptr, StatusPriority::Normal, true, 2);
    svc.post(StatusKind::ChangesWaiting, nullptr, StatusPriority::High, true, 3);
    svc.post(StatusKind::Listening, nullptr, StatusPriority::Normal, true, 4);
    TEST_ASSERT_EQUAL_INT(4, svc.count());
    TEST_ASSERT_TRUE(svc.has(StatusKind::Checking));
    TEST_ASSERT_TRUE(svc.has(StatusKind::UpdateReady));
    TEST_ASSERT_TRUE(svc.has(StatusKind::ChangesWaiting));
    TEST_ASSERT_TRUE(svc.has(StatusKind::Listening));
}

void test_default_priorities(void) {
    TEST_ASSERT_EQUAL_UINT8(StatusPriority::Normal, StatusService::defaultPriority(StatusKind::Checking));
    TEST_ASSERT_EQUAL_UINT8(StatusPriority::High,   StatusService::defaultPriority(StatusKind::UpdateReady));
    TEST_ASSERT_EQUAL_UINT8(StatusPriority::High,   StatusService::defaultPriority(StatusKind::ChangesWaiting));
    TEST_ASSERT_EQUAL_UINT8(StatusPriority::Normal, StatusService::defaultPriority(StatusKind::Listening));
}

void test_kind_names_round_trip(void) {
    for (uint8_t k = 0; k < (uint8_t)StatusKind::Count; k++) {
        StatusKind back = StatusKind::Count;
        TEST_ASSERT_TRUE(StatusService::kindFromName(StatusService::kindName((StatusKind)k), &back));
        TEST_ASSERT_EQUAL_UINT8(k, (uint8_t)back);
    }
    StatusKind out;
    TEST_ASSERT_FALSE(StatusService::kindFromName("bogus", &out));
    TEST_ASSERT_FALSE(StatusService::kindFromName(nullptr, &out));
}

// ---- late / cached flags ----

void test_late_and_cached_flags_are_preserved(void) {
    svc.post(StatusKind::UpdateReady, nullptr, StatusPriority::High, true, 0, StatusFlag::Late);
    TEST_ASSERT_TRUE(svc.current()->late());
    TEST_ASSERT_FALSE(svc.current()->cached());
    svc.post(StatusKind::ChangesWaiting, nullptr, StatusPriority::High, true, 1, StatusFlag::Cached);
    TEST_ASSERT_TRUE(svc.current()->cached());
    TEST_ASSERT_FALSE(svc.current()->late());
    svc.post(StatusKind::Checking, nullptr, StatusPriority::High, true, 2,
             StatusFlag::Late | StatusFlag::Cached);
    TEST_ASSERT_TRUE(svc.current()->late());
    TEST_ASSERT_TRUE(svc.current()->cached());
}

void test_ignored_late_result_keeps_its_flag(void) {
    svc.resolvePopup(StatusKind::UpdateReady, nullptr, false, 0, StatusFlag::Late | StatusFlag::Cached);
    const StatusEntry *e = svc.current();
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_TRUE(e->late());
    TEST_ASSERT_TRUE(e->cached());
}

// ---- badge ----

void test_high_priority_sets_badge_normal_does_not(void) {
    svc.post(StatusKind::Checking, nullptr, StatusPriority::Normal, true, 0);
    TEST_ASSERT_FALSE(svc.badge());
    svc.post(StatusKind::UpdateReady, nullptr, StatusPriority::High, true, 1);
    TEST_ASSERT_TRUE(svc.badge());
}

void test_mark_seen_clears_badge_keeps_entries(void) {
    svc.post(StatusKind::UpdateReady, nullptr, StatusPriority::High, true, 0);
    svc.markSeen();
    TEST_ASSERT_FALSE(svc.badge());
    TEST_ASSERT_EQUAL_INT(1, svc.count());
}

void test_identical_repost_stays_seen_changed_text_asks_again(void) {
    svc.post(StatusKind::UpdateReady, "Update 1.4.0 ready", StatusPriority::High, true, 0);
    svc.markSeen();
    svc.post(StatusKind::UpdateReady, "Update 1.4.0 ready", StatusPriority::High, true, 1);
    TEST_ASSERT_FALSE(svc.badge());
    svc.post(StatusKind::UpdateReady, "Update 1.5.0 ready", StatusPriority::High, true, 2);
    TEST_ASSERT_TRUE(svc.badge());
}

void test_clearing_the_attention_entry_drops_the_badge(void) {
    svc.post(StatusKind::ChangesWaiting, nullptr, StatusPriority::High, true, 0);
    svc.post(StatusKind::Info, "fyi", StatusPriority::Normal, true, 0);
    TEST_ASSERT_TRUE(svc.badge());
    svc.clear(StatusKind::ChangesWaiting);
    TEST_ASSERT_FALSE(svc.badge());
}

// ---- popup routing ----

void test_ignored_popup_lands_in_bar_with_badge(void) {
    svc.resolvePopup(StatusKind::UpdateReady, "Update 1.4.0 ready", false, 50);
    const StatusEntry *e = svc.current();
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_STRING("Update 1.4.0 ready", e->text);
    TEST_ASSERT_TRUE(e->sticky);
    TEST_ASSERT_TRUE(e->attention);
    TEST_ASSERT_TRUE(svc.badge());
}

void test_ignored_popup_is_never_dropped_by_expiry(void) {
    svc.resolvePopup(StatusKind::ChangesWaiting, nullptr, false, 0);
    svc.expire(10u * StatusService::kTransientMs);
    TEST_ASSERT_TRUE(svc.has(StatusKind::ChangesWaiting));
    TEST_ASSERT_TRUE(svc.badge());
}

void test_ignored_normal_kind_is_raised_to_attention(void) {
    // Kinds that do not normally badge still badge once their popup is ignored.
    svc.resolvePopup(StatusKind::Info, "Battery low", false, 0);
    TEST_ASSERT_TRUE(svc.badge());
    TEST_ASSERT_EQUAL_UINT8(StatusPriority::High, svc.current()->priority);
}

void test_ignore_after_seen_asks_again(void) {
    svc.post(StatusKind::UpdateReady, nullptr, StatusPriority::High, true, 0);
    svc.markSeen();
    svc.resolvePopup(StatusKind::UpdateReady, nullptr, false, 1);
    TEST_ASSERT_TRUE(svc.badge());
}

void test_accepted_popup_clears_its_entry(void) {
    svc.post(StatusKind::UpdateReady, nullptr, StatusPriority::High, true, 0);
    svc.post(StatusKind::Info, "keep", StatusPriority::Normal, true, 0);
    svc.resolvePopup(StatusKind::UpdateReady, nullptr, true, 1);
    TEST_ASSERT_FALSE(svc.has(StatusKind::UpdateReady));
    TEST_ASSERT_FALSE(svc.badge());
    TEST_ASSERT_EQUAL_STRING("keep", svc.current()->text);
}

void test_accepted_info_popup_clears_only_matching_line(void) {
    svc.post(StatusKind::Info, "one", StatusPriority::Normal, true, 0);
    svc.post(StatusKind::Info, "two", StatusPriority::Normal, true, 0);
    svc.resolvePopup(StatusKind::Info, "one", true, 1);
    TEST_ASSERT_EQUAL_INT(1, svc.count());
    TEST_ASSERT_EQUAL_STRING("two", svc.current()->text);
}

// ---- last check-in age -> glyph ----

void test_no_check_in_is_never(void) {
    TEST_ASSERT_TRUE(svc.ageBucket(1000) == CheckInAge::Never);
    TEST_ASSERT_TRUE(svc.glyph(1000) == StatusGlyph::Never);
    char buf[8];
    svc.ageLabel(1000, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("--", buf);
}

void test_age_buckets_at_their_edges(void) {
    const uint32_t at = 5000;
    svc.setCheckIn(at, false);
    TEST_ASSERT_TRUE(svc.ageBucket(at) == CheckInAge::Hour);
    TEST_ASSERT_TRUE(svc.ageBucket(at + StatusService::kHourSec - 1) == CheckInAge::Hour);
    TEST_ASSERT_TRUE(svc.ageBucket(at + StatusService::kHourSec) == CheckInAge::Today);
    TEST_ASSERT_TRUE(svc.ageBucket(at + StatusService::kDaySec - 1) == CheckInAge::Today);
    TEST_ASSERT_TRUE(svc.ageBucket(at + StatusService::kDaySec) == CheckInAge::Days);
    TEST_ASSERT_TRUE(svc.ageBucket(at + 30u * StatusService::kDaySec) == CheckInAge::Days);
}

void test_clock_before_check_in_reads_as_fresh(void) {
    svc.setCheckIn(10000, false);
    TEST_ASSERT_EQUAL_UINT32(0, svc.checkInAgeSec(5000));
    TEST_ASSERT_TRUE(svc.ageBucket(5000) == CheckInAge::Hour);
}

void test_glyph_follows_age_not_connection(void) {
    const uint32_t at = 100;
    svc.setCheckIn(at, false);
    TEST_ASSERT_TRUE(svc.glyph(at + 60) == StatusGlyph::Hour);
    TEST_ASSERT_TRUE(svc.glyph(at + 5 * StatusService::kHourSec) == StatusGlyph::Today);
    TEST_ASSERT_TRUE(svc.glyph(at + 3 * StatusService::kDaySec) == StatusGlyph::Days);
    // Posting Checking does not make the glyph live: only Listening does.
    svc.post(StatusKind::Checking, nullptr, StatusPriority::Normal, true, 0);
    TEST_ASSERT_TRUE(svc.glyph(at + 3 * StatusService::kDaySec) == StatusGlyph::Days);
}

void test_listening_is_the_only_live_glyph(void) {
    TEST_ASSERT_TRUE(svc.glyph(0) == StatusGlyph::Never);
    svc.post(StatusKind::Listening, nullptr, StatusPriority::Normal, true, 0);
    TEST_ASSERT_TRUE(svc.glyph(0) == StatusGlyph::Live);
    char buf[8];
    svc.ageLabel(0, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("", buf);
    svc.clear(StatusKind::Listening);
    svc.setCheckIn(0, false);
    TEST_ASSERT_TRUE(svc.glyph(2 * StatusService::kDaySec) == StatusGlyph::Days);
}

void test_age_labels(void) {
    char buf[8];
    svc.setCheckIn(0, false);
    svc.ageLabel(59 * 60, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("<1h", buf);
    svc.ageLabel(StatusService::kHourSec, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("1h", buf);
    svc.ageLabel(23 * StatusService::kHourSec + 3599, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("23h", buf);
    svc.ageLabel(StatusService::kDaySec, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("1d", buf);
    svc.ageLabel(12 * StatusService::kDaySec, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("12d", buf);
    svc.ageLabel(12 * StatusService::kDaySec, buf, 3);   // truncates safely
    TEST_ASSERT_EQUAL_STRING("1d", buf);
}

void test_cached_check_in_is_reported(void) {
    svc.setCheckIn(100, true);
    TEST_ASSERT_TRUE(svc.hasCheckIn());
    TEST_ASSERT_TRUE(svc.checkInCached());
    svc.setCheckIn(200, false);
    TEST_ASSERT_FALSE(svc.checkInCached());
    svc.clearCheckIn();
    TEST_ASSERT_FALSE(svc.hasCheckIn());
    TEST_ASSERT_TRUE(svc.glyph(300) == StatusGlyph::Never);
}

void test_glyph_names(void) {
    TEST_ASSERT_EQUAL_STRING("never", StatusService::glyphName(StatusGlyph::Never));
    TEST_ASSERT_EQUAL_STRING("hour",  StatusService::glyphName(StatusGlyph::Hour));
    TEST_ASSERT_EQUAL_STRING("today", StatusService::glyphName(StatusGlyph::Today));
    TEST_ASSERT_EQUAL_STRING("days",  StatusService::glyphName(StatusGlyph::Days));
    TEST_ASSERT_EQUAL_STRING("live",  StatusService::glyphName(StatusGlyph::Live));
}

// ---- refresh never weakens ----

void test_ignored_popup_survives_transient_repost(void) {
    svc.resolvePopup(StatusKind::UpdateReady, nullptr, false, 0);
    // The checker re-posts the same state as a transient, lower-priority line.
    svc.post(StatusKind::UpdateReady, nullptr, StatusPriority::Normal, false, 100);
    const StatusEntry *e = svc.current();
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_TRUE(e->sticky);
    TEST_ASSERT_TRUE(e->attention);
    TEST_ASSERT_EQUAL_UINT8(StatusPriority::High, e->priority);
    svc.expire(100 + 10u * StatusService::kTransientMs);
    TEST_ASSERT_TRUE(svc.has(StatusKind::UpdateReady));
    TEST_ASSERT_TRUE(svc.badge());
}

void test_refresh_keeps_sticky_and_higher_priority(void) {
    svc.post(StatusKind::Info, "note", StatusPriority::High, true, 0);
    svc.markSeen();
    svc.post(StatusKind::Info, "note", StatusPriority::Low, false, 1);
    const StatusEntry *e = svc.current();
    TEST_ASSERT_TRUE(e->sticky);
    TEST_ASSERT_EQUAL_UINT8(StatusPriority::High, e->priority);
    TEST_ASSERT_FALSE(e->attention);   // seen stays seen
    // A stronger refresh does strengthen a weak entry.
    svc.post(StatusKind::Warning, "w", StatusPriority::Low, false, 2);
    svc.post(StatusKind::Warning, "w", StatusPriority::Normal, true, 3);
    const StatusEntry *list[StatusService::kMaxEntries];
    const int n = svc.pending(list, StatusService::kMaxEntries);
    for (int i = 0; i < n; i++) {
        if (list[i]->kind == StatusKind::Warning) {
            TEST_ASSERT_TRUE(list[i]->sticky);
            TEST_ASSERT_EQUAL_UINT8(StatusPriority::Normal, list[i]->priority);
        }
    }
}

void test_refresh_with_new_text_keeps_attention(void) {
    svc.resolvePopup(StatusKind::ChangesWaiting, "2 app changes waiting", false, 0);
    svc.post(StatusKind::ChangesWaiting, "3 app changes waiting", StatusPriority::Normal, false, 1);
    TEST_ASSERT_TRUE(svc.badge());
    TEST_ASSERT_TRUE(svc.current()->sticky);
    TEST_ASSERT_EQUAL_STRING("3 app changes waiting", svc.current()->text);
    svc.clear(StatusKind::ChangesWaiting);   // only clear/accept weakens
    TEST_ASSERT_FALSE(svc.badge());
}

// ---- Awake & dev mode ----

void test_clear_entry_removes_only_that_line(void) {
    svc.post(StatusKind::Warning, "Dev mode: not connected", StatusPriority::Normal, true, 0);
    svc.post(StatusKind::Warning, "Battery low", StatusPriority::High, true, 1);
    TEST_ASSERT_TRUE(svc.clearEntry(StatusKind::Warning, "Dev mode: not connected"));
    TEST_ASSERT_EQUAL_INT(1, svc.count());
    TEST_ASSERT_EQUAL_STRING("Battery low", svc.current()->text);
    TEST_ASSERT_FALSE(svc.clearEntry(StatusKind::Warning, "Dev mode: not connected"));
    // An empty text names the kind's default line.
    svc.post(StatusKind::Listening, nullptr, StatusPriority::Normal, true, 2);
    TEST_ASSERT_TRUE(svc.clearEntry(StatusKind::Listening, nullptr));
    TEST_ASSERT_FALSE(svc.has(StatusKind::Listening));
}

void test_awake_marker_and_bluetooth_mark(void) {
    TEST_ASSERT_EQUAL_INT((int)AwakeMarker::None, (int)svc.awakeMarker());
    TEST_ASSERT_FALSE(svc.bluetoothNeedsRestart());
    svc.setAwakeMarker(AwakeMarker::Listening);
    svc.setBluetoothNeedsRestart(true);
    TEST_ASSERT_EQUAL_INT((int)AwakeMarker::Listening, (int)svc.awakeMarker());
    TEST_ASSERT_TRUE(svc.bluetoothNeedsRestart());
    // The marker is not a notification: no entry, no badge.
    TEST_ASSERT_EQUAL_INT(0, svc.count());
    TEST_ASSERT_FALSE(svc.badge());
    svc.reset();
    TEST_ASSERT_EQUAL_INT((int)AwakeMarker::None, (int)svc.awakeMarker());
    TEST_ASSERT_FALSE(svc.bluetoothNeedsRestart());
}

int main(int /*argc*/, char** /*argv*/) {
    UNITY_BEGIN();
    RUN_TEST(test_empty_store_has_no_line_and_no_badge);
    RUN_TEST(test_post_then_read_back);
    RUN_TEST(test_info_needs_text);
    RUN_TEST(test_long_text_is_truncated);
    RUN_TEST(test_clear_kind_and_clear_all);
    RUN_TEST(test_info_lines_from_different_posters_coexist);
    RUN_TEST(test_highest_priority_wins_the_bar);
    RUN_TEST(test_equal_priority_most_recent_wins);
    RUN_TEST(test_pending_lists_in_bar_order);
    RUN_TEST(test_full_store_evicts_lowest_and_refuses_when_outranked);
    RUN_TEST(test_transient_entry_expires_sticky_stays);
    RUN_TEST(test_transient_repost_restarts_its_clock);
    RUN_TEST(test_transient_expiry_survives_clock_wrap);
    RUN_TEST(test_update_states_use_plain_default_copy);
    RUN_TEST(test_update_states_hold_one_entry_each);
    RUN_TEST(test_default_priorities);
    RUN_TEST(test_kind_names_round_trip);
    RUN_TEST(test_late_and_cached_flags_are_preserved);
    RUN_TEST(test_ignored_late_result_keeps_its_flag);
    RUN_TEST(test_high_priority_sets_badge_normal_does_not);
    RUN_TEST(test_mark_seen_clears_badge_keeps_entries);
    RUN_TEST(test_identical_repost_stays_seen_changed_text_asks_again);
    RUN_TEST(test_clearing_the_attention_entry_drops_the_badge);
    RUN_TEST(test_ignored_popup_lands_in_bar_with_badge);
    RUN_TEST(test_ignored_popup_is_never_dropped_by_expiry);
    RUN_TEST(test_ignored_normal_kind_is_raised_to_attention);
    RUN_TEST(test_ignore_after_seen_asks_again);
    RUN_TEST(test_accepted_popup_clears_its_entry);
    RUN_TEST(test_accepted_info_popup_clears_only_matching_line);
    RUN_TEST(test_no_check_in_is_never);
    RUN_TEST(test_age_buckets_at_their_edges);
    RUN_TEST(test_clock_before_check_in_reads_as_fresh);
    RUN_TEST(test_glyph_follows_age_not_connection);
    RUN_TEST(test_listening_is_the_only_live_glyph);
    RUN_TEST(test_age_labels);
    RUN_TEST(test_cached_check_in_is_reported);
    RUN_TEST(test_glyph_names);
    RUN_TEST(test_ignored_popup_survives_transient_repost);
    RUN_TEST(test_refresh_keeps_sticky_and_higher_priority);
    RUN_TEST(test_refresh_with_new_text_keeps_attention);
    RUN_TEST(test_clear_entry_removes_only_that_line);
    RUN_TEST(test_awake_marker_and_bluetooth_mark);
    return UNITY_END();
}
