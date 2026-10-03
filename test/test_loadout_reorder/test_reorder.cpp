// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#include <unity.h>
#include "LoadoutManifest.h"

using namespace LoadoutManifest;

void setUp(void) {}
void tearDown(void) {}

void test_leaf_ids_match_saved_menu(void) {
    TEST_ASSERT_EQUAL_STRING("dino-run", menuLeafId(false, "Dino Run", "").c_str());
    TEST_ASSERT_EQUAL_STRING("flashlight", menuLeafId(false, "Flashlight", "ignored").c_str());
    TEST_ASSERT_EQUAL_STRING("delivered-one", menuLeafId(true, "App Host", "delivered-one").c_str());
    TEST_ASSERT_EQUAL_STRING("delivered-two", menuLeafId(true, "App Host", "delivered-two").c_str());
    TEST_ASSERT_TRUE(menuLeafId(true, "App Host", "").empty());
    TEST_ASSERT_TRUE(menuLeafId(false, nullptr, "").empty());
    TEST_ASSERT_TRUE(menuLeafId(false, "", "").empty());
}

static const RegistryApp kRegistry[] = {
    { "booper", "Booper", "Games", "APP_BOOPER" },
    { "breakout", "Breakout", "Games", "APP_BREAKOUT" },
};

static Loadout savedMenu(void) {
    Loadout loadout;
    TEST_ASSERT_TRUE(parseManifest(
        R"({"schemaVersion":1,"entries":[{"id":"booper","name":"Booper","category":"Games","position":0,"format":"builtin"},{"id":"breakout","name":"Breakout","category":"Games","position":1,"format":"builtin"}]})",
        loadout));
    return loadout;
}

// Minimal host leaf data: the device's category traversal supplies the
// full path, while the shared helper chooses the saved-menu id.
struct Leaf {
    bool isBlob;
    const char* builtinName;
    std::string blobId;
    std::string category;
};

void test_leaf_arrange_moves_breakout_above_booper(void) {
    Loadout loadout = savedMenu();
    const std::string before = serializeManifest(loadout);
    const Leaf leaves[] = {
        { false, "Breakout", "", "Games" },
        { false, "Booper", "", "Games" },
    };
    std::vector<ArrangeItem> order;
    for (const auto& leaf : leaves) {
        ArrangeItem item;
        item.id = menuLeafId(leaf.isBlob, leaf.builtinName, leaf.blobId);
        item.category = leaf.category;
        item.hasCategory = true;
        order.push_back(item);
    }
    TEST_ASSERT_TRUE(applyArrange(loadout, order, kRegistry, 2));
    TEST_ASSERT_TRUE(before != serializeManifest(loadout));
    // Round-trip the changed saved file before rebuilding the menu.
    Loadout restarted;
    TEST_ASSERT_TRUE(parseManifest(serializeManifest(loadout).c_str(), restarted));
    const auto merged = mergeWithRegistry(restarted, kRegistry, 2);
    TEST_ASSERT_EQUAL_UINT(2, merged.size());
    TEST_ASSERT_EQUAL_STRING("Games", merged[0].category.c_str());
    TEST_ASSERT_EQUAL_STRING("Breakout", kRegistry[merged[0].appIndex].name.c_str());
    TEST_ASSERT_EQUAL_STRING("Booper", kRegistry[merged[1].appIndex].name.c_str());
}

void test_legacy_leaf_ids_leave_saved_order_unchanged(void) {
    Loadout loadout = savedMenu();
    const std::string before = serializeManifest(loadout);
    // The old collector sent enum names, so the same requested move failed:
    // unknown ids are ignored and the saved file stays byte-identical.
    const std::vector<ArrangeItem> order = {
        { "APP_BREAKOUT", "Games", true },
        { "APP_BOOPER", "Games", true },
    };
    TEST_ASSERT_TRUE(applyArrange(loadout, order, kRegistry, 2));
    TEST_ASSERT_EQUAL_STRING(before.c_str(), serializeManifest(loadout).c_str());
    const auto merged = mergeWithRegistry(loadout, kRegistry, 2);
    TEST_ASSERT_EQUAL_UINT(2, merged.size());
    TEST_ASSERT_EQUAL_STRING("Games", merged[0].category.c_str());
    TEST_ASSERT_EQUAL_STRING("Booper", kRegistry[merged[0].appIndex].name.c_str());
    TEST_ASSERT_EQUAL_STRING("Breakout", kRegistry[merged[1].appIndex].name.c_str());
}

int main(int /*argc*/, char** /*argv*/) {
    UNITY_BEGIN();
    RUN_TEST(test_leaf_ids_match_saved_menu);
    RUN_TEST(test_leaf_arrange_moves_breakout_above_booper);
    RUN_TEST(test_legacy_leaf_ids_leave_saved_order_unchanged);
    return UNITY_END();
}
