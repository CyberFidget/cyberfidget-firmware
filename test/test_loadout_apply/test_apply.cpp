// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

// test/test_loadout_apply/test_apply.cpp
//
// Sync-operation tests: adds / removes / hides plus
// ONE declarative arrange op. Section contiguity (sections = contiguous
// category runs in flat position order) must hold by construction after
// every op — that invariant is asserted throughout.

#include <unity.h>
#include <algorithm>
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

// The apps the device shows for `l`, per the real mergeWithRegistry: a
// built-in as "b:<id>", a delivered app as "w:<id>|<file>"; hidden rows
// left out. Sorted, so two calls compare as sets.
static std::vector<std::string> shownApps(const Loadout& l,
                                          const RegistryApp* apps, int count) {
    std::vector<std::string> out;
    for (const auto& m : mergeWithRegistry(l, apps, count)) {
        if (m.hidden) continue;
        out.push_back(m.appIndex >= 0 ? "b:" + apps[m.appIndex].id
                                      : "w:" + m.id + "|" + m.blobPath);
    }
    std::sort(out.begin(), out.end());
    return out;
}

// applyArrange plus the invariant every arrange test checks: an arrange
// never changes which apps the device shows (it cannot hide or remove).
// Without a registry, one is made from the entries' built-in ids so the
// merge still runs.
static bool arrangeChecked(Loadout& l, const std::vector<ArrangeItem>& order,
                           const RegistryApp* apps = nullptr, int count = 0) {
    std::vector<RegistryApp> made;
    if (!apps) {
        for (const auto& e : l.entries) {
            if (e.format != "builtin" && !e.format.empty()) continue;
            bool dup = false;
            for (const auto& r : made) if (r.id == e.id) dup = true;
            if (!dup) made.push_back(RegistryApp{ e.id, e.id, "", "" });
        }
    }
    const RegistryApp* reg = apps ? apps : made.data();
    const int regCount = apps ? count : (int)made.size();
    const std::vector<std::string> before = shownApps(l, reg, regCount);
    const bool ok = applyArrange(l, order, apps, count);
    const std::vector<std::string> after = shownApps(l, reg, regCount);
    TEST_ASSERT_EQUAL_INT((int)before.size(), (int)after.size());
    for (size_t i = 0; i < before.size(); i++)
        TEST_ASSERT_EQUAL_STRING(before[i].c_str(), after[i].c_str());
    return ok;
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
    TEST_ASSERT_TRUE(arrangeChecked(l, order));
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
    TEST_ASSERT_TRUE(arrangeChecked(l, order));
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
    TEST_ASSERT_TRUE(arrangeChecked(l, order));
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
    TEST_ASSERT_TRUE(arrangeChecked(l, order));
    TEST_ASSERT_EQUAL_INT(4, (int)l.entries.size()); // nothing invented, nothing lost
    TEST_ASSERT_EQUAL_STRING("APP_D", l.entries[0].id.c_str());
    assertContiguous(l);
}

void test_arrange_missing_entries_appended_stably(void) {
    Loadout l = makeBaseline();
    // Order only names two of the four: the others keep their relative
    // order and are appended after (never dropped).
    std::vector<ArrangeItem> order = { arr("APP_C"), arr("APP_A") };
    TEST_ASSERT_TRUE(arrangeChecked(l, order));
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
    TEST_ASSERT_TRUE(arrangeChecked(l, order));
    TEST_ASSERT_EQUAL_STRING("APP_B", l.entries[0].id.c_str());
    TEST_ASSERT_TRUE(l.entries[0].hidden); // arrange must not un-hide
}

// ---------- operation sequences ----------

// The registry the seeded tests use: compiled order, with the two LED apps
// interleaved among the Tools apps as in the real AppManifest.h.
static const RegistryApp kSeedApps[] = {
    { "boot-animation", "Boot Animation",     "Screensavers" },
    { "",               "",                   ""             }, // the menu
    { "booper",         "Booper",             "Games"        },
    { "flashlight",     "Flashlight",         "Tools/LEDs"   },
    { "power-manager",  "Power Manager",      "Tools"        },
    { "clock",          "Clock",              "Tools"        },
    { "snake",          "Snake",              "Games"        },
    { "accelerometer",  "Accelerometer Demo", "Tools/LEDs"   },
    { "music-player",   "Music Player",       "Media"        },
};

