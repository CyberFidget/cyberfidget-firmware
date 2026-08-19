// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#ifndef BATTERY_DIARY_H
#define BATTERY_DIARY_H

#include <stddef.h>
#include <stdint.h>

namespace BatteryDiary {

constexpr size_t kRecordSize = 16;
constexpr uint32_t kMaxRecords = 3072;
constexpr uint32_t kRotationRecords = 1024;
constexpr uint32_t kRtcCapacity = 384;
// Flush daily, not at ring capacity: any USB plug or port-open can pulse EN
// through the auto-reset circuit, and EN-low clears RTC memory - so the
// buffer's loss bound must stay small. A flush wake costs ~2-3 uAh against a
// ~250 uA sleep floor, so daily flushing is ~0.04% of standby drain. The
// large ring remains useful as retry headroom when a flush mount fails.
constexpr uint32_t kRtcFlushThreshold = 24;

enum Event : uint8_t {
    BOOT = 1,
    TIMER_CHECKIN = 2,
    AWAKE_SAMPLE = 3,
    SLEEP_ENTER = 4,
    SHUTDOWN_SLEEPSIDE = 5,
    SHUTDOWN_RUNTIME = 6,
    FLUSH_MARKER = 7,
};

enum WakeCause : uint8_t {
    WAKE_POWER_ON = 0,
    WAKE_EXT0 = 1,
    WAKE_EXT1 = 2,
    WAKE_TIMER = 3,
    WAKE_TOUCH = 4,
    WAKE_ULP = 5,
    WAKE_OTHER = 6,
};

#pragma pack(push, 1)
struct Record {
    uint32_t seq;
    uint32_t uptime_or_count;
    int16_t vcell_mv;
    uint8_t soc_half_pct;
    int8_t crate_qtr_pct_hr;
    uint8_t event;
    uint8_t boot_count_lo;
    uint16_t crc16;
};
#pragma pack(pop)

static_assert(sizeof(Record) == kRecordSize, "battery diary record must be 16 bytes");

inline uint16_t crc16(const uint8_t* data, size_t len) {
    uint16_t crc = 0xFFFFU;
    for (size_t i = 0; i < len; ++i) {
        crc ^= (uint16_t)data[i] << 8;
        for (uint8_t bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x8000U) ? (uint16_t)((crc << 1) ^ 0x1021U)
                                  : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

inline void putLe16(uint8_t* out, uint16_t value) {
    out[0] = (uint8_t)(value & 0xFFU);
    out[1] = (uint8_t)(value >> 8);
}

inline void putLe32(uint8_t* out, uint32_t value) {
    out[0] = (uint8_t)(value & 0xFFU);
    out[1] = (uint8_t)((value >> 8) & 0xFFU);
    out[2] = (uint8_t)((value >> 16) & 0xFFU);
    out[3] = (uint8_t)(value >> 24);
}

inline uint16_t getLe16(const uint8_t* in) {
    return (uint16_t)in[0] | ((uint16_t)in[1] << 8);
}

inline uint32_t getLe32(const uint8_t* in) {
    return (uint32_t)in[0] | ((uint32_t)in[1] << 8) |
           ((uint32_t)in[2] << 16) | ((uint32_t)in[3] << 24);
}

inline void encode(const Record& record, uint8_t out[kRecordSize]) {
    putLe32(out + 0, record.seq);
    putLe32(out + 4, record.uptime_or_count);
    putLe16(out + 8, (uint16_t)record.vcell_mv);
    out[10] = record.soc_half_pct;
    out[11] = (uint8_t)record.crate_qtr_pct_hr;
    out[12] = record.event;
    out[13] = record.boot_count_lo;
    putLe16(out + 14, crc16(out, 14));
}

inline bool validate(const uint8_t encoded[kRecordSize]) {
    return getLe16(encoded + 14) == crc16(encoded, 14);
}

inline bool decode(const uint8_t encoded[kRecordSize], Record* record) {
    if (record == nullptr || !validate(encoded)) return false;
    record->seq = getLe32(encoded + 0);
    record->uptime_or_count = getLe32(encoded + 4);
    record->vcell_mv = (int16_t)getLe16(encoded + 8);
    record->soc_half_pct = encoded[10];
    record->crate_qtr_pct_hr = (int8_t)encoded[11];
    record->event = encoded[12];
    record->boot_count_lo = encoded[13];
    record->crc16 = getLe16(encoded + 14);
    return true;
}

inline uint8_t clampSocHalfPct(int32_t soc_hundredths_pct) {
    if (soc_hundredths_pct <= 0) return 0;
    if (soc_hundredths_pct >= 12750) return 255;
    return (uint8_t)((soc_hundredths_pct * 2 + 50) / 100);
}

inline int8_t clampCrateQuarterPctHr(int32_t crate_hundredths_pct_hr) {
    if (crate_hundredths_pct_hr <= -3200) return -128;
    if (crate_hundredths_pct_hr >= 3175) return 127;
    int32_t scaled = crate_hundredths_pct_hr * 4;
    scaled += (scaled >= 0) ? 50 : -50;
    return (int8_t)(scaled / 100);
}

inline Record makeRecord(uint32_t seq, uint32_t uptime_or_count,
                         int16_t vcell_mv, int32_t soc_hundredths_pct,
                         int32_t crate_hundredths_pct_hr, Event event,
                         uint8_t boot_count_lo) {
    Record record = {};
    record.seq = seq;
    record.uptime_or_count = uptime_or_count;
    record.vcell_mv = vcell_mv;
    record.soc_half_pct = clampSocHalfPct(soc_hundredths_pct);
    record.crate_qtr_pct_hr = clampCrateQuarterPctHr(crate_hundredths_pct_hr);
    record.event = (uint8_t)event;
    record.boot_count_lo = boot_count_lo;
    uint8_t encoded[kRecordSize];
    encode(record, encoded);
    record.crc16 = getLe16(encoded + 14);
    return record;
}

struct ChargeCycleDetector {
    uint32_t positive_seconds;
    uint32_t negative_seconds;
    bool positive_qualified;
};

inline void resetChargeCycleDetector(ChargeCycleDetector* detector,
                                     bool charging_boot = false) {
    if (detector == nullptr) return;
    detector->positive_seconds = 0;
    detector->negative_seconds = 0;
    detector->positive_qualified = charging_boot;
}

inline bool feedChargeCycle(ChargeCycleDetector* detector,
                            int8_t crate_qtr_pct_hr,
                            uint32_t elapsed_seconds) {
    if (detector == nullptr || elapsed_seconds == 0) return false;
    constexpr uint32_t kSustainedSeconds = 10U * 60U;
    if (crate_qtr_pct_hr > 0) {
        detector->negative_seconds = 0;
        if (detector->positive_seconds < kSustainedSeconds) {
            uint32_t room = kSustainedSeconds - detector->positive_seconds;
            detector->positive_seconds += (elapsed_seconds > room) ? room : elapsed_seconds;
        }
        if (detector->positive_seconds >= kSustainedSeconds)
            detector->positive_qualified = true;
        return false;
    }
    if (crate_qtr_pct_hr < 0) {
        detector->positive_seconds = 0;
        if (!detector->positive_qualified) {
            detector->negative_seconds = 0;
            return false;
        }
        if (detector->negative_seconds < kSustainedSeconds) {
            uint32_t room = kSustainedSeconds - detector->negative_seconds;
            detector->negative_seconds += (elapsed_seconds > room) ? room : elapsed_seconds;
        }
        if (detector->negative_seconds >= kSustainedSeconds) {
            resetChargeCycleDetector(detector, false);
            return true;
        }
        return false;
    }
    detector->positive_seconds = 0;
    detector->negative_seconds = 0;
    return false;
}

inline uint32_t recordCountForBytes(uint32_t bytes) {
    return bytes / (uint32_t)kRecordSize;
}

inline bool ringNeedsRotation(uint32_t existing_records, uint32_t incoming_records) {
    return incoming_records > kMaxRecords ||
           existing_records > kMaxRecords - incoming_records;
}

inline uint32_t rotationDropCount(uint32_t existing_records,
                                  uint32_t incoming_records) {
    if (!ringNeedsRotation(existing_records, incoming_records)) return 0;
    uint32_t overflow = existing_records + incoming_records - kMaxRecords;
    uint32_t blocks = (overflow + kRotationRecords - 1U) / kRotationRecords;
    uint32_t drop = blocks * kRotationRecords;
    return (drop > existing_records) ? existing_records : drop;
}

inline uint32_t rotationReadOffsetBytes(uint32_t existing_records,
                                        uint32_t incoming_records) {
    return rotationDropCount(existing_records, incoming_records) * (uint32_t)kRecordSize;
}

#ifndef HOST_TEST

struct Stats {
    uint32_t schema_version;
    uint32_t boot_count;
    uint32_t checkin_count;
    uint32_t cum_on_time_s;
    uint32_t charge_cycle_count;
    uint32_t min_vcell_mv;
    uint32_t max_vcell_mv;
    uint32_t records_written;
    uint32_t records_dropped;
};

bool begin(const char* wake_cause_name);
void onAwakeTick(float vcell, float soc_pct, float crate_pct_hr);
void onTimerCheckin(int32_t vcell_mv);
bool timerFlushDue();
bool flushTimerCheckins();
void onTimerShutdown(int32_t vcell_mv);
void onSleepEnter(float vcell, float soc_pct, float crate_pct_hr);
void onRuntimeShutdown(float vcell, float soc_pct, float crate_pct_hr);
void onFlushMarker(uint32_t app_index, float vcell, float soc_pct,
                   float crate_pct_hr);
bool getStats(Stats* stats);
size_t readLastRecords(Record* records, size_t capacity, uint32_t* total_records);
bool clear();
const char* eventName(uint8_t event);

#endif

}  // namespace BatteryDiary

#endif  // BATTERY_DIARY_H
