// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#include <unity.h>
#include <string.h>
#include "PromptPolicy.h"

using namespace CloudSync;
using namespace PromptPolicy;

void setUp(void) {}
void tearDown(void) {}

static void checkBounds(const CheckLines& lines) {
    TEST_ASSERT_TRUE(lines.count >= 1 && lines.count <= 3);
    const char* banned[] = {"WASM", "ESP32", "OTA", "TLS", "JSON", "HTTP", "manifest"};
    for (int i = 0; i < 3; ++i) {
        TEST_ASSERT_NOT_NULL(memchr(lines.lines[i], '\0', kCheckLineLen));
        TEST_ASSERT_TRUE(strlen(lines.lines[i]) <= 21);
        if (i >= lines.count) TEST_ASSERT_EQUAL_STRING("", lines.lines[i]);
        for (const char* word : banned) TEST_ASSERT_NULL(strstr(lines.lines[i], word));
    }
}

void test_every_phase(void) {
    const SessionPhase phases[] = {SessionPhase::Idle, SessionPhase::JoiningWifi,
        SessionPhase::CheckingIn, SessionPhase::LookingForUpdate, SessionPhase::GettingApps,
        SessionPhase::Waiting, SessionPhase::Done, SessionPhase::Failed};
    const char* expected[] = {"Ready to check", "Joining WiFi", "Checking in",
        "Looking for updates", "Getting apps", "Waiting 0 s", "Check complete", "Could not check"};
    SessionSnapshot status;
    CheckLines lines;
    for (unsigned i = 0; i < sizeof(phases) / sizeof(phases[0]); ++i) {
        status.phase = phases[i];
        formatSessionStatus(status, lines);
        TEST_ASSERT_EQUAL_STRING(expected[i], lines.lines[0]);
        checkBounds(lines);
    }
}

void test_join_details_and_name_truncation(void) {
    SessionSnapshot status;
    status.phase = SessionPhase::JoiningWifi;
    status.current = 2;
    status.total = 2;
    CheckLines lines;
    formatSessionStatus(status, lines);
    TEST_ASSERT_EQUAL_INT(2, lines.count);
    TEST_ASSERT_EQUAL_STRING("Try 2 of 2", lines.lines[1]);
    formatSessionStatus(status, lines, "12345678901234567890123456789012");
    TEST_ASSERT_EQUAL_INT(3, lines.count);
    TEST_ASSERT_EQUAL_STRING("123456789012345678901", lines.lines[1]);
    TEST_ASSERT_EQUAL_STRING("Try 2 of 2", lines.lines[2]);
    checkBounds(lines);
    formatSessionStatus(status, lines, "");
    TEST_ASSERT_EQUAL_INT(2, lines.count);
    status.total = 0;
    formatSessionStatus(status, lines, "MyNetwork");
    TEST_ASSERT_EQUAL_INT(2, lines.count);
    TEST_ASSERT_EQUAL_STRING("MyNetwork", lines.lines[1]);
    checkBounds(lines);
}

void test_app_details_and_wait(void) {
    SessionSnapshot status;
    status.phase = SessionPhase::GettingApps;
    status.current = 2;
    status.total = 5;
    CheckLines lines;
    formatSessionStatus(status, lines);
    TEST_ASSERT_EQUAL_STRING("Getting apps 2 of 5", lines.lines[0]);
    checkBounds(lines);
    status.phase = SessionPhase::Waiting;
    status.secondsLeft = 41;
    formatSessionStatus(status, lines);
    TEST_ASSERT_EQUAL_INT(2, lines.count);
    TEST_ASSERT_EQUAL_STRING("Waiting 41 s", lines.lines[0]);
    TEST_ASSERT_EQUAL_STRING("(server asks us to)", lines.lines[1]);
    checkBounds(lines);
    status.serverWait = false;
    formatSessionStatus(status, lines);
    TEST_ASSERT_EQUAL_STRING("Next check soon", lines.lines[1]);
    checkBounds(lines);
}