// Would the device menu show this row? Models mergeWithRegistry's pruning
// (a built-in must be a named registry app and the first row with its id;
// a delivered app needs its file) plus buildNestedMenu skipping hidden
// rows. No registry = the duplicate and hidden rules only. `seen` collects
// built-in ids across one pass over the entries.
static bool deviceShows(const LoadoutEntry& e, const RegistryApp* apps, int count,
                        std::vector<std::string>& seen) {
    const bool builtin = e.format == "builtin" || e.format.empty();
    if (builtin) {
        if (std::find(seen.begin(), seen.end(), e.id) != seen.end()) return false;
        seen.push_back(e.id);
    }
    if (e.hidden) return false;
    if (!apps) return true;
    if (!builtin) return !e.blobPath.empty();
    for (int i = 0; i < count; i++) {
        if (apps[i].id == e.id) return !apps[i].name.empty();
    }
    return false;
}

// The shown children of the top-level menu folder `top`, in the order the
// device menu builder creates them from this flat order (a leaf as its id,
// a submenu as "[name]" at its first shown entry) - mirrors the merge +
// buildNestedMenu + registerApp / findOrCreateCategory for one level of
// nesting.
static std::vector<std::string> childrenOf(const Loadout& l, const char* top,
                                           const RegistryApp* apps = nullptr,
                                           int count = 0) {
    std::vector<std::string> kids;
    std::vector<std::string> seen;
    const std::string prefix = std::string(top) + "/";
    for (const auto& e : l.entries) {
        if (!deviceShows(e, apps, count, seen)) continue;
        if (e.category == top) {
            kids.push_back(e.id);
        } else if (e.category.compare(0, prefix.size(), prefix) == 0) {
            const std::string folder = "[" + e.category.substr(prefix.size()) + "]";
            bool seen = false;
            for (const auto& k : kids) if (k == folder) seen = true;
            if (!seen) kids.push_back(folder);
        }
    }
    return kids;
}

static void assertKids(const std::vector<std::string>& kids,
                       const char* const* want, int n) {
    TEST_ASSERT_EQUAL_INT(n, (int)kids.size());
    for (int i = 0; i < n; i++) TEST_ASSERT_EQUAL_STRING(want[i], kids[i].c_str());
}

// First write: arranging only Clock moves Clock to the front of Tools; the
// LEDs folder keeps its place ahead of Power Manager.
void test_arrange_seeded_only_clock_keeps_leds_place(void) {
    Loadout l = buildFromRegistry(kSeedApps, 9);
    const char* const before[] = { "[LEDs]", "power-manager", "clock" };
    assertKids(childrenOf(l, "Tools"), before, 3);

    std::vector<ArrangeItem> order = { arr("clock") };
    TEST_ASSERT_TRUE(arrangeChecked(l, order));
    const char* const after[] = { "clock", "[LEDs]", "power-manager" };
    assertKids(childrenOf(l, "Tools"), after, 3);
    // Flat order: the Tools section is contiguous, LEDs entries together.
    const char* ids[] = { "clock", "flashlight", "accelerometer", "power-manager",
                          "boot-animation", "booper", "snake", "music-player" };
    TEST_ASSERT_EQUAL_INT(8, (int)l.entries.size());
    for (int i = 0; i < 8; i++) TEST_ASSERT_EQUAL_STRING(ids[i], l.entries[i].id.c_str());
    TEST_ASSERT_EQUAL_STRING("Tools/LEDs", l.entries[1].category.c_str());
    TEST_ASSERT_EQUAL_STRING("Tools/LEDs", l.entries[2].category.c_str());
    assertPositionsRenumbered(l);
}

