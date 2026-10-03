// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

// test/test_sync_ops/test_ops.cpp
//
// Manifest-apply merge logic for the serial-sync write direction:
// LoadoutManifest::applyOps parses a staged-ops document (adds / removes /
// hides + one declarative arrange — the shared staged-change vocabulary) and
// applies it atomically. A malformed document or a rejected op must leave the loadout
// untouched (never half-applied), which is the manifest-level mirror of the
// transport's checksum rejection.

#include <string>

#include <unity.h>
#include "LoadoutManifest.h"

using namespace LoadoutManifest;

static LoadoutEntry makeEntry(const char* id, const char* category,
                              bool hidden = false) {
    LoadoutEntry e;
    e.id       = id;
    e.name     = id;
    e.category = category;
    e.hidden   = hidden;
    return e;
}

// Games: A, B ; Tools: C, D.
static Loadout makeBaseline(void) {
    Loadout l;
    l.entries.push_back(makeEntry("APP_A", "Games"));
    l.entries.push_back(makeEntry("APP_B", "Games"));
    l.entries.push_back(makeEntry("APP_C", "Tools"));
    l.entries.push_back(makeEntry("APP_D", "Tools"));
    for (int i = 0; i < (int)l.entries.size(); i++) l.entries[i].position = i;
    return l;
}

static void assertContiguous(const Loadout& l) {
    std::vector<std::string> seen;
    for (size_t i = 0; i < l.entries.size(); i++) {
        const std::string& cat = l.entries[i].category;
        if (i > 0 && l.entries[i - 1].category == cat) continue;
        for (const auto& s : seen) {
            if (s == cat) TEST_FAIL_MESSAGE("category section is not contiguous");
        }
        seen.push_back(cat);
    }
}

static int indexOf(const Loadout& l, const char* id) {
    for (int i = 0; i < (int)l.entries.size(); i++) {
        if (l.entries[i].id == id) return i;
    }
    return -1;
}

// ---------- single ops ----------

void test_ops_add(void) {
    Loadout l = makeBaseline();
    int applied = -1;
    const char* doc =
        "{\"ops\":[{\"op\":\"add\",\"entry\":"
        "{\"id\":\"APP_E\",\"name\":\"E\",\"category\":\"Games\","
        "\"format\":\"blob\",\"blobPath\":\"/apps/e.wasm\"}}]}";
    TEST_ASSERT_TRUE(applyOps(l, doc, &applied));
    TEST_ASSERT_EQUAL_INT(1, applied);
    int idx = indexOf(l, "APP_E");
    TEST_ASSERT_NOT_EQUAL(-1, idx);
    // Reserved fields round-trip through the add.
    TEST_ASSERT_EQUAL_STRING("blob", l.entries[idx].format.c_str());
    TEST_ASSERT_EQUAL_STRING("/apps/e.wasm", l.entries[idx].blobPath.c_str());
    TEST_ASSERT_EQUAL_STRING("Games", l.entries[idx].category.c_str());
    assertContiguous(l);
}

void test_ops_remove_and_hide(void) {
    Loadout l = makeBaseline();
    const char* doc =
        "{\"ops\":["
        "{\"op\":\"remove\",\"id\":\"APP_B\"},"
        "{\"op\":\"hide\",\"id\":\"APP_C\",\"hidden\":true}]}";
    int applied = 0;
    TEST_ASSERT_TRUE(applyOps(l, doc, &applied));
    TEST_ASSERT_EQUAL_INT(2, applied);
    TEST_ASSERT_EQUAL_INT(-1, indexOf(l, "APP_B"));
    TEST_ASSERT_TRUE(l.entries[indexOf(l, "APP_C")].hidden);
}

void test_ops_arrange_with_category_move(void) {
    Loadout l = makeBaseline();
    // Move APP_A into Tools via the arrange op's category rewrite.
    const char* doc =
        "{\"ops\":[{\"op\":\"arrange\",\"order\":["
        "{\"id\":\"APP_B\"},"
        "{\"id\":\"APP_C\"},"
        "{\"id\":\"APP_A\",\"category\":\"Tools\"},"
        "{\"id\":\"APP_D\"}]}]}";
    TEST_ASSERT_TRUE(applyOps(l, doc, nullptr));
    TEST_ASSERT_EQUAL_STRING("Tools", l.entries[indexOf(l, "APP_A")].category.c_str());
    assertContiguous(l);
}

