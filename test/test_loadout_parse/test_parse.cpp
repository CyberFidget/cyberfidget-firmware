// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

// test/test_loadout_parse/test_parse.cpp
//
// Loadout manifest JSON parse + serialize round-trip tests
// (LoadoutManifest::parseManifest / serializeManifest). Schema doc:
// lib/LoadoutManifest/README.md.

#include <unity.h>
#include "LoadoutManifest.h"

using namespace LoadoutManifest;

static const char* kValidManifest = R"JSON({
  "schemaVersion": 1,
  "entries": [
    { "id": "APP_SNAKE",  "name": "Snake",  "category": "Games", "position": 1, "hidden": false },
    { "id": "APP_BOOPER", "name": "Booper", "category": "Games", "position": 0, "hidden": true }
  ]
})JSON";

void test_valid_manifest_parses(void) {
    Loadout l;
    TEST_ASSERT_TRUE(parseManifest(kValidManifest, l));
    TEST_ASSERT_EQUAL_INT(1, l.schemaVersion);
    TEST_ASSERT_EQUAL_INT(2, (int)l.entries.size());
    // Entries come back sorted by position and renumbered 0..n-1.
    TEST_ASSERT_EQUAL_STRING("APP_BOOPER", l.entries[0].id.c_str());
    TEST_ASSERT_EQUAL_INT(0, l.entries[0].position);
    TEST_ASSERT_TRUE(l.entries[0].hidden);
    TEST_ASSERT_EQUAL_STRING("APP_SNAKE", l.entries[1].id.c_str());
    TEST_ASSERT_EQUAL_INT(1, l.entries[1].position);
    TEST_ASSERT_FALSE(l.entries[1].hidden);
    TEST_ASSERT_EQUAL_STRING("Games", l.entries[0].category.c_str());
    TEST_ASSERT_EQUAL_STRING("Booper", l.entries[0].name.c_str());
}

void test_unknown_fields_skipped(void) {
    // Forward compat: unknown scalar, object, and array fields are ignored.
    const char* json = R"JSON({
      "schemaVersion": 1,
      "futureTopLevel": { "nested": [1, 2, {"deep": true}] },
      "entries": [
        { "id": "APP_SNAKE", "name": "Snake", "category": "Games",
          "position": 0, "hidden": false,
          "futureField": "whatever", "futureNum": 3.75,
          "futureObj": {"a": [null, false]} }
      ]
    })JSON";
    Loadout l;
    TEST_ASSERT_TRUE(parseManifest(json, l));
    TEST_ASSERT_EQUAL_INT(1, (int)l.entries.size());
    TEST_ASSERT_EQUAL_STRING("APP_SNAKE", l.entries[0].id.c_str());
}

void test_reserved_fields_roundtrip(void) {
    const char* json = R"JSON({
      "schemaVersion": 1,
      "entries": [
        { "id": "APP_X", "name": "X", "category": "Games", "position": 0,
          "hidden": false, "format": "blob", "blobPath": "/apps/x.bin",
          "version": "1.2.3", "abi": "hal-2", "signature": "deadbeef" }
      ]
    })JSON";
    Loadout l;
    TEST_ASSERT_TRUE(parseManifest(json, l));
    TEST_ASSERT_EQUAL_STRING("blob",        l.entries[0].format.c_str());
    TEST_ASSERT_EQUAL_STRING("/apps/x.bin", l.entries[0].blobPath.c_str());
    TEST_ASSERT_EQUAL_STRING("1.2.3",       l.entries[0].version.c_str());
    TEST_ASSERT_EQUAL_STRING("hal-2",       l.entries[0].abi.c_str());
    TEST_ASSERT_EQUAL_STRING("deadbeef",    l.entries[0].signature.c_str());

    // Reserved fields survive a serialize -> parse round trip.
    Loadout l2;
    TEST_ASSERT_TRUE(parseManifest(serializeManifest(l).c_str(), l2));
    TEST_ASSERT_EQUAL_STRING("blob",     l2.entries[0].format.c_str());
    TEST_ASSERT_EQUAL_STRING("deadbeef", l2.entries[0].signature.c_str());
}

void test_malformed_json_rejected(void) {
    Loadout l;
    TEST_ASSERT_FALSE(parseManifest("", l));
    TEST_ASSERT_FALSE(parseManifest(nullptr, l));
    TEST_ASSERT_FALSE(parseManifest("not json at all", l));
    TEST_ASSERT_FALSE(parseManifest("{\"schemaVersion\": 1", l));          // truncated
    TEST_ASSERT_FALSE(parseManifest("{\"schemaVersion\": 1} garbage", l)); // trailing garbage
    TEST_ASSERT_FALSE(parseManifest("[1, 2, 3]", l));                      // root must be object
    TEST_ASSERT_FALSE(parseManifest(
        "{\"schemaVersion\": 1, \"entries\": [{\"id\": }]}", l));          // bad value
}

void test_missing_schema_version_rejected(void) {
    Loadout l;
    TEST_ASSERT_FALSE(parseManifest("{\"entries\": []}", l));
    TEST_ASSERT_FALSE(parseManifest("{}", l));
}

