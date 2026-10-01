// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

// test/test_loadout_merge/test_merge.cpp
//
// Manifest <-> compiled-in registry merge tests
// (LoadoutManifest::mergeWithRegistry / buildFromRegistry). These pin the
// Manifest behavior: preserve manifest order, append unlisted apps
// appended in compile order, stale ids pruned-not-fatal, hidden flagged,
// empty manifest == compile order.

#include <unity.h>
#include "LoadoutManifest.h"

using namespace LoadoutManifest;

// A miniature registry mirroring the compiled-in appDefs[] shape.
// Index 1 has an empty name = not a menu item (like APP_MENU).
static const RegistryApp kRegistry[] = {
    { "APP_BOOT",   "Boot Animation", "Screensavers" },
    { "APP_MENU",   "",               ""             },
    { "APP_BOOPER", "Booper",         "Games"        },
    { "APP_FLASH",  "Flashlight",     "Tools"        }, // was "Tools/LEDs", pre-flattened
    { "APP_CLOCK",  "Clock",          "Tools"        },
    { "APP_SNAKE",  "Snake",          "Games"        },
};
static const int kRegistryCount = (int)(sizeof(kRegistry) / sizeof(kRegistry[0]));

static const RegistryApp kCanonicalRegistry[] = {
    { "booper", "Booper", "Games", "APP_BOOPER" },
    { "snake",   "Snake",  "Games", "APP_SNAKE" },
};

static LoadoutEntry makeEntry(const char* id, const char* category,
                              bool hidden = false) {
    LoadoutEntry e;
    e.id       = id;
    e.category = category;
    e.hidden   = hidden;
    return e;
}

void test_manifest_order_honored(void) {
    Loadout l;
    l.entries.push_back(makeEntry("APP_SNAKE",  "Games"));
    l.entries.push_back(makeEntry("APP_CLOCK",  "Tools"));
    l.entries.push_back(makeEntry("APP_BOOPER", "Games"));

    auto merged = mergeWithRegistry(l, kRegistry, kRegistryCount);
    // Manifest order first, then unlisted apps (compile order).
    TEST_ASSERT_EQUAL_INT(5, (int)merged.size());
    TEST_ASSERT_EQUAL_INT(5, merged[0].appIndex); // APP_SNAKE
    TEST_ASSERT_EQUAL_INT(4, merged[1].appIndex); // APP_CLOCK
    TEST_ASSERT_EQUAL_INT(2, merged[2].appIndex); // APP_BOOPER
    TEST_ASSERT_EQUAL_INT(0, merged[3].appIndex); // APP_BOOT   (appended)
    TEST_ASSERT_EQUAL_INT(3, merged[4].appIndex); // APP_FLASH  (appended)
}

void test_unlisted_apps_appended_in_compile_order(void) {
    Loadout l;
    l.entries.push_back(makeEntry("APP_CLOCK", "Tools"));

    auto merged = mergeWithRegistry(l, kRegistry, kRegistryCount);
    TEST_ASSERT_EQUAL_INT(5, (int)merged.size());
    TEST_ASSERT_EQUAL_INT(4, merged[0].appIndex); // manifest entry
    TEST_ASSERT_EQUAL_INT(0, merged[1].appIndex); // then compile order...
    TEST_ASSERT_EQUAL_INT(2, merged[2].appIndex);
    TEST_ASSERT_EQUAL_INT(3, merged[3].appIndex);
    TEST_ASSERT_EQUAL_INT(5, merged[4].appIndex);
    // Appended apps carry their (flattened) registry category.
    TEST_ASSERT_EQUAL_STRING("Screensavers", merged[1].category.c_str());
    TEST_ASSERT_FALSE(merged[1].hidden);
}

void test_stale_ids_pruned_not_fatal(void) {
    Loadout l;
    l.entries.push_back(makeEntry("APP_REMOVED_IN_UPDATE", "Games"));
    l.entries.push_back(makeEntry("APP_SNAKE", "Games"));
    l.entries.push_back(makeEntry("APP_ALSO_GONE", "Tools"));

    auto merged = mergeWithRegistry(l, kRegistry, kRegistryCount);
    // Stale entries silently dropped; everything else still works.
    TEST_ASSERT_EQUAL_INT(5, (int)merged.size());
    TEST_ASSERT_EQUAL_INT(5, merged[0].appIndex); // APP_SNAKE first
}

void test_hidden_flag_carried(void) {
    Loadout l;
    l.entries.push_back(makeEntry("APP_BOOPER", "Games", true));
    l.entries.push_back(makeEntry("APP_SNAKE",  "Games", false));

    auto merged = mergeWithRegistry(l, kRegistry, kRegistryCount);
    // Hidden entries stay in the merge (so rewrites preserve them) but
    // are flagged for the menu to skip.
    TEST_ASSERT_EQUAL_INT(2, merged[0].appIndex);
    TEST_ASSERT_TRUE(merged[0].hidden);
    TEST_ASSERT_FALSE(merged[1].hidden);
}