// ---------- compound + ordering ----------

void test_ops_compound_session(void) {
    Loadout l = makeBaseline();
    const char* doc =
        "{\"ops\":["
        "{\"op\":\"add\",\"entry\":{\"id\":\"APP_E\",\"category\":\"Media\"}},"
        "{\"op\":\"remove\",\"id\":\"APP_A\"},"
        "{\"op\":\"hide\",\"id\":\"APP_D\",\"hidden\":true},"
        "{\"op\":\"arrange\",\"order\":["
        "{\"id\":\"APP_E\"},{\"id\":\"APP_C\"},{\"id\":\"APP_D\"},{\"id\":\"APP_B\"}]}"
        "]}";
    int applied = 0;
    TEST_ASSERT_TRUE(applyOps(l, doc, &applied));
    TEST_ASSERT_EQUAL_INT(4, applied);
    TEST_ASSERT_EQUAL_INT(4, (int)l.entries.size()); // A removed, E added
    TEST_ASSERT_EQUAL_INT(-1, indexOf(l, "APP_A"));
    TEST_ASSERT_TRUE(l.entries[indexOf(l, "APP_D")].hidden);
    assertContiguous(l);
    for (int i = 0; i < (int)l.entries.size(); i++)
        TEST_ASSERT_EQUAL_INT(i, l.entries[i].position);
}

void test_ops_empty_document_is_noop(void) {
    Loadout l = makeBaseline();
    int applied = -1;
    TEST_ASSERT_TRUE(applyOps(l, "{\"ops\":[]}", &applied));
    TEST_ASSERT_EQUAL_INT(0, applied);
    TEST_ASSERT_EQUAL_INT(4, (int)l.entries.size());
    // A document with no ops key at all is also a valid no-op.
    TEST_ASSERT_TRUE(applyOps(l, "{}", &applied));
    TEST_ASSERT_EQUAL_INT(0, applied);
}

// ---------- atomicity: reject, never half-apply ----------

void test_ops_malformed_json_untouched(void) {
    Loadout l = makeBaseline();
    TEST_ASSERT_FALSE(applyOps(l, "{\"ops\":[", nullptr));       // truncated
    TEST_ASSERT_FALSE(applyOps(l, "not json", nullptr));
    TEST_ASSERT_FALSE(applyOps(l, nullptr, nullptr));
    TEST_ASSERT_FALSE(applyOps(l, "{\"ops\":[]} trailing", nullptr));
    TEST_ASSERT_EQUAL_INT(4, (int)l.entries.size());            // unchanged
    TEST_ASSERT_EQUAL_STRING("APP_A", l.entries[0].id.c_str());
}

void test_ops_rejected_op_rolls_back_whole_document(void) {
    Loadout l = makeBaseline();
    // First op (add APP_E) would succeed; second op (remove a nonexistent id)
    // fails, so the entire document must roll back — APP_E must NOT appear.
    const char* doc =
        "{\"ops\":["
        "{\"op\":\"add\",\"entry\":{\"id\":\"APP_E\",\"category\":\"Games\"}},"
        "{\"op\":\"remove\",\"id\":\"APP_NOPE\"}]}";
    TEST_ASSERT_FALSE(applyOps(l, doc, nullptr));
    TEST_ASSERT_EQUAL_INT(-1, indexOf(l, "APP_E")); // not half-applied
    TEST_ASSERT_EQUAL_INT(4, (int)l.entries.size());
}