void test_large_details_are_bounded_and_output_is_cleared(void) {
    SessionSnapshot status;
    status.current = UINT32_MAX;
    status.total = UINT32_MAX;
    status.secondsLeft = UINT32_MAX;
    CheckLines lines;
    status.phase = SessionPhase::JoiningWifi;
    formatSessionStatus(status, lines, "1234567890123456789012345");
    TEST_ASSERT_EQUAL_STRING("Try 4294967295 of 429", lines.lines[2]);
    checkBounds(lines);
    status.phase = SessionPhase::GettingApps;
    formatSessionStatus(status, lines);
    TEST_ASSERT_EQUAL_STRING("Getting apps 42949672", lines.lines[0]);
    checkBounds(lines);
    status.phase = SessionPhase::Waiting;
    formatSessionStatus(status, lines);
    TEST_ASSERT_EQUAL_STRING("Waiting 4294967295 s", lines.lines[0]);
    checkBounds(lines);
    status.phase = SessionPhase::CheckingIn;
    formatSessionStatus(status, lines);
    TEST_ASSERT_EQUAL_INT(1, lines.count);
    checkBounds(lines);
}

void test_done_results(void) {
    const SessionOutcome outcomes[] = {SessionOutcome::Complete, SessionOutcome::NoChanges,
        SessionOutcome::AppsApplied, SessionOutcome::AppsWaiting, SessionOutcome::UpdateFound};
    const char* expected[] = {"Check complete", "No changes found", "App changes applied",
        "App changes waiting", "Update available"};
    SessionSnapshot status;
    status.phase = SessionPhase::Done;
    CheckLines lines;
    for (unsigned i = 0; i < sizeof(outcomes) / sizeof(outcomes[0]); ++i) {
        status.outcome = outcomes[i];
        formatSessionStatus(status, lines);
        TEST_ASSERT_EQUAL_STRING(expected[i], lines.lines[0]);
        checkBounds(lines);
    }
}

void test_failure_reasons(void) {
    const char* errors[] = {"join", "no-network", "sta-mode", "no-wifi", "deadline",
        "not-linked", "cancelled", "rate-limited", "backoff", "checkin-transport",
        "ack-transport", "offer-transport", "blob-transport", "roots-parse", "checkin-body",
        "ack-http", "offer-body", "blob-write", "blob-open", "blob-commit",
        "rejected:blob-sha", "task-create", "unknown"};
    const char* expected[] = {"Couldn't join WiFi", "Couldn't join WiFi", "Couldn't join WiFi",
        "No saved WiFi yet", "Check timed out", "Not linked yet", "Check stopped",
        "Try again later", "Try again later", "No answer from server", "No answer from server",
        "No answer from server", "No answer from server", "No answer from server",
        "Couldn't read reply", "Couldn't read reply", "Couldn't read reply", "Couldn't get apps",
        "Couldn't get apps", "Couldn't get apps", "Couldn't get apps", "Could not check", "Could not check"};
    SessionSnapshot status;
    status.phase = SessionPhase::Failed;
    CheckLines lines;
    for (unsigned i = 0; i < sizeof(errors) / sizeof(errors[0]); ++i) {
        strcpy(status.error, errors[i]);
        TEST_ASSERT_EQUAL_STRING(expected[i], sessionFailureCopy(errors[i]));
        formatSessionStatus(status, lines);
        TEST_ASSERT_EQUAL_STRING(expected[i], lines.lines[0]);
        checkBounds(lines);
    }
    TEST_ASSERT_EQUAL_STRING("Could not check", sessionFailureCopy(nullptr));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_every_phase);
    RUN_TEST(test_join_details_and_name_truncation);
    RUN_TEST(test_app_details_and_wait);
    RUN_TEST(test_large_details_are_bounded_and_output_is_cleared);
    RUN_TEST(test_done_results);
    RUN_TEST(test_failure_reasons);
    return UNITY_END();
}
