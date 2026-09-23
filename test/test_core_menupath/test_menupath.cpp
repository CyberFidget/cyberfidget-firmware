// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

// test/test_core_menupath/test_menupath.cpp
//
// splitCategoryPath (lib/MenuManager/CategoryPath.h) is what
// MenuManager::parseCategoryPath uses to turn an app's category path into
// menu levels. It replaced a std::stringstream + std::getline split so the
// device image no longer links iostream. These tests pin that the result is
// identical to the old split: fixed cases, plus every string up to length 7
// over {'a', 'b', '/'} compared against the old implementation (the host
// build can use <sstream>; the device build cannot afford it).

#include <unity.h>
#include <sstream>
#include <string>
#include <vector>

#include "../../lib/MenuManager/CategoryPath.h"

void setUp(void) {}
void tearDown(void) {}

// The previous MenuManager::parseCategoryPath body, verbatim.
static std::vector<std::string> oldParseCategoryPath(const std::string &path)
{
    std::vector<std::string> result;
    if (path.empty()) {
        return result; // no subcategories
    }
    std::stringstream ss(path);
    std::string segment;
    while (std::getline(ss, segment, '/')) {
        if (!segment.empty()) {
            result.push_back(segment);
        }
    }
    return result;
}

static void assertSegments(const std::vector<std::string> &got,
                           const std::vector<std::string> &want)
{
    TEST_ASSERT_EQUAL_UINT32(want.size(), got.size());
    for (size_t i = 0; i < want.size(); ++i) {
        TEST_ASSERT_EQUAL_STRING(want[i].c_str(), got[i].c_str());
    }
}

void test_empty_path_has_no_segments(void) {
    assertSegments(splitCategoryPath(""), {});
}

void test_single_segment(void) {
    assertSegments(splitCategoryPath("Games"), {"Games"});
}

void test_two_segments(void) {
    assertSegments(splitCategoryPath("Tools/WiFi"), {"Tools", "WiFi"});
}

void test_empty_segments_are_dropped(void) {
    assertSegments(splitCategoryPath("/Tools//WiFi/"), {"Tools", "WiFi"});
    assertSegments(splitCategoryPath("/"), {});
    assertSegments(splitCategoryPath("///"), {});
}

void test_spaces_are_kept(void) {
    assertSegments(splitCategoryPath("Examples/Image Demo 1"),
                   {"Examples", "Image Demo 1"});
    assertSegments(splitCategoryPath(" / "), {" ", " "});
}

void test_matches_old_split_exhaustively(void) {
    const char alphabet[] = {'a', 'b', '/'};
    int checked = 0;
    for (int len = 0; len <= 7; ++len) {
        int total = 1;
        for (int i = 0; i < len; ++i) total *= 3;
        for (int n = 0; n < total; ++n) {
            std::string s;
            int v = n;
            for (int i = 0; i < len; ++i) {
                s.push_back(alphabet[v % 3]);
                v /= 3;
            }
            assertSegments(splitCategoryPath(s), oldParseCategoryPath(s));
            ++checked;
        }
    }
    TEST_ASSERT_EQUAL_INT(3280, checked);  // 3^0 + 3^1 + ... + 3^7
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_empty_path_has_no_segments);
    RUN_TEST(test_single_segment);
    RUN_TEST(test_two_segments);
    RUN_TEST(test_empty_segments_are_dropped);
    RUN_TEST(test_spaces_are_kept);
    RUN_TEST(test_matches_old_split_exhaustively);
    return UNITY_END();
}