void test_ops_unknown_op_and_duplicate_add_rejected(void) {
    Loadout l = makeBaseline();
    TEST_ASSERT_FALSE(applyOps(l, "{\"ops\":[{\"op\":\"frobnicate\"}]}", nullptr));
    // Duplicate id add is rejected by the underlying applyAdd.
    TEST_ASSERT_FALSE(applyOps(l,
        "{\"ops\":[{\"op\":\"add\",\"entry\":{\"id\":\"APP_A\",\"category\":\"X\"}}]}",
        nullptr));
    TEST_ASSERT_EQUAL_INT(4, (int)l.entries.size());
}

void test_ops_missing_required_field_rejected(void) {
    Loadout l = makeBaseline();
    TEST_ASSERT_FALSE(applyOps(l, "{\"ops\":[{\"op\":\"add\"}]}", nullptr));       // no entry
    TEST_ASSERT_FALSE(applyOps(l, "{\"ops\":[{\"op\":\"remove\"}]}", nullptr));    // no id
    TEST_ASSERT_FALSE(applyOps(l, "{\"ops\":[{\"op\":\"arrange\"}]}", nullptr));   // no order
    TEST_ASSERT_EQUAL_INT(4, (int)l.entries.size());
}

// The lapply payload reaches skipValue via applyOneOp's unknown-field branch,
// so an ops document with a hostile deep-nested value in an entry must be
// rejected cleanly (no stack overflow) and leave the loadout untouched — the
// wire-reachable mirror of the parseManifest depth test.
void test_ops_deeply_nested_unknown_field_rejected(void) {
    Loadout l = makeBaseline();
    std::string doc =
        "{\"ops\":[{\"op\":\"add\",\"entry\":"
        "{\"id\":\"APP_E\",\"category\":\"Games\",\"deep\":";
    const int kDepth = 500; // well past the ~16-level cap
    for (int i = 0; i < kDepth; i++) doc += "[";
    for (int i = 0; i < kDepth; i++) doc += "]";
    doc += "}}]}";
    TEST_ASSERT_FALSE(applyOps(l, doc.c_str(), nullptr));
    TEST_ASSERT_EQUAL_INT(4, (int)l.entries.size());   // untouched
    TEST_ASSERT_EQUAL_INT(-1, indexOf(l, "APP_E"));    // not half-applied
}

void test_ops_shallow_nested_unknown_field_applies(void) {
    // A shallow unknown field inside an op is skipped, and the op still applies.
    Loadout l = makeBaseline();
    const char* doc =
        "{\"ops\":[{\"op\":\"add\",\"future\":[1,[2,{\"a\":true}]],"
        "\"entry\":{\"id\":\"APP_E\",\"category\":\"Games\"}}]}";
    int applied = 0;
    TEST_ASSERT_TRUE(applyOps(l, doc, &applied));
    TEST_ASSERT_EQUAL_INT(1, applied);
    TEST_ASSERT_NOT_EQUAL(-1, indexOf(l, "APP_E"));
}

// ---------- batch contract: batch / base fields and the replace op ----------

void test_ops_batch_and_base_fields_accepted(void) {
    Loadout l = makeBaseline();
    const char* doc =
        "{\"batch\":\"b-1\",\"base\":\"0a1B2c3D\","
        "\"ops\":[{\"op\":\"hide\",\"id\":\"APP_A\",\"hidden\":true}]}";
    int applied = 0;
    TEST_ASSERT_TRUE(applyOps(l, doc, &applied));
    TEST_ASSERT_EQUAL_INT(1, applied);
    TEST_ASSERT_TRUE(l.entries[0].hidden);

    OpsMeta meta;
    TEST_ASSERT_TRUE(parseOpsMeta(doc, meta));
    TEST_ASSERT_TRUE(meta.hasBatch);
    TEST_ASSERT_EQUAL_STRING("b-1", meta.batch.c_str());
    TEST_ASSERT_TRUE(meta.hasBase);
    TEST_ASSERT_EQUAL_UINT32(0x0a1b2c3du, meta.base);

    OpsMeta none;
    TEST_ASSERT_TRUE(parseOpsMeta("{\"ops\":[]}", none));
    TEST_ASSERT_FALSE(none.hasBatch);
    TEST_ASSERT_FALSE(none.hasBase);
}

