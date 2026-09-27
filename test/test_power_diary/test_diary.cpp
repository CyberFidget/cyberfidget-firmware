#include <unity.h>

#include "../../lib/BatteryDiary/BatteryDiary.h"
#include "../../lib/BatteryDiary/UsageUpload.h"

using namespace BatteryDiary;

void setUp() {}
void tearDown() {}

static void test_codec_round_trip_all_events_and_clamps() {
    for (uint8_t event = BOOT; event <= FLUSH_MARKER; ++event) {
        Record source = makeRecord(0x10203040U + event, 0x50607080U + event,
                                   (int16_t)(3000 + event), 5025, -1275,
                                   (Event)event, 0xA5);
        uint8_t encoded[kRecordSize];
        encode(source, encoded);
        TEST_ASSERT_TRUE(validate(encoded));
        Record decoded;
        TEST_ASSERT_TRUE(decode(encoded, &decoded));
        TEST_ASSERT_EQUAL_UINT32(source.seq, decoded.seq);
        TEST_ASSERT_EQUAL_UINT32(source.uptime_or_count, decoded.uptime_or_count);
        TEST_ASSERT_EQUAL_INT16(source.vcell_mv, decoded.vcell_mv);
        TEST_ASSERT_EQUAL_UINT8(101, decoded.soc_half_pct);
        TEST_ASSERT_EQUAL_INT8(-51, decoded.crate_qtr_pct_hr);
        TEST_ASSERT_EQUAL_UINT8(event, decoded.event);
        TEST_ASSERT_EQUAL_UINT8(0xA5, decoded.boot_count_lo);
        TEST_ASSERT_EQUAL_UINT16(source.crc16, decoded.crc16);
    }

    TEST_ASSERT_EQUAL_UINT8(0, clampSocHalfPct(-1));
    TEST_ASSERT_EQUAL_UINT8(255, clampSocHalfPct(20000));
    TEST_ASSERT_EQUAL_INT8(-128, clampCrateQuarterPctHr(-5000));
    TEST_ASSERT_EQUAL_INT8(127, clampCrateQuarterPctHr(5000));
}

static void test_crc_rejects_corruption() {
    Record source = makeRecord(7, 11, 3777, 7500, 125, AWAKE_SAMPLE, 3);
    uint8_t encoded[kRecordSize];
    encode(source, encoded);
    encoded[6] ^= 0x40;
    TEST_ASSERT_FALSE(validate(encoded));
    Record decoded;
    TEST_ASSERT_FALSE(decode(encoded, &decoded));
}

static void test_charge_cycle_clean_sequence() {
    ChargeCycleDetector detector;
    resetChargeCycleDetector(&detector);
    TEST_ASSERT_FALSE(feedChargeCycle(&detector, 4, 300));
    TEST_ASSERT_FALSE(feedChargeCycle(&detector, 4, 300));
    TEST_ASSERT_FALSE(feedChargeCycle(&detector, -4, 300));
    TEST_ASSERT_TRUE(feedChargeCycle(&detector, -4, 300));
    TEST_ASSERT_FALSE(feedChargeCycle(&detector, -4, 600));
}

static void test_charge_cycle_jitter_does_not_count() {
    ChargeCycleDetector detector;
    resetChargeCycleDetector(&detector);
    for (int i = 0; i < 20; ++i) {
        int8_t rate = (i & 1) ? -1 : 1;
        TEST_ASSERT_FALSE(feedChargeCycle(&detector, rate, 300));
    }
    TEST_ASSERT_FALSE(feedChargeCycle(&detector, 0, 600));
}

static void test_charge_cycle_charging_boot() {
    ChargeCycleDetector detector;
    resetChargeCycleDetector(&detector, true);
    TEST_ASSERT_FALSE(feedChargeCycle(&detector, -2, 300));
    TEST_ASSERT_TRUE(feedChargeCycle(&detector, -2, 300));
}

static void test_ring_rotation_math() {
    TEST_ASSERT_EQUAL_UINT32(3, recordCountForBytes(3U * kRecordSize + 15U));
    TEST_ASSERT_FALSE(ringNeedsRotation(kMaxRecords - 1U, 1));
    TEST_ASSERT_TRUE(ringNeedsRotation(kMaxRecords, 1));
    TEST_ASSERT_EQUAL_UINT32(kRotationRecords,
                             rotationDropCount(kMaxRecords, 1));
    TEST_ASSERT_EQUAL_UINT32(kRotationRecords * kRecordSize,
                             rotationReadOffsetBytes(kMaxRecords, 384));
    TEST_ASSERT_EQUAL_UINT32(2U * kRotationRecords,
                             rotationDropCount(kMaxRecords, kRotationRecords + 1U));
}

static void test_upload_gate() {
    const uint32_t now = 1800000000U;
    TEST_ASSERT_TRUE(uploadDue(true, true, true, true, true, now, 0, 8000));
    TEST_ASSERT_FALSE(uploadDue(false, true, true, true, true, now, 0, 8000));
    TEST_ASSERT_FALSE(uploadDue(true, false, true, true, true, now, 0, 8000));
    TEST_ASSERT_FALSE(uploadDue(true, true, false, true, true, now, 0, 8000));
    TEST_ASSERT_FALSE(uploadDue(true, true, true, false, true, now, 0, 8000));
    TEST_ASSERT_FALSE(uploadDue(true, true, true, true, false, now, 0, 8000));
    TEST_ASSERT_FALSE(uploadDue(true, true, true, true, true, now, now - (21U * 3600U - 1U), 8000));
    TEST_ASSERT_TRUE(uploadDue(true, true, true, true, true, now, now - 21U * 3600U, 8000));
    TEST_ASSERT_FALSE(uploadDue(true, true, true, true, true, now, 0, 7999));
}

