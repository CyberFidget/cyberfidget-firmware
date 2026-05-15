// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

// test/test_version/test_version.cpp
//
// Placeholder smoke test for the core test environment. The bash layer
// `scripts/test_acceptance.sh` covers most version-related assertions
// today; this file exists so the `pio test -e test_core` env has at
// least one passing test to verify the toolchain works end-to-end.
//
// As more core/infra concerns migrate from bash to Unity, they land in
// sibling `test/test_<thing>/` directories.

#include <unity.h>

// Trivial smoke: the test runner itself works.
void test_unity_runner_alive(void) {
    TEST_ASSERT_TRUE(true);
    TEST_ASSERT_EQUAL_INT(42, 42);
}

void setUp(void)    {}
void tearDown(void) {}

int main(int /*argc*/, char** /*argv*/) {
    UNITY_BEGIN();
    RUN_TEST(test_unity_runner_alive);
    return UNITY_END();
}
