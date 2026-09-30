// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#ifndef MENU_IDENTITY_H
#define MENU_IDENTITY_H

#include <string>
#include <vector>

// Which menu item is "the same item" across a rebuild of the tree. Header
// only and templated on the item type (MenuItem on the device, a plain
// struct in the native tests), so the rule is tested without the display.
//
//  - A category is its label (categories have no other identity).
//  - A built-in app is its compiled-in app index.
//  - A delivered app is its manifest id (`blobId`); its file path changes
//    when it is replaced, and two apps may share a name. Without an id the
//    file path is used.
// Labels never identify an app: two apps may carry the same name.

template <class Item>
bool sameMenuItem(const Item& a, const Item& b)
{
    if (a.isCategory != b.isCategory) return false;
    if (a.isCategory) return a.label == b.label;
    const bool aBlob = !a.blobPath.empty();
    const bool bBlob = !b.blobPath.empty();
    if (aBlob != bBlob) return false;
    if (!aBlob) return a.appIndex == b.appIndex;
    if (!a.blobId.empty() || !b.blobId.empty()) return a.blobId == b.blobId;
    return a.blobPath == b.blobPath;
}

/// Index of the item in `items` that is the same item as `keep`, or -1.
template <class Item>
int findSameMenuItem(const std::vector<Item>& items, const Item& keep)
{
    for (int i = 0; i < (int)items.size(); i++) {
        if (sameMenuItem(items[i], keep)) return i;
    }
    return -1;
}

#endif