// Flashlight hidden: the device shows Tools as Power Manager, Clock, LEDs
// (the folder appears at Accelerometer, its first visible entry).
// Arranging only Clock must give Clock, Power Manager, LEDs.
void test_arrange_hidden_first_entry_keeps_visible_order(void) {
    Loadout l = buildFromRegistry(kSeedApps, 9);
    TEST_ASSERT_TRUE(applyHide(l, "flashlight", true));
    const char* const before[] = { "power-manager", "clock", "[LEDs]" };
    assertKids(childrenOf(l, "Tools"), before, 3);

    std::vector<ArrangeItem> order = { arr("clock") };
    TEST_ASSERT_TRUE(arrangeChecked(l, order));
    const char* const after[] = { "clock", "power-manager", "[LEDs]" };
    assertKids(childrenOf(l, "Tools"), after, 3);
    // Hidden Flashlight stays with its submenu and keeps its flag.
    const char* ids[] = { "clock", "power-manager", "flashlight", "accelerometer" };
    for (int i = 0; i < 4; i++) TEST_ASSERT_EQUAL_STRING(ids[i], l.entries[i].id.c_str());
    TEST_ASSERT_TRUE(l.entries[2].hidden);
}

// A submenu whose entries are all hidden is gathered at its first entry.
void test_arrange_all_hidden_submenu_at_first_entry(void) {
    Loadout l = buildFromRegistry(kSeedApps, 9);
    TEST_ASSERT_TRUE(applyHide(l, "flashlight", true));
    TEST_ASSERT_TRUE(applyHide(l, "accelerometer", true));
    std::vector<ArrangeItem> order = { arr("clock") };
    TEST_ASSERT_TRUE(arrangeChecked(l, order));
    // clock, then the rest in seed order; Tools: flashlight (anchor),
    // accelerometer gathered with it, then power-manager.
    const char* ids[] = { "clock", "flashlight", "accelerometer", "power-manager" };
    for (int i = 0; i < 4; i++) TEST_ASSERT_EQUAL_STRING(ids[i], l.entries[i].id.c_str());
}

// A hostile, very deep category (1,000 levels, fits an 8 KB request) must
// not exhaust the stack and gives a defined order: only the first 8
// levels are used for ordering; the stored string is unchanged.
void test_arrange_very_deep_category_is_bounded(void) {
    std::string deep;
    for (int i = 0; i < 1000; i++) deep += (i ? "/a" : "a");
    Loadout l;
    l.entries.push_back(makeEntry("d1", deep.c_str()));
    l.entries.push_back(makeEntry("t1", "Tools"));
    l.entries.push_back(makeEntry("d2", deep.c_str()));
    std::vector<ArrangeItem> order = { arr("t1"), arr("d2"), arr("d1") };
    TEST_ASSERT_TRUE(arrangeChecked(l, order));
    TEST_ASSERT_EQUAL_STRING("t1", l.entries[0].id.c_str());
    TEST_ASSERT_EQUAL_STRING("d2", l.entries[1].id.c_str());
    TEST_ASSERT_EQUAL_STRING("d1", l.entries[2].id.c_str());
    TEST_ASSERT_EQUAL_STRING(deep.c_str(), l.entries[1].category.c_str());

    // Below level 8 the order is not regrouped: q1 and q3 share a ninth
    // level "b" but keep their relative order around q2 ("z").
    const std::string base = "a/a/a/a/a/a/a/a";
    Loadout q;
    q.entries.push_back(makeEntry("q1", (base + "/b/c").c_str()));
    q.entries.push_back(makeEntry("q2", (base + "/z").c_str()));
    q.entries.push_back(makeEntry("q3", (base + "/b/d").c_str()));
    std::vector<ArrangeItem> qo = { arr("q1"), arr("q2"), arr("q3") };
    TEST_ASSERT_TRUE(arrangeChecked(q, qo));
    TEST_ASSERT_EQUAL_STRING("q1", q.entries[0].id.c_str());
    TEST_ASSERT_EQUAL_STRING("q2", q.entries[1].id.c_str());
    TEST_ASSERT_EQUAL_STRING("q3", q.entries[2].id.c_str());
}

