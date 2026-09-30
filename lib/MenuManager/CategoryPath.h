// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#ifndef CATEGORY_PATH_H
#define CATEGORY_PATH_H

#include <string>
#include <vector>

/**
 * @brief Split a menu category path like "Tools/WiFi" into its segments.
 *
 * Splits on '/' and drops empty segments, so "Tools//WiFi/" and "/Tools/WiFi"
 * both give ["Tools", "WiFi"] and "" gives []. This matches the old
 * std::stringstream + std::getline split exactly, without <sstream>, which
 * would link the whole iostream/locale runtime into the device image.
 *
 * Header-only and free of Arduino deps so the native tests can pin it
 * (test/test_core_menupath).
 */
inline std::vector<std::string> splitCategoryPath(const std::string &path)
{
    std::vector<std::string> result;
    size_t start = 0;
    while (start < path.size()) {
        size_t slash = path.find('/', start);
        if (slash == std::string::npos) slash = path.size();
        if (slash > start) {
            result.push_back(path.substr(start, slash - start));
        }
        start = slash + 1;
    }
    return result;
}

#endif // CATEGORY_PATH_H