void test_empty_manifest_equals_compile_order(void) {
    Loadout l; // valid but empty (schemaVersion 1, no entries)
    auto merged = mergeWithRegistry(l, kRegistry, kRegistryCount);
    // Exactly compile order, minus non-menu apps (empty name).
    TEST_ASSERT_EQUAL_INT(5, (int)merged.size());
    TEST_ASSERT_EQUAL_INT(0, merged[0].appIndex);
    TEST_ASSERT_EQUAL_INT(2, merged[1].appIndex);
    TEST_ASSERT_EQUAL_INT(3, merged[2].appIndex);
    TEST_ASSERT_EQUAL_INT(4, merged[3].appIndex);
    TEST_ASSERT_EQUAL_INT(5, merged[4].appIndex);
}

void test_non_menu_apps_never_merged(void) {
    Loadout l;
    // Even a manifest that explicitly names APP_MENU can't surface it.
    l.entries.push_back(makeEntry("APP_MENU", "Tools"));
    auto merged = mergeWithRegistry(l, kRegistry, kRegistryCount);
    for (const auto& m : merged) {
        TEST_ASSERT_NOT_EQUAL(1, m.appIndex);
    }
}

void test_duplicate_manifest_ids_first_wins(void) {
    Loadout l;
    l.entries.push_back(makeEntry("APP_SNAKE", "Games"));
    l.entries.push_back(makeEntry("APP_SNAKE", "Tools", true));

    auto merged = mergeWithRegistry(l, kRegistry, kRegistryCount);
    int snakeCount = 0;
    for (const auto& m : merged) {
        if (m.appIndex == 5) {
            snakeCount++;
            TEST_ASSERT_EQUAL_STRING("Games", m.category.c_str());
            TEST_ASSERT_FALSE(m.hidden);
        }
    }
    TEST_ASSERT_EQUAL_INT(1, snakeCount);
}

void test_manifest_category_overrides_registry(void) {
    Loadout l;
    l.entries.push_back(makeEntry("APP_SNAKE", "Favorites")); // user re-categorized
    l.entries.push_back(makeEntry("APP_BOOPER", ""));         // "" -> registry category

    auto merged = mergeWithRegistry(l, kRegistry, kRegistryCount);
    TEST_ASSERT_EQUAL_STRING("Favorites", merged[0].category.c_str());
    TEST_ASSERT_EQUAL_STRING("Games",     merged[1].category.c_str());
}

void test_build_from_registry_snapshot(void) {
    Loadout l = buildFromRegistry(kRegistry, kRegistryCount);
    // Compile order, non-menu apps skipped, positions renumbered.
    TEST_ASSERT_EQUAL_INT(5, (int)l.entries.size());
    TEST_ASSERT_EQUAL_STRING("APP_BOOT",   l.entries[0].id.c_str());
    TEST_ASSERT_EQUAL_STRING("APP_BOOPER", l.entries[1].id.c_str());
    TEST_ASSERT_EQUAL_STRING("APP_SNAKE",  l.entries[4].id.c_str());
    TEST_ASSERT_EQUAL_INT(4, l.entries[4].position);
    TEST_ASSERT_FALSE(l.entries[0].hidden);
    TEST_ASSERT_EQUAL_STRING("Screensavers", l.entries[0].category.c_str());
}

void test_empty_registry_yields_empty_merge(void) {
    Loadout l;
    l.entries.push_back(makeEntry("APP_SNAKE", "Games"));
    auto merged = mergeWithRegistry(l, nullptr, 0);
    TEST_ASSERT_EQUAL_INT(0, (int)merged.size());
}

void test_slug_and_legacy_migration(void) {
    TEST_ASSERT_EQUAL_STRING("dino-run", slugifyBuiltinName("--Dino  Run--").c_str());
    Loadout l;
    l.entries.push_back(makeEntry("APP_BOOPER", "Favorites", true));
    TEST_ASSERT_TRUE(normalizeBuiltinIds(l, kCanonicalRegistry, 2));
    TEST_ASSERT_EQUAL_STRING("booper", l.entries[0].id.c_str());
    TEST_ASSERT_EQUAL_STRING("builtin", l.entries[0].format.c_str());
    TEST_ASSERT_EQUAL_STRING("Favorites", l.entries[0].category.c_str());
    TEST_ASSERT_TRUE(l.entries[0].hidden);
    TEST_ASSERT_FALSE(normalizeBuiltinIds(l, kCanonicalRegistry, 2));
}

void test_migration_dedup_keeps_slug_at_earlier_position(void) {
    Loadout l;
    l.entries.push_back(makeEntry("APP_BOOPER", "Legacy"));
    l.entries.push_back(makeEntry("snake", "Games"));
    l.entries.push_back(makeEntry("booper", "Website"));
    l.entries[2].format = "blob";
    l.entries[2].blobPath = "/apps/booper.bin";
    TEST_ASSERT_TRUE(normalizeBuiltinIds(l, kCanonicalRegistry, 2));
    TEST_ASSERT_EQUAL_INT(2, (int)l.entries.size());
    TEST_ASSERT_EQUAL_STRING("booper", l.entries[0].id.c_str());
    TEST_ASSERT_EQUAL_STRING("Website", l.entries[0].category.c_str());
    TEST_ASSERT_EQUAL_STRING("blob", l.entries[0].format.c_str());
    TEST_ASSERT_EQUAL_STRING("snake", l.entries[1].id.c_str());
}