static LoadoutEntry builtinEntry(const char* id, const char* category) {
    LoadoutEntry e = makeEntry(id, category);
    e.format = "builtin";
    return e;
}

// A retired built-in (an id this firmware no longer has) under Tools/LEDs
// is pruned by the merge, so the device shows Tools as Power Manager,
// Clock, LEDs (the folder appears at Flashlight). Arranging only Clock
// must give Clock, Power Manager, LEDs; the retired row stays in the file.
void test_arrange_retired_builtin_does_not_anchor_submenu(void) {
    Loadout l;
    l.entries.push_back(builtinEntry("boot-animation", "Screensavers"));
    l.entries.push_back(builtinEntry("booper",         "Games"));
    l.entries.push_back(builtinEntry("retired-led",    "Tools/LEDs"));
    l.entries.push_back(builtinEntry("power-manager",  "Tools"));
    l.entries.push_back(builtinEntry("clock",          "Tools"));
    l.entries.push_back(builtinEntry("snake",          "Games"));
    l.entries.push_back(builtinEntry("flashlight",     "Tools/LEDs"));
    l.entries.push_back(builtinEntry("accelerometer",  "Tools/LEDs"));
    l.entries.push_back(builtinEntry("music-player",   "Media"));
    const char* const before[] = { "power-manager", "clock", "[LEDs]" };
    assertKids(childrenOf(l, "Tools", kSeedApps, 9), before, 3);

    std::vector<ArrangeItem> order = { arr("clock") };
    TEST_ASSERT_TRUE(arrangeChecked(l, order, kSeedApps, 9));
    const char* const after[] = { "clock", "power-manager", "[LEDs]" };
    assertKids(childrenOf(l, "Tools", kSeedApps, 9), after, 3);
    TEST_ASSERT_EQUAL_INT(9, (int)l.entries.size());
    const char* ids[] = { "clock", "power-manager", "retired-led", "flashlight",
                          "accelerometer" };
    for (int i = 0; i < 5; i++) TEST_ASSERT_EQUAL_STRING(ids[i], l.entries[i].id.c_str());

    // The same arrange sent as an ops document reaches the same result.
    Loadout viaOps;
    viaOps.entries.push_back(builtinEntry("boot-animation", "Screensavers"));
    viaOps.entries.push_back(builtinEntry("retired-led",    "Tools/LEDs"));
    viaOps.entries.push_back(builtinEntry("power-manager",  "Tools"));
    viaOps.entries.push_back(builtinEntry("clock",          "Tools"));
    viaOps.entries.push_back(builtinEntry("flashlight",     "Tools/LEDs"));
    int applied = 0;
    TEST_ASSERT_TRUE(applyOps(viaOps,
        "{\"ops\":[{\"op\":\"arrange\",\"order\":[{\"id\":\"clock\"}]}]}",
        &applied, kSeedApps, 9));
    TEST_ASSERT_EQUAL_INT(1, applied);
    // This file omits Accelerometer; the merge shows it at the end of Tools
    // (first-segment fallback) and the arrange writes it in there.
    const char* const opsKids[] = { "clock", "power-manager", "[LEDs]", "accelerometer" };
    assertKids(childrenOf(viaOps, "Tools", kSeedApps, 9), opsKids, 4);
}

// A delivered app is shown only with its file: one without a file does not
// anchor the submenu; one with a file does (the folder appears there).
void test_arrange_delivered_app_anchors_only_with_file(void) {
    for (int withFile = 0; withFile < 2; withFile++) {
        Loadout l;
        l.entries.push_back(builtinEntry("boot-animation", "Screensavers"));
        LoadoutEntry app = makeEntry("my-led-app", "Tools/LEDs");
        app.format = "wasm";
        if (withFile) app.blobPath = "/apps/my-led-app-0123abcd.wasm";
        l.entries.push_back(app);
        l.entries.push_back(builtinEntry("power-manager", "Tools"));
        l.entries.push_back(builtinEntry("clock",         "Tools"));
        l.entries.push_back(builtinEntry("flashlight",    "Tools/LEDs"));
        std::vector<ArrangeItem> order = { arr("clock") };
        TEST_ASSERT_TRUE(arrangeChecked(l, order, kSeedApps, 9));
        // Accelerometer is not listed: the merge shows it last in Tools and
        // the arrange writes it in there.
        if (withFile) {
            const char* const want[] = { "clock", "[LEDs]", "power-manager", "accelerometer" };
            assertKids(childrenOf(l, "Tools", kSeedApps, 9), want, 4);
        } else {
            const char* const want[] = { "clock", "power-manager", "[LEDs]", "accelerometer" };
            assertKids(childrenOf(l, "Tools", kSeedApps, 9), want, 4);
        }
    }
}