void test_wrong_schema_version_rejected(void) {
    // No legacy readers, no migration shims: any other version is
    // unreadable and the caller falls back to compile order.
    Loadout l;
    TEST_ASSERT_FALSE(parseManifest("{\"schemaVersion\": 2, \"entries\": []}", l));
    TEST_ASSERT_FALSE(parseManifest("{\"schemaVersion\": 0, \"entries\": []}", l));
}

void test_failed_parse_leaves_output_untouched(void) {
    Loadout l;
    TEST_ASSERT_TRUE(parseManifest(kValidManifest, l));
    TEST_ASSERT_FALSE(parseManifest("{\"schemaVersion\": 2}", l));
    // Previous successful parse still intact.
    TEST_ASSERT_EQUAL_INT(2, (int)l.entries.size());
    TEST_ASSERT_EQUAL_STRING("APP_BOOPER", l.entries[0].id.c_str());
}

void test_entry_without_id_dropped(void) {
    const char* json = R"JSON({
      "schemaVersion": 1,
      "entries": [
        { "name": "No Id", "category": "Games", "position": 0 },
        { "id": "APP_SNAKE", "name": "Snake", "category": "Games", "position": 1 }
      ]
    })JSON";
    Loadout l;
    TEST_ASSERT_TRUE(parseManifest(json, l));
    TEST_ASSERT_EQUAL_INT(1, (int)l.entries.size());
    TEST_ASSERT_EQUAL_STRING("APP_SNAKE", l.entries[0].id.c_str());
}

void test_missing_entries_means_empty(void) {
    Loadout l;
    TEST_ASSERT_TRUE(parseManifest("{\"schemaVersion\": 1}", l));
    TEST_ASSERT_EQUAL_INT(0, (int)l.entries.size());
}

void test_missing_positions_use_array_order(void) {
    const char* json = R"JSON({
      "schemaVersion": 1,
      "entries": [
        { "id": "APP_B", "name": "B", "category": "Games" },
        { "id": "APP_A", "name": "A", "category": "Games" }
      ]
    })JSON";
    Loadout l;
    TEST_ASSERT_TRUE(parseManifest(json, l));
    TEST_ASSERT_EQUAL_STRING("APP_B", l.entries[0].id.c_str());
    TEST_ASSERT_EQUAL_STRING("APP_A", l.entries[1].id.c_str());
}

void test_string_escapes_roundtrip(void) {
    Loadout l;
    LoadoutEntry e;
    e.id       = "APP_X";
    e.name     = "Say \"hi\"\\now\ttab";
    e.category = "Games";
    l.entries.push_back(e);

    Loadout l2;
    TEST_ASSERT_TRUE(parseManifest(serializeManifest(l).c_str(), l2));
    TEST_ASSERT_EQUAL_STRING("Say \"hi\"\\now\ttab", l2.entries[0].name.c_str());
}

void test_serialize_parse_roundtrip_preserves_order(void) {
    Loadout l;
    TEST_ASSERT_TRUE(parseManifest(kValidManifest, l));
    Loadout l2;
    TEST_ASSERT_TRUE(parseManifest(serializeManifest(l).c_str(), l2));
    TEST_ASSERT_EQUAL_INT((int)l.entries.size(), (int)l2.entries.size());
    for (size_t i = 0; i < l.entries.size(); i++) {
        TEST_ASSERT_EQUAL_STRING(l.entries[i].id.c_str(), l2.entries[i].id.c_str());
        TEST_ASSERT_EQUAL(l.entries[i].hidden, l2.entries[i].hidden);
        TEST_ASSERT_EQUAL_INT(l.entries[i].position, l2.entries[i].position);
    }
}

void test_flatten_category(void) {
    TEST_ASSERT_EQUAL_STRING("Games", flattenCategory("Games/Arcade").c_str());
    TEST_ASSERT_EQUAL_STRING("Tools", flattenCategory("Tools/LEDs").c_str());
    TEST_ASSERT_EQUAL_STRING("Tools", flattenCategory("Tools").c_str());
    TEST_ASSERT_EQUAL_STRING("",      flattenCategory("").c_str());
    TEST_ASSERT_EQUAL_STRING("",      flattenCategory(nullptr).c_str());
}

void setUp(void)    {}
void tearDown(void) {}

int main(int /*argc*/, char** /*argv*/) {
    UNITY_BEGIN();
    RUN_TEST(test_valid_manifest_parses);
    RUN_TEST(test_unknown_fields_skipped);
    RUN_TEST(test_reserved_fields_roundtrip);
    RUN_TEST(test_malformed_json_rejected);
    RUN_TEST(test_missing_schema_version_rejected);
    RUN_TEST(test_wrong_schema_version_rejected);
    RUN_TEST(test_failed_parse_leaves_output_untouched);
    RUN_TEST(test_entry_without_id_dropped);
    RUN_TEST(test_missing_entries_means_empty);
    RUN_TEST(test_missing_positions_use_array_order);
    RUN_TEST(test_string_escapes_roundtrip);
    RUN_TEST(test_serialize_parse_roundtrip_preserves_order);
    RUN_TEST(test_flatten_category);
    return UNITY_END();
}
