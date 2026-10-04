// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#include <unity.h>
#include <string.h>
#include "PromptPolicy.h"

using namespace PromptPolicy;

void setUp(void) {}
void tearDown(void) {}

void test_dirty_build(void) {
    AboutLines lines;
    formatAboutLines("1.4.2+a1b2c3d-dirty", "dev", "2026-10-03", &lines);
    TEST_ASSERT_EQUAL_INT(4, lines.count);
    TEST_ASSERT_EQUAL_STRING("Version 1.4.2", lines.lines[0]);
    TEST_ASSERT_EQUAL_STRING("Build a1b2c3d-dirty", lines.lines[1]);
    TEST_ASSERT_EQUAL_STRING("Type dev", lines.lines[2]);
    TEST_ASSERT_EQUAL_STRING("Built 2026-10-03", lines.lines[3]);
    char status[kAboutLineLen];
    formatStatusVersion("1.4.2+a1b2c3d-dirty", "dev", status, sizeof(status));
    TEST_ASSERT_EQUAL_STRING("fw 1.4.2 dev", status);
}

void test_release_without_build(void) {
    AboutLines lines;
    formatAboutLines("1.4.2", "release", "2026-10-03", &lines);
    TEST_ASSERT_EQUAL_INT(3, lines.count);
    TEST_ASSERT_EQUAL_STRING("Version 1.4.2", lines.lines[0]);
    TEST_ASSERT_EQUAL_STRING("Type release", lines.lines[1]);
    TEST_ASSERT_EQUAL_STRING("Built 2026-10-03", lines.lines[2]);
    char status[kAboutLineLen];
    formatStatusVersion("1.4.2", "release", status, sizeof(status));
    TEST_ASSERT_EQUAL_STRING("fw 1.4.2", status);
}

void test_long_strings_are_bounded(void) {
    char version[61];
    memset(version, 'a', 60);
    version[60] = '\0';
    struct Guarded {
        unsigned char before;
        AboutLines lines;
        unsigned char after;
    } guarded = {};
    guarded.before = 0xA5;
    guarded.after = 0x5A;
    formatAboutLines(version, version, version, &guarded.lines);
    TEST_ASSERT_EQUAL_HEX8(0xA5, guarded.before);
    TEST_ASSERT_EQUAL_HEX8(0x5A, guarded.after);
    TEST_ASSERT_EQUAL_STRING("Version aaaaaaaaaaaaa", guarded.lines.lines[0]);
    for (int i = 0; i < guarded.lines.count; i++) {
        TEST_ASSERT_EQUAL_UINT32(kAboutLineLen - 1, strlen(guarded.lines.lines[i]));
        TEST_ASSERT_EQUAL_CHAR('\0', guarded.lines.lines[i][kAboutLineLen - 1]);
    }
    char fullVersion[67];
    memcpy(fullVersion, "1.4.2+", 6);
    memcpy(fullVersion + 6, version, sizeof(version));
    formatAboutLines(fullVersion, "dev", "date", &guarded.lines);
    TEST_ASSERT_EQUAL_INT(4, guarded.lines.count);
    TEST_ASSERT_EQUAL_STRING("Version 1.4.2", guarded.lines.lines[0]);
    TEST_ASSERT_EQUAL_STRING("Build aaaaaaaaaaaaaaa", guarded.lines.lines[1]);
    TEST_ASSERT_EQUAL_HEX8(0xA5, guarded.before);
    TEST_ASSERT_EQUAL_HEX8(0x5A, guarded.after);
    struct StatusGuard {
        char line[9];
        unsigned char after;
    } status = {};
    status.after = 0x5A;
    formatStatusVersion(version, "user-build", status.line, sizeof(status.line));
    TEST_ASSERT_EQUAL_STRING("fw aaaaa", status.line);
    TEST_ASSERT_EQUAL_HEX8(0x5A, status.after);
    char one[1] = {'x'};
    formatStatusVersion(version, "dev", one, sizeof(one));
    TEST_ASSERT_EQUAL_CHAR('\0', one[0]);
    one[0] = 'x';
    formatStatusVersion(version, "dev", one, 0);
    TEST_ASSERT_EQUAL_CHAR('x', one[0]);
}

void test_user_build_status(void) {
    char status[kAboutLineLen];
    formatStatusVersion("1.4.2+a1b2c3d", "user-build", status, sizeof(status));
    TEST_ASSERT_EQUAL_STRING("fw 1.4.2 user-build", status);
}

void test_empty_build_and_reuse(void) {
    AboutLines lines;
    formatAboutLines("1.4.2+abc", "dev", "date", &lines);
    formatAboutLines("1.4.2+", "release", "date", &lines);
    TEST_ASSERT_EQUAL_INT(3, lines.count);
    TEST_ASSERT_EQUAL_STRING("Version 1.4.2", lines.lines[0]);
    TEST_ASSERT_EQUAL_STRING("Type release", lines.lines[1]);
    TEST_ASSERT_EQUAL_STRING("", lines.lines[3]);
}

int main(int /*argc*/, char** /*argv*/) {
    UNITY_BEGIN();
    RUN_TEST(test_dirty_build);
    RUN_TEST(test_release_without_build);
    RUN_TEST(test_long_strings_are_bounded);
    RUN_TEST(test_user_build_status);
    RUN_TEST(test_empty_build_and_reuse);
    return UNITY_END();
}
