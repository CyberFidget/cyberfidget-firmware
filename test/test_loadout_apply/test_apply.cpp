// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

// test/test_loadout_apply/test_apply.cpp
//
// Sync-vocabulary tests (REQ-053 clause 4): adds / removes / hides plus
// ONE declarative arrange op. Section contiguity (sections = contiguous
// category runs in flat position order) must hold by construction after
// every op — that invariant is asserted throughout.

#include <unity.h>
#include "LoadoutManifest.h"

using namespace LoadoutManifest;

static LoadoutEntry makeEntry(const char* id, const char* category,
                              bool hidden = false) {
    LoadoutEntry e;
    e.id       = id;
    e.name     = id; // label irrelevant to these tests
    e.category = category;
    e.hidden   = hidden;
    return e;
}

// Baseline: two contiguous sections, Games then Tools.
static Loadout makeBaseline(void) {
    Loadout l;
    l.entries.push_back(makeEntry("APP_A", "Games"));
    l.entries.push_back(makeEntry("APP_B", "Games"));
    l.entries.push_back(makeEntry("APP_C", "Tools"));
    l.entries.push_back(makeEntry("APP_D", "Tools"));
    for (int i = 0; i < (int)l.entries.size(); i++) l.entries[i].position = i;
    return l;
}

// Assert sections are contiguous: once a category run ends, that category
// never appears again.
static void assertContiguous(const Loadout& l) {
    std::vector<std::string> seen;
    for (size_t i = 0; i < l.entries.size(); i++) {
        const std::string& cat = l.entries[i].category;
        if (i > 0 && l.entries[i - 1].category == cat) continue; // same run
        for (const auto& s : seen) {
            if (s == cat) {
                TEST_FAIL_MESSAGE("category section is not contiguous");
            }
        }
        seen.push_back(cat);
    }
}

static void assertPositionsRenumbered(const Loadout& l) {
    for (int i = 0; i < (int)l.entries.size(); i++) {
        TEST_ASSERT_EQUAL_INT(i, l.entries[i].position);
    }
}

// ---------- add ----------

void test_add_appends_to_category_section(void) {
    Loadout l = makeBaseline();
    TEST_ASSERT_TRUE(applyAdd(l, makeEntry("APP_E", "Games")));
    // New Games entry lands at the END of the Games section, not the file.
    TEST_ASSERT_EQUAL_INT(5, (int)l.entries.size());
    TEST_ASSERT_EQUAL_STRING("APP_E", l.entries[2].id.c_str());
    assertContiguous(l);
    assertPositionsRenumbered(l);
}

void test_add_new_category_becomes_new_section(void) {
    Loadout l = makeBaseline();
    TEST_ASSERT_TRUE(applyAdd(l, makeEntry("APP_E", "Media")));
    TEST_ASSERT_EQUAL_STRING("APP_E", l.entries[4].id.c_str());
    TEST_ASSERT_EQUAL_STRING("Media", l.entries[4].category.c_str());
    assertContiguous(l);
}

void test_add_duplicate_id_rejected(void) {
    Loadout l = makeBaseline();
    TEST_ASSERT_FALSE(applyAdd(l, makeEntry("APP_A", "Tools")));
    TEST_ASSERT_EQUAL_INT(4, (int)l.entries.size());
    TEST_ASSERT_FALSE(applyAdd(l, makeEntry("", "Games"))); // empty id
}

// ---------- remove ----------

void test_remove_deletes_entry(void) {
    Loadout l = makeBaseline();
    TEST_ASSERT_TRUE(applyRemove(l, "APP_B"));
    TEST_ASSERT_EQUAL_INT(3, (int)l.entries.size());
    TEST_ASSERT_EQUAL_STRING("APP_A", l.entries[0].id.c_str());
    TEST_ASSERT_EQUAL_STRING("APP_C", l.entries[1].id.c_str());
    assertContiguous(l);
    assertPositionsRenumbered(l);
}

void test_remove_unknown_id_fails_without_change(void) {
    Loadout l = makeBaseline();
    TEST_ASSERT_FALSE(applyRemove(l, "APP_NOPE"));
    TEST_ASSERT_FALSE(applyRemove(l, nullptr));
    TEST_ASSERT_EQUAL_INT(4, (int)l.entries.size());
}