void test_ops_invalid_batch_or_base_rejects_document(void) {
    const char* bad[] = {
        "{\"batch\":\"\",\"ops\":[{\"op\":\"remove\",\"id\":\"APP_A\"}]}",
        "{\"batch\":\"has space\",\"ops\":[{\"op\":\"remove\",\"id\":\"APP_A\"}]}",
        "{\"batch\":\"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\",\"ops\":[{\"op\":\"remove\",\"id\":\"APP_A\"}]}",
        "{\"batch\":1,\"ops\":[{\"op\":\"remove\",\"id\":\"APP_A\"}]}",
        "{\"base\":\"\",\"ops\":[{\"op\":\"remove\",\"id\":\"APP_A\"}]}",
        "{\"base\":\"12g45678\",\"ops\":[{\"op\":\"remove\",\"id\":\"APP_A\"}]}",
        "{\"base\":\"123456789\",\"ops\":[{\"op\":\"remove\",\"id\":\"APP_A\"}]}",
        "{\"base\":12345678,\"ops\":[{\"op\":\"remove\",\"id\":\"APP_A\"}]}",
    };
    for (const char* doc : bad) {
        Loadout l = makeBaseline();
        TEST_ASSERT_FALSE_MESSAGE(applyOps(l, doc, nullptr), doc);
        TEST_ASSERT_EQUAL_INT(4, (int)l.entries.size());
        OpsMeta meta;
        TEST_ASSERT_FALSE_MESSAGE(parseOpsMeta(doc, meta), doc);
    }
    // Exactly 40 bytes is the longest accepted id.
    Loadout l = makeBaseline();
    TEST_ASSERT_TRUE(applyOps(l,
        "{\"batch\":\"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\",\"ops\":[]}", nullptr));
}

void test_ops_replace_swaps_blob_fields_only(void) {
    Loadout l = makeBaseline();
    l.entries[1].format    = "wasm";
    l.entries[1].blobPath  = "/apps/APP_B-00000001.wasm";
    l.entries[1].version   = "1";
    l.entries[1].abi       = "1";
    l.entries[1].signature = "sig";
    l.entries[1].hidden    = true;
    const char* doc =
        "{\"ops\":[{\"op\":\"replace\",\"entry\":{\"id\":\"APP_B\",\"name\":\"New B\","
        "\"category\":\"Tools\",\"hidden\":false,\"position\":3,\"format\":\"blob\","
        "\"blobPath\":\"/apps/APP_B-00000002.wasm\",\"version\":\"2\",\"abi\":\"3\"}}]}";
    int applied = 0;
    TEST_ASSERT_TRUE(applyOps(l, doc, &applied));
    TEST_ASSERT_EQUAL_INT(1, applied);
    TEST_ASSERT_EQUAL_INT(1, indexOf(l, "APP_B"));
    const LoadoutEntry& e = l.entries[1];
    TEST_ASSERT_EQUAL_STRING("/apps/APP_B-00000002.wasm", e.blobPath.c_str());
    TEST_ASSERT_EQUAL_STRING("2", e.version.c_str());
    TEST_ASSERT_EQUAL_STRING("3", e.abi.c_str());
    TEST_ASSERT_EQUAL_STRING("New B", e.name.c_str());
    TEST_ASSERT_EQUAL_STRING("Games", e.category.c_str());
    TEST_ASSERT_TRUE(e.hidden);
    TEST_ASSERT_EQUAL_STRING("wasm", e.format.c_str());
    // The old blob's signature must not follow a new blob.
    TEST_ASSERT_EQUAL_STRING("", e.signature.c_str());
    TEST_ASSERT_EQUAL_INT(4, (int)l.entries.size());

    const char* signedDoc =
        "{\"ops\":[{\"op\":\"replace\",\"entry\":{\"id\":\"APP_B\","
        "\"blobPath\":\"/apps/APP_B-00000003.wasm\",\"signature\":\"sig3\"}}]}";
    TEST_ASSERT_TRUE(applyOps(l, signedDoc, &applied));
    TEST_ASSERT_EQUAL_STRING("sig3", l.entries[1].signature.c_str());
}

