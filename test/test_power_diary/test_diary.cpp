#include <unity.h>

#include "../../lib/BatteryDiary/BatteryDiary.h"

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

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_codec_round_trip_all_events_and_clamps);
    RUN_TEST(test_crc_rejects_corruption);
    RUN_TEST(test_charge_cycle_clean_sequence);
    RUN_TEST(test_charge_cycle_jitter_does_not_count);
    RUN_TEST(test_charge_cycle_charging_boot);
    RUN_TEST(test_ring_rotation_math);
    return UNITY_END();
}