// A hidden later duplicate must never overtake the row the merge uses.
// Saved order: retired-led [Tools/LEDs], power-manager [Tools/Other],
// clock [Tools/LEDs], clock [Tools/Other, hidden]. The merge shows the
// first clock under Tools/LEDs. Arranging an unrelated app (Booper) must
// keep Clock visible there; no app may disappear.
void test_arrange_hidden_duplicate_never_wins(void) {
    Loadout l;
    l.entries.push_back(builtinEntry("retired-led",   "Tools/LEDs"));
    l.entries.push_back(builtinEntry("power-manager", "Tools/Other"));
    l.entries.push_back(builtinEntry("clock",         "Tools/LEDs"));
    LoadoutEntry dup = builtinEntry("clock", "Tools/Other");
    dup.hidden = true;
    l.entries.push_back(dup);
    l.entries.push_back(builtinEntry("booper",        "Games"));

    auto clockRow = [](const Loadout& lo) -> const MergedApp* {
        static std::vector<MergedApp> merged;
        merged = mergeWithRegistry(lo, kSeedApps, 9);
        for (const auto& m : merged) {
            if (m.appIndex == 5) return &m; // clock in kSeedApps
        }
        return nullptr;
    };
    const MergedApp* before = clockRow(l);
    TEST_ASSERT_NOT_NULL(before);
    TEST_ASSERT_FALSE(before->hidden);
    TEST_ASSERT_EQUAL_STRING("Tools/LEDs", before->category.c_str());

    // arrangeChecked asserts the shown set is unchanged (nothing vanishes).
    std::vector<ArrangeItem> order = { arr("booper") };
    TEST_ASSERT_TRUE(arrangeChecked(l, order, kSeedApps, 9));
    const MergedApp* after = clockRow(l);
    TEST_ASSERT_NOT_NULL(after);
    TEST_ASSERT_FALSE(after->hidden);
    TEST_ASSERT_EQUAL_STRING("Tools/LEDs", after->category.c_str());
    // The duplicate row is gone from the file; the first one stays.
    int clocks = 0;
    for (const auto& e : l.entries) if (e.id == "clock") clocks++;
    TEST_ASSERT_EQUAL_INT(1, clocks);
    // 4 rows kept + the 5 compiled apps the file did not list, written in.
    TEST_ASSERT_EQUAL_INT(9, (int)l.entries.size());

    // Same without a registry: the duplicate drop does not depend on it.
    Loadout n;
    n.entries.push_back(builtinEntry("clock", "Tools/LEDs"));
    LoadoutEntry d2 = builtinEntry("clock", "Tools/Other");
    d2.hidden = true;
    n.entries.push_back(d2);
    TEST_ASSERT_TRUE(applyArrange(n, order));
    TEST_ASSERT_EQUAL_INT(1, (int)n.entries.size());
    TEST_ASSERT_FALSE(n.entries[0].hidden);
}