void test_ops_replace_unknown_or_blobless_rejected(void) {
    const char* bad[] = {
        "{\"ops\":[{\"op\":\"hide\",\"id\":\"APP_A\",\"hidden\":true},"
        "{\"op\":\"replace\",\"entry\":{\"id\":\"APP_Z\",\"blobPath\":\"/apps/z-00000000.wasm\"}}]}",
        "{\"ops\":[{\"op\":\"replace\",\"entry\":{\"id\":\"APP_A\"}}]}",
        "{\"ops\":[{\"op\":\"replace\",\"id\":\"APP_A\"}]}",
    };
    for (const char* doc : bad) {
        Loadout l = makeBaseline();
        TEST_ASSERT_FALSE_MESSAGE(applyOps(l, doc, nullptr), doc);
        TEST_ASSERT_FALSE(l.entries[0].hidden);
        TEST_ASSERT_EQUAL_INT(4, (int)l.entries.size());
    }
}

void test_ops_replace_only_on_delivered_blob_entries(void) {
    // Built-ins and any kind the firmware does not deliver (e.g. a sprite
    // pack) have no file to swap.
    const char* formats[] = { "builtin", "", "spritepack" };
    for (const char* f : formats) {
        Loadout l = makeBaseline();
        l.entries[0].format = f;
        l.entries[0].blobPath = "/apps/APP_A.bin";
        TEST_ASSERT_FALSE_MESSAGE(applyOps(l,
            "{\"ops\":[{\"op\":\"replace\",\"entry\":{\"id\":\"APP_A\","
            "\"blobPath\":\"/apps/APP_A-00000001.wasm\"}}]}", nullptr), f);
        TEST_ASSERT_EQUAL_STRING("/apps/APP_A.bin", l.entries[0].blobPath.c_str());
    }
    // A data screensaver ("cfsprite") swaps its drawing the same way.
    const char* ok[] = { "wasm", "blob", "cfsprite" };
    for (const char* f : ok) {
        Loadout l = makeBaseline();
        l.entries[0].format = f;
        TEST_ASSERT_TRUE_MESSAGE(applyOps(l,
            "{\"ops\":[{\"op\":\"replace\",\"entry\":{\"id\":\"APP_A\","
            "\"blobPath\":\"/apps/APP_A-00000001.wasm\"}}]}", nullptr), f);
    }
}

// Replacing a data screensaver swaps its drawing file; the entry stays a
// data screensaver in its place.
void test_ops_replace_swaps_data_screensaver_file(void) {
    Loadout l = makeBaseline();
    l.entries[1].format = "cfsprite";
    l.entries[1].blobPath = "/assets/ss/APP_B-0123abcd.cfs";
    l.entries[1].name = "Old";
    TEST_ASSERT_TRUE(applyOps(l,
        "{\"ops\":[{\"op\":\"replace\",\"entry\":{\"id\":\"APP_B\",\"name\":\"New\","
        "\"blobPath\":\"/assets/ss/APP_B-89abcdef.cfs\"}}]}", nullptr));
    TEST_ASSERT_EQUAL_STRING("/assets/ss/APP_B-89abcdef.cfs", l.entries[1].blobPath.c_str());
    TEST_ASSERT_EQUAL_STRING("cfsprite", l.entries[1].format.c_str());
    TEST_ASSERT_EQUAL_STRING("New", l.entries[1].name.c_str());
    TEST_ASSERT_EQUAL_STRING("Games", l.entries[1].category.c_str());
    TEST_ASSERT_EQUAL_INT(1, l.entries[1].position);
}