// ---------- hide ----------

void test_hide_sets_flag_keeps_position(void) {
    Loadout l = makeBaseline();
    TEST_ASSERT_TRUE(applyHide(l, "APP_C", true));
    TEST_ASSERT_TRUE(l.entries[2].hidden);
    TEST_ASSERT_EQUAL_INT(4, (int)l.entries.size()); // hide != remove
    TEST_ASSERT_TRUE(applyHide(l, "APP_C", false));  // and back
    TEST_ASSERT_FALSE(l.entries[2].hidden);
}

void test_hide_unknown_id_fails(void) {
    Loadout l = makeBaseline();
    TEST_ASSERT_FALSE(applyHide(l, "APP_NOPE", true));
    TEST_ASSERT_FALSE(applyHide(l, nullptr, true));
}

// ---------- arrange ----------

static ArrangeItem arr(const char* id) {
    ArrangeItem it;
    it.id = id;
    return it;
}

static ArrangeItem arrCat(const char* id, const char* category) {
    ArrangeItem it;
    it.id          = id;
    it.category    = category;
    it.hasCategory = true;
    return it;
}

void test_arrange_full_reorder(void) {
    Loadout l = makeBaseline();
    std::vector<ArrangeItem> order = {
        arr("APP_D"), arr("APP_C"), arr("APP_B"), arr("APP_A"),
    };
    TEST_ASSERT_TRUE(applyArrange(l, order));
    TEST_ASSERT_EQUAL_STRING("APP_D", l.entries[0].id.c_str());
    TEST_ASSERT_EQUAL_STRING("APP_C", l.entries[1].id.c_str());
    TEST_ASSERT_EQUAL_STRING("APP_B", l.entries[2].id.c_str());
    TEST_ASSERT_EQUAL_STRING("APP_A", l.entries[3].id.c_str());
    assertContiguous(l);
    assertPositionsRenumbered(l);
}

void test_arrange_with_category_override_moves_sections(void) {
    Loadout l = makeBaseline();
    // Move APP_B into Tools, at the front of that section.
    std::vector<ArrangeItem> order = {
        arr("APP_A"),
        arrCat("APP_B", "Tools"),
        arr("APP_C"),
        arr("APP_D"),
    };
    TEST_ASSERT_TRUE(applyArrange(l, order));
    TEST_ASSERT_EQUAL_STRING("APP_B", l.entries[1].id.c_str());
    TEST_ASSERT_EQUAL_STRING("Tools", l.entries[1].category.c_str());
    assertContiguous(l);
}

void test_arrange_noncontiguous_input_normalized(void) {
    Loadout l = makeBaseline();
    // Interleaved categories: Games, Tools, Games, Tools. Contiguity must
    // be restored by construction — first-appearance section order, stable
    // order within each section.
    std::vector<ArrangeItem> order = {
        arr("APP_B"), arr("APP_C"), arr("APP_A"), arr("APP_D"),
    };
    TEST_ASSERT_TRUE(applyArrange(l, order));
    TEST_ASSERT_EQUAL_STRING("APP_B", l.entries[0].id.c_str()); // Games
    TEST_ASSERT_EQUAL_STRING("APP_A", l.entries[1].id.c_str()); // Games
    TEST_ASSERT_EQUAL_STRING("APP_C", l.entries[2].id.c_str()); // Tools
    TEST_ASSERT_EQUAL_STRING("APP_D", l.entries[3].id.c_str()); // Tools
    assertContiguous(l);
    assertPositionsRenumbered(l);
}

void test_arrange_unknown_ids_ignored(void) {
    Loadout l = makeBaseline();
    std::vector<ArrangeItem> order = {
        arr("APP_GHOST"), arr("APP_D"), arr("APP_A"),
        arr("APP_B"), arr("APP_C"), arr("APP_GONE"),
    };
    TEST_ASSERT_TRUE(applyArrange(l, order));
    TEST_ASSERT_EQUAL_INT(4, (int)l.entries.size()); // nothing invented, nothing lost
    TEST_ASSERT_EQUAL_STRING("APP_D", l.entries[0].id.c_str());
    assertContiguous(l);
}

