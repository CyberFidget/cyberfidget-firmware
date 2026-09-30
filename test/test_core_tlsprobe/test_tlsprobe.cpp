// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#include <unity.h>

#include "../../lib/SerialCli/TlsProbeSession.h"

void setUp(void) {}
void tearDown(void) {}

void test_single_start_and_completion_handoff(void) {
    TlsProbeSession session;
    TlsProbeSession::State outcome = TlsProbeSession::State::Idle;
    TEST_ASSERT_EQUAL_INT((int)TlsProbeSession::State::Idle, (int)session.current());
    TEST_ASSERT_TRUE(session.start());
    TEST_ASSERT_FALSE(session.start());
    TEST_ASSERT_FALSE(session.consume(outcome));
    TEST_ASSERT_TRUE(session.finish(TlsProbeSession::State::Done));
    TEST_ASSERT_FALSE(session.start());
    TEST_ASSERT_TRUE(session.consume(outcome));
    TEST_ASSERT_EQUAL_INT((int)TlsProbeSession::State::Done, (int)outcome);
    TEST_ASSERT_FALSE(session.consume(outcome));
    TEST_ASSERT_TRUE(session.start());
}

void test_timeout_handoff(void) {
    TlsProbeSession session;
    TlsProbeSession::State outcome = TlsProbeSession::State::Idle;
    TEST_ASSERT_TRUE(session.start());
    TEST_ASSERT_TRUE(session.finish(TlsProbeSession::State::Timeout));
    TEST_ASSERT_TRUE(session.consume(outcome));
    TEST_ASSERT_EQUAL_INT((int)TlsProbeSession::State::Timeout, (int)outcome);
}

void test_failure_handoff(void) {
    TlsProbeSession session;
    TlsProbeSession::State outcome = TlsProbeSession::State::Idle;
    TEST_ASSERT_TRUE(session.start());
    TEST_ASSERT_FALSE(session.finish(TlsProbeSession::State::Idle));
    TEST_ASSERT_TRUE(session.finish(TlsProbeSession::State::Failed));
    TEST_ASSERT_FALSE(session.finish(TlsProbeSession::State::Done));
    TEST_ASSERT_TRUE(session.consume(outcome));
    TEST_ASSERT_EQUAL_INT((int)TlsProbeSession::State::Failed, (int)outcome);
}

int main(int /*argc*/, char** /*argv*/) {
    UNITY_BEGIN();
    RUN_TEST(test_single_start_and_completion_handoff);
    RUN_TEST(test_timeout_handoff);
    RUN_TEST(test_failure_handoff);
    return UNITY_END();
}