// Two delivered apps may share a name; the menu keeps a selection on the
// right one across a rebuild by the manifest id each merged row carries.
void test_blob_rows_carry_their_manifest_id(void) {
    Loadout l;
    l.entries.push_back(makeEntry("timer-a", "Tools"));
    l.entries.push_back(makeEntry("timer-b", "Tools"));
    for (auto& e : l.entries) {
        e.format = "wasm";
        e.name = "Timer";
        e.blobPath = "/apps/" + e.id + "-0123abcd.wasm";
    }
    l.entries.push_back(makeEntry("APP_SNAKE", "Games"));
    auto merged = mergeWithRegistry(l, kRegistry, kRegistryCount);
    TEST_ASSERT_EQUAL_INT(-1, merged[0].appIndex);
    TEST_ASSERT_EQUAL_STRING("timer-a", merged[0].id.c_str());
    TEST_ASSERT_EQUAL_STRING("timer-b", merged[1].id.c_str());
    TEST_ASSERT_EQUAL_STRING(merged[0].label.c_str(), merged[1].label.c_str());
    TEST_ASSERT_EQUAL_STRING("", merged[2].id.c_str());   // builtin rows use the index
}

// The REAL compiled-in registry, straight from AppManifest.h: only the
// label and category columns are expanded (the lifecycle columns are macro
// arguments that are dropped, so nothing app-side has to link here).
enum RealAppIndex {
#define APP_ENTRY(ID, LABEL, CATPATH, BEGINF, ENDF, RUNF) ID,
#include "AppManifest.h"
#undef APP_ENTRY
    REAL_APP_COUNT
};
static const RegistryApp kRealRegistry[] = {
#define APP_ENTRY(ID, LABEL, CATPATH, BEGINF, ENDF, RUNF) { "", LABEL, CATPATH, #ID },
#include "AppManifest.h"
#undef APP_ENTRY
};

// With no manifest (after Reset to factory) the menu is built from the
// compiled registry. It must never show an internal slot as a blank row -
// the delivered-app host sat between Media and Status and opened a dead
// "no app staged" screen.
void test_compiled_menu_rows_skip_internal_slots(void) {
    auto rows = compiledMenuRows(kRealRegistry, REAL_APP_COUNT);
    TEST_ASSERT_TRUE(rows.size() > 10);
    for (int i : rows) {
        TEST_ASSERT_NOT_EQUAL(APP_WASM_HOST, i);
        TEST_ASSERT_NOT_EQUAL(APP_MENU, i);
        TEST_ASSERT_TRUE(!kRealRegistry[i].name.empty());
    }
    // Real, labelled root entries are still there.
    bool status = false, setupWifi = false;
    for (int i : rows) {
        if (i == APP_STATUS) status = true;
        if (i == APP_SETUP_WIFI) setupWifi = true;
    }
    TEST_ASSERT_TRUE(status);
    TEST_ASSERT_TRUE(setupWifi);
    // Every row that is not an internal slot is kept, in compile order.
    int named = 0;
    for (int i = 0; i < REAL_APP_COUNT; i++) if (!kRealRegistry[i].name.empty()) named++;
    TEST_ASSERT_EQUAL_INT(named, (int)rows.size());
    for (size_t k = 1; k < rows.size(); k++) TEST_ASSERT_TRUE(rows[k - 1] < rows[k]);
}

void test_compiled_menu_rows_empty_registry(void) {
    TEST_ASSERT_EQUAL_INT(0, (int)compiledMenuRows(nullptr, 0).size());
    auto rows = compiledMenuRows(kRegistry, kRegistryCount);
    TEST_ASSERT_EQUAL_INT(kRegistryCount - 1, (int)rows.size());
    for (int i : rows) TEST_ASSERT_NOT_EQUAL(1, i);
}

void setUp(void)    {}
void tearDown(void) {}

int main(int /*argc*/, char** /*argv*/) {
    UNITY_BEGIN();
    RUN_TEST(test_manifest_order_honored);
    RUN_TEST(test_unlisted_apps_appended_in_compile_order);
    RUN_TEST(test_stale_ids_pruned_not_fatal);
    RUN_TEST(test_hidden_flag_carried);
    RUN_TEST(test_empty_manifest_equals_compile_order);
    RUN_TEST(test_non_menu_apps_never_merged);
    RUN_TEST(test_duplicate_manifest_ids_first_wins);
    RUN_TEST(test_manifest_category_overrides_registry);
    RUN_TEST(test_build_from_registry_snapshot);
    RUN_TEST(test_empty_registry_yields_empty_merge);
    RUN_TEST(test_slug_and_legacy_migration);
    RUN_TEST(test_migration_dedup_keeps_slug_at_earlier_position);
    RUN_TEST(test_blob_rows_carry_their_manifest_id);
    RUN_TEST(test_compiled_menu_rows_skip_internal_slots);
    RUN_TEST(test_compiled_menu_rows_empty_registry);
    return UNITY_END();
}