void test_arrange_missing_entries_appended_stably(void) {
    Loadout l = makeBaseline();
    // Order only names two of the four: the others keep their relative
    // order and are appended after (never dropped).
    std::vector<ArrangeItem> order = { arr("APP_C"), arr("APP_A") };
    TEST_ASSERT_TRUE(applyArrange(l, order));
    TEST_ASSERT_EQUAL_INT(4, (int)l.entries.size());
    TEST_ASSERT_EQUAL_STRING("APP_C", l.entries[0].id.c_str()); // Tools
    TEST_ASSERT_EQUAL_STRING("APP_D", l.entries[1].id.c_str()); // Tools (pulled into section)
    TEST_ASSERT_EQUAL_STRING("APP_A", l.entries[2].id.c_str()); // Games
    TEST_ASSERT_EQUAL_STRING("APP_B", l.entries[3].id.c_str()); // Games
    assertContiguous(l);
}

void test_arrange_preserves_hidden_flags(void) {
    Loadout l = makeBaseline();
    TEST_ASSERT_TRUE(applyHide(l, "APP_B", true));
    std::vector<ArrangeItem> order = {
        arr("APP_B"), arr("APP_A"), arr("APP_C"), arr("APP_D"),
    };
    TEST_ASSERT_TRUE(applyArrange(l, order));
    TEST_ASSERT_EQUAL_STRING("APP_B", l.entries[0].id.c_str());
    TEST_ASSERT_TRUE(l.entries[0].hidden); // arrange must not un-hide
}

// ---------- op sequences (REQ-053 clause 4 mirror) ----------

void test_op_sequence_keeps_contiguity(void) {
    Loadout l = makeBaseline();
    TEST_ASSERT_TRUE(applyAdd(l, makeEntry("APP_E", "Media")));
    TEST_ASSERT_TRUE(applyAdd(l, makeEntry("APP_F", "Games")));
    TEST_ASSERT_TRUE(applyRemove(l, "APP_A"));
    TEST_ASSERT_TRUE(applyHide(l, "APP_D", true));
    std::vector<ArrangeItem> order = {
        arr("APP_E"), arr("APP_C"), arr("APP_D"), arr("APP_F"), arr("APP_B"),
    };
    TEST_ASSERT_TRUE(applyArrange(l, order));
    assertContiguous(l);
    assertPositionsRenumbered(l);
    TEST_ASSERT_EQUAL_INT(5, (int)l.entries.size());
    // Section order by first appearance: Media, Tools, Games.
    TEST_ASSERT_EQUAL_STRING("APP_E", l.entries[0].id.c_str());
    TEST_ASSERT_EQUAL_STRING("APP_C", l.entries[1].id.c_str());
    TEST_ASSERT_EQUAL_STRING("APP_D", l.entries[2].id.c_str());
    TEST_ASSERT_EQUAL_STRING("APP_F", l.entries[3].id.c_str());
    TEST_ASSERT_EQUAL_STRING("APP_B", l.entries[4].id.c_str());
    TEST_ASSERT_TRUE(l.entries[2].hidden); // survived the arrange
}

void setUp(void)    {}
void tearDown(void) {}

int main(int /*argc*/, char** /*argv*/) {
    UNITY_BEGIN();
    RUN_TEST(test_add_appends_to_category_section);
    RUN_TEST(test_add_new_category_becomes_new_section);
    RUN_TEST(test_add_duplicate_id_rejected);
    RUN_TEST(test_remove_deletes_entry);
    RUN_TEST(test_remove_unknown_id_fails_without_change);
    RUN_TEST(test_hide_sets_flag_keeps_position);
    RUN_TEST(test_hide_unknown_id_fails);
    RUN_TEST(test_arrange_full_reorder);
    RUN_TEST(test_arrange_with_category_override_moves_sections);
    RUN_TEST(test_arrange_noncontiguous_input_normalized);
    RUN_TEST(test_arrange_unknown_ids_ignored);
    RUN_TEST(test_arrange_missing_entries_appended_stably);
    RUN_TEST(test_arrange_preserves_hidden_flags);
    RUN_TEST(test_op_sequence_keeps_contiguity);
    return UNITY_END();
}