void test_collect_op_blob_paths(void) {
    std::vector<std::string> paths;
    TEST_ASSERT_TRUE(collectOpBlobPaths(
        "{\"batch\":\"b\",\"ops\":["
        "{\"op\":\"add\",\"entry\":{\"id\":\"A\",\"blobPath\":\"/apps/a.wasm\"}},"
        "{\"op\":\"hide\",\"id\":\"A\",\"hidden\":true},"
        "{\"op\":\"add\",\"entry\":{\"id\":\"B\"}},"
        "{\"entry\":{\"id\":\"C\",\"blobPath\":\"/x\"},\"op\":\"replace\"}]}", paths));
    TEST_ASSERT_EQUAL_INT(2, (int)paths.size());
    TEST_ASSERT_EQUAL_STRING("/apps/a.wasm", paths[0].c_str());
    TEST_ASSERT_EQUAL_STRING("/x", paths[1].c_str());
    TEST_ASSERT_TRUE(collectOpBlobPaths("{}", paths));
    TEST_ASSERT_EQUAL_INT(0, (int)paths.size());
    TEST_ASSERT_FALSE(collectOpBlobPaths("{\"ops\":[", paths));
}

void test_applied_record_round_trip(void) {
    AppliedRecord rec;
    rec.batch = "b-\"q\"";
    rec.result = "applied";
    rec.crcAfter = 0x00c0ffeeu;
    rec.at = 1790000000u;
    rec.ops = 3;
    rec.entries = 12;
    rec.docCrc = 0x0000abcdu;
    const std::string json = serializeAppliedRecord(rec);
    TEST_ASSERT_EQUAL_STRING(
        "{\"batch\":\"b-\\\"q\\\"\",\"result\":\"applied\",\"crc_after\":\"00c0ffee\","
        "\"at\":1790000000,\"ops\":3,\"entries\":12,\"doc_crc\":\"0000abcd\"}\n",
        json.c_str());
    AppliedRecord back;
    TEST_ASSERT_TRUE(parseAppliedRecord(json.c_str(), back));
    TEST_ASSERT_EQUAL_STRING(rec.batch.c_str(), back.batch.c_str());
    TEST_ASSERT_EQUAL_STRING("applied", back.result.c_str());
    TEST_ASSERT_EQUAL_UINT32(rec.crcAfter, back.crcAfter);
    TEST_ASSERT_EQUAL_UINT32(rec.at, back.at);
    TEST_ASSERT_EQUAL_INT(3, back.ops);
    TEST_ASSERT_EQUAL_INT(12, back.entries);
    TEST_ASSERT_EQUAL_UINT32(0x0000abcdu, back.docCrc);

    AppliedRecord junk;
    TEST_ASSERT_FALSE(parseAppliedRecord("{\"result\":\"applied\"}", junk));
    TEST_ASSERT_FALSE(parseAppliedRecord("{\"batch\":\"b\"", junk));
    TEST_ASSERT_FALSE(parseAppliedRecord("", junk));
}

void setUp(void)    {}
void tearDown(void) {}

int main(int /*argc*/, char** /*argv*/) {
    UNITY_BEGIN();
    RUN_TEST(test_ops_add);
    RUN_TEST(test_ops_remove_and_hide);
    RUN_TEST(test_ops_arrange_with_category_move);
    RUN_TEST(test_ops_compound_session);
    RUN_TEST(test_ops_empty_document_is_noop);
    RUN_TEST(test_ops_malformed_json_untouched);
    RUN_TEST(test_ops_rejected_op_rolls_back_whole_document);
    RUN_TEST(test_ops_unknown_op_and_duplicate_add_rejected);
    RUN_TEST(test_ops_missing_required_field_rejected);
    RUN_TEST(test_ops_deeply_nested_unknown_field_rejected);
    RUN_TEST(test_ops_shallow_nested_unknown_field_applies);
    RUN_TEST(test_ops_batch_and_base_fields_accepted);
    RUN_TEST(test_ops_invalid_batch_or_base_rejects_document);
    RUN_TEST(test_ops_replace_swaps_blob_fields_only);
    RUN_TEST(test_ops_replace_unknown_or_blobless_rejected);
    RUN_TEST(test_ops_replace_only_on_delivered_blob_entries);
    RUN_TEST(test_ops_replace_swaps_data_screensaver_file);
    RUN_TEST(test_collect_op_blob_paths);
    RUN_TEST(test_applied_record_round_trip);
    return UNITY_END();
}