// Delivered apps that share an id are all kept by the merge, so the
// arrange keeps them all too.
void test_arrange_keeps_delivered_apps_with_same_id(void) {
    Loadout l;
    for (int i = 0; i < 2; i++) {
        LoadoutEntry e = makeEntry("timer", "Tools");
        e.format   = "wasm";
        e.blobPath = i ? "/apps/timer-b.wasm" : "/apps/timer-a.wasm";
        l.entries.push_back(e);
    }
    l.entries.push_back(builtinEntry("clock", "Tools"));
    std::vector<ArrangeItem> order = { arr("clock") };
    TEST_ASSERT_TRUE(arrangeChecked(l, order, kSeedApps, 9));
    // Both delivered apps + clock + the 7 unlisted compiled apps written in.
    TEST_ASSERT_EQUAL_INT(10, (int)l.entries.size());
    int timers = 0;
    for (const auto& e : l.entries) if (e.id == "timer") timers++;
    TEST_ASSERT_EQUAL_INT(2, timers);
}

// An explicit nested order (Clock, LEDs folder, Power Manager) survives
// arrange -> serialize -> parse, so the menu rebuilds the same way.
void test_nested_sibling_order_roundtrip(void) {
    Loadout l = buildFromRegistry(kSeedApps, 9);
    std::vector<ArrangeItem> order = {
        arrCat("clock",         "Tools"),
        arrCat("flashlight",    "Tools/LEDs"),
        arrCat("accelerometer", "Tools/LEDs"),
        arrCat("power-manager", "Tools"),
    };
    TEST_ASSERT_TRUE(arrangeChecked(l, order));
    Loadout back;
    TEST_ASSERT_TRUE(parseManifest(serializeManifest(l).c_str(), back));
    const char* const want[] = { "clock", "[LEDs]", "power-manager" };
    assertKids(childrenOf(back, "Tools"), want, 3);
    const char* ids[] = { "clock", "flashlight", "accelerometer", "power-manager" };
    for (int i = 0; i < 4; i++) TEST_ASSERT_EQUAL_STRING(ids[i], back.entries[i].id.c_str());
    // A second arrange of the same order changes nothing.
    TEST_ASSERT_TRUE(arrangeChecked(back, order));
    for (int i = 0; i < 4; i++) TEST_ASSERT_EQUAL_STRING(ids[i], back.entries[i].id.c_str());
}

// Reference for the old rule: each exact category string one contiguous
// run, by first appearance, relative order kept.
static std::vector<std::string> exactGrouping(const std::vector<LoadoutEntry>& in) {
    std::vector<std::string> ids;
    std::vector<bool> placed(in.size(), false);
    for (size_t i = 0; i < in.size(); i++) {
        if (placed[i]) continue;
        for (size_t j = i; j < in.size(); j++) {
            if (!placed[j] && in[j].category == in[i].category) {
                placed[j] = true;
                ids.push_back(in[j].id);
            }
        }
    }
    return ids;
}

// Root entries keep their own places among the folders, as the device
// shows them (compiled order: Status, the Settings folder, Check for
// updates). Arranging an unrelated app leaves that alone; the earlier rule
// gathered the root entries into one run and moved Check above Settings.
void test_arrange_root_entries_keep_places(void) {
    Loadout l;
    l.entries.push_back(makeEntry("status", ""));
    l.entries.push_back(makeEntry("link",   "Settings"));
    l.entries.push_back(makeEntry("check",  ""));
    l.entries.push_back(makeEntry("booper", "Games"));
    std::vector<ArrangeItem> order = { arr("booper") };
    TEST_ASSERT_TRUE(arrangeChecked(l, order));
    const char* ids[] = { "booper", "status", "link", "check" };
    for (int i = 0; i < 4; i++) TEST_ASSERT_EQUAL_STRING(ids[i], l.entries[i].id.c_str());
}