static void test_an_answered_upload_waits_a_day() {
    // Any answer (accepted or refused) stores the attempt time; only no
    // answer at all tries again at the next session.
    TEST_ASSERT_TRUE(uploadStartsWait(200));
    TEST_ASSERT_TRUE(uploadStartsWait(400));
    TEST_ASSERT_TRUE(uploadStartsWait(413));
    TEST_ASSERT_TRUE(uploadStartsWait(429));
    TEST_ASSERT_TRUE(uploadStartsWait(503));
    TEST_ASSERT_FALSE(uploadStartsWait(0));
    TEST_ASSERT_FALSE(uploadStartsWait(-1));
    // The stored time then holds the next upload back for the full gap.
    const uint32_t now = 1800000000U;
    TEST_ASSERT_FALSE(uploadDue(true, true, true, true, true, now + 3600U, now, 8000));
    TEST_ASSERT_TRUE(uploadDue(true, true, true, true, true, now + kUploadGapSec, now, 8000));
}

static void test_upload_window() {
    Record records[5] = {};
    for (uint32_t i = 0; i < 5; ++i) records[i].seq = i + 10;
    UploadWindow w = uploadWindow(records, 5, 11, false, 4);
    TEST_ASSERT_EQUAL(2, w.first);
    TEST_ASSERT_EQUAL(3, w.count);
    TEST_ASSERT_TRUE(w.complete);
    // More unsent than one upload holds: the OLDEST go first (was: the
    // newest, first == 3), so the next upload continues from there.
    w = uploadWindow(records, 5, 0, false, 2);
    TEST_ASSERT_EQUAL(0, w.first);
    TEST_ASSERT_EQUAL(2, w.count);
    TEST_ASSERT_FALSE(w.complete);
    w = uploadWindow(records, 5, records[1].seq, false, 2);
    TEST_ASSERT_EQUAL(2, w.first);
    TEST_ASSERT_EQUAL(2, w.count);
    w = uploadWindow(records, 5, 8, true, 5);
    TEST_ASSERT_EQUAL(5, w.count);
    TEST_ASSERT_FALSE(w.complete);
    w = uploadWindow(records, 5, 14, false, 5);
    TEST_ASSERT_EQUAL(0, w.count);
    records[3].seq = 20;
    records[4].seq = 21;
    w = uploadWindow(records, 5, 11, false, 5);
    TEST_ASSERT_FALSE(w.complete);
}

static void test_upload_window_drops_duplicate_sequence() {
    Record records[5] = {};
    const uint32_t seqs[] = {1813, 1814, 1814, 1815, 1816};
    for (size_t i = 0; i < 5; ++i) records[i].seq = seqs[i];
    UploadWindow w = uploadWindow(records, 5, 1812, false);
    TEST_ASSERT_EQUAL(0, w.first);
    TEST_ASSERT_EQUAL(4, w.count);
    TEST_ASSERT_FALSE(w.complete);
    const uint32_t kept[] = {1813, 1814, 1815, 1816};
    for (size_t i = 0; i < w.count; ++i)
        TEST_ASSERT_EQUAL_UINT32(kept[i], records[w.first + i].seq);
}

static void test_upload_window_drops_decreasing_sequence() {
    Record records[5] = {};
    const uint32_t seqs[] = {1813, 1814, 1812, 1815, 1816};
    for (size_t i = 0; i < 5; ++i) records[i].seq = seqs[i];
    const UploadWindow w = uploadWindow(records, 5, 1812, false);
    TEST_ASSERT_EQUAL(4, w.count);
    TEST_ASSERT_FALSE(w.complete);
    const uint32_t kept[] = {1813, 1814, 1815, 1816};
    for (size_t i = 0; i < w.count; ++i)
        TEST_ASSERT_EQUAL_UINT32(kept[i], records[w.first + i].seq);
}

static void test_upload_window_normal_run_stays_complete() {
    Record records[4] = {};
    for (uint32_t i = 0; i < 4; ++i) records[i].seq = 20 + i;
    const UploadWindow w = uploadWindow(records, 4, 19, false);
    TEST_ASSERT_EQUAL(4, w.count);
    TEST_ASSERT_TRUE(w.complete);
    for (uint32_t i = 0; i < 4; ++i)
        TEST_ASSERT_EQUAL_UINT32(20 + i, records[w.first + i].seq);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_codec_round_trip_all_events_and_clamps);
    RUN_TEST(test_crc_rejects_corruption);
    RUN_TEST(test_charge_cycle_clean_sequence);
    RUN_TEST(test_charge_cycle_jitter_does_not_count);
    RUN_TEST(test_charge_cycle_charging_boot);
    RUN_TEST(test_ring_rotation_math);
    RUN_TEST(test_upload_gate);
    RUN_TEST(test_upload_window);
    RUN_TEST(test_upload_window_drops_duplicate_sequence);
    RUN_TEST(test_upload_window_drops_decreasing_sequence);
    RUN_TEST(test_upload_window_normal_run_stays_complete);
    RUN_TEST(test_an_answered_upload_waits_a_day);
    return UNITY_END();
}