// With single-level categories (every file earlier firmware wrote) the
// arrange normalization is exactly the old exact-category grouping. (Root
// entries are the exception, see test_arrange_root_entries_keep_places;
// s1/r2 were root entries here before that rule changed.)
void test_arrange_flat_categories_match_old_grouping(void) {
    Loadout l;
    l.entries.push_back(makeEntry("s1", "Misc"));
    l.entries.push_back(makeEntry("t1", "Tools"));
    l.entries.push_back(makeEntry("g1", "Games"));
    l.entries.push_back(makeEntry("s2", "Settings"));
    l.entries.push_back(makeEntry("r2", "Misc"));
    l.entries.push_back(makeEntry("t2", "Tools"));
    l.entries.push_back(makeEntry("g2", "Games"));
    l.entries.push_back(makeEntry("s3", "Settings"));
    const char* orders[][8] = {
        { "t2", "s1", "g1", "r2", "t1", "s3", "g2", "s2" },
        { "g2", "t1", "s1", "s2", "g1", "r2", "t2", "s3" },
        { "r2", "s3", "t1", "g2", "s1", "t2", "g1", "s2" },
    };
    for (const auto& o : orders) {
        Loadout work = l;
        std::vector<LoadoutEntry> input;
        std::vector<ArrangeItem> order;
        for (const char* id : o) {
            order.push_back(arr(id));
            for (const auto& e : l.entries) if (e.id == id) input.push_back(e);
        }
        TEST_ASSERT_TRUE(arrangeChecked(work, order));
        std::vector<std::string> want = exactGrouping(input);
        TEST_ASSERT_EQUAL_INT((int)want.size(), (int)work.entries.size());
        for (size_t i = 0; i < want.size(); i++)
            TEST_ASSERT_EQUAL_STRING(want[i].c_str(), work.entries[i].id.c_str());
        assertContiguous(work);
    }
}

// First write on a device with no saved menu: the seed comes from the
// compiled registry (nested paths, in the real compiled interleaving), then
// an unrelated one-item arrange lands. Both LED apps stay in "Tools/LEDs"
// and every other entry keeps its section and relative order.
void test_arrange_seeded_nested_kept(void) {
    Loadout l = buildFromRegistry(kSeedApps, 9);
    std::vector<ArrangeItem> order = { arr("snake") };
    TEST_ASSERT_TRUE(arrangeChecked(l, order));

    // Arrange semantics: the named item leads, the rest follow in seed
    // order, then sections are made contiguous by first appearance.
    const char* ids[]  = { "snake", "booper", "boot-animation", "flashlight",
                           "accelerometer", "power-manager", "clock",
                           "music-player" };
    const char* cats[] = { "Games", "Games", "Screensavers", "Tools/LEDs",
                           "Tools/LEDs", "Tools", "Tools", "Media" };
    TEST_ASSERT_EQUAL_INT(8, (int)l.entries.size());
    for (int i = 0; i < 8; i++) {
        TEST_ASSERT_EQUAL_STRING(ids[i],  l.entries[i].id.c_str());
        TEST_ASSERT_EQUAL_STRING(cats[i], l.entries[i].category.c_str());
    }
    assertContiguous(l);
    assertPositionsRenumbered(l);
}

// The same arrange on a menu saved by earlier firmware (flattened "Tools"
// for the LED apps) keeps "Tools": nothing is re-nested behind the user.
void test_arrange_flattened_saved_menu_unchanged(void) {
    Loadout l;
    l.entries.push_back(makeEntry("booper",        "Games"));
    l.entries.push_back(makeEntry("flashlight",    "Tools"));
    l.entries.push_back(makeEntry("power-manager", "Tools"));
    l.entries.push_back(makeEntry("accelerometer", "Tools"));
    l.entries.push_back(makeEntry("snake",         "Games"));
    std::vector<ArrangeItem> order = { arr("booper") };
    TEST_ASSERT_TRUE(arrangeChecked(l, order));
    const char* ids[] = { "booper", "snake", "flashlight", "power-manager",
                          "accelerometer" };
    for (int i = 0; i < 5; i++) {
        TEST_ASSERT_EQUAL_STRING(ids[i], l.entries[i].id.c_str());
        TEST_ASSERT_EQUAL_STRING(i < 2 ? "Games" : "Tools",
                                 l.entries[i].category.c_str());
    }
}

void test_op_sequence_keeps_contiguity(void) {
    Loadout l = makeBaseline();
    TEST_ASSERT_TRUE(applyAdd(l, makeEntry("APP_E", "Media")));
    TEST_ASSERT_TRUE(applyAdd(l, makeEntry("APP_F", "Games")));
    TEST_ASSERT_TRUE(applyRemove(l, "APP_A"));
    TEST_ASSERT_TRUE(applyHide(l, "APP_D", true));
    std::vector<ArrangeItem> order = {
        arr("APP_E"), arr("APP_C"), arr("APP_D"), arr("APP_F"), arr("APP_B"),
    };
    TEST_ASSERT_TRUE(arrangeChecked(l, order));
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

void test_change_of_hands_removes_non_builtin_entries(void) {
    Loadout l = makeBaseline();
    for (auto& entry : l.entries) entry.format = "builtin";
    LoadoutEntry delivered = makeEntry("delivered", "Games");
    delivered.format = "wasm";
    delivered.blobPath = "/apps/delivered-1234abcd.wasm";
    TEST_ASSERT_TRUE(applyAdd(l, delivered));
    LoadoutEntry other = makeEntry("other", "Tools");
    other.format = "blob";
    other.blobPath = "/apps/other.wasm";
    TEST_ASSERT_TRUE(applyAdd(l, other));
    const auto paths = removeNonBuiltin(l);
    TEST_ASSERT_EQUAL_INT(2, (int)paths.size());
    TEST_ASSERT_EQUAL_STRING(delivered.blobPath.c_str(), paths[0].c_str());
    TEST_ASSERT_EQUAL_STRING(other.blobPath.c_str(), paths[1].c_str());
    TEST_ASSERT_EQUAL_INT(4, (int)l.entries.size());
    for (const auto& entry : l.entries) TEST_ASSERT_EQUAL_STRING("builtin", entry.format.c_str());
    assertPositionsRenumbered(l);
}

void test_change_of_hands_keeps_empty_format_and_missing_blob(void) {
    Loadout l = makeBaseline();
    for (auto& entry : l.entries) entry.format = "builtin";
    LoadoutEntry legacy = makeEntry("legacy", "Games");
    legacy.format = "";
    legacy.blobPath = "/apps/legacy.wasm";
    TEST_ASSERT_TRUE(applyAdd(l, legacy));
    LoadoutEntry incomplete = makeEntry("incomplete", "Games");
    incomplete.format = "wasm";
    TEST_ASSERT_TRUE(applyAdd(l, incomplete));
    const auto paths = removeNonBuiltin(l);
    TEST_ASSERT_TRUE(paths.empty());
    TEST_ASSERT_EQUAL_INT(6, (int)l.entries.size());
    bool foundLegacy = false, foundIncomplete = false;
    for (const auto& entry : l.entries) {
        if (entry.id == "legacy") foundLegacy = true;
        if (entry.id == "incomplete") foundIncomplete = true;
    }
    TEST_ASSERT_TRUE(foundLegacy);
    TEST_ASSERT_TRUE(foundIncomplete);
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
    RUN_TEST(test_arrange_seeded_nested_kept);
    RUN_TEST(test_arrange_flattened_saved_menu_unchanged);
    RUN_TEST(test_arrange_seeded_only_clock_keeps_leds_place);
    RUN_TEST(test_nested_sibling_order_roundtrip);
    RUN_TEST(test_arrange_hidden_first_entry_keeps_visible_order);
    RUN_TEST(test_arrange_all_hidden_submenu_at_first_entry);
    RUN_TEST(test_arrange_very_deep_category_is_bounded);
    RUN_TEST(test_arrange_retired_builtin_does_not_anchor_submenu);
    RUN_TEST(test_arrange_delivered_app_anchors_only_with_file);
    RUN_TEST(test_arrange_hidden_duplicate_never_wins);
    RUN_TEST(test_arrange_keeps_delivered_apps_with_same_id);
    RUN_TEST(test_arrange_flat_categories_match_old_grouping);
    RUN_TEST(test_arrange_root_entries_keep_places);
    RUN_TEST(test_op_sequence_keeps_contiguity);
    RUN_TEST(test_change_of_hands_removes_non_builtin_entries);
    RUN_TEST(test_change_of_hands_keeps_empty_format_and_missing_blob);
    return UNITY_END();
}
