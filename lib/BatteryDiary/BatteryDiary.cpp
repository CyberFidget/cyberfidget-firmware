// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#include "BatteryDiary.h"

#ifndef HOST_TEST

#include <Arduino.h>
#include <FS.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <esp_attr.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace BatteryDiary {
namespace {

constexpr char kDirectory[] = "/apps/.diary";
constexpr char kRingPath[] = "/apps/.diary/batdiary.bin";
constexpr char kRingTempPath[] = "/apps/.diary/batdiary.tmp";
constexpr char kStatsPath[] = "/apps/.diary/batstats.bin";
constexpr uint32_t kStatsSchema = 1;
constexpr uint32_t kRtcMagic = 0x42445231U;
constexpr uint32_t kFirstAwakeTicks = 30U * 5U;
constexpr uint32_t kAwakeSampleTicks = 5U * 60U * 5U;
// A sample held back by audio is written after 2 min at the latest (well
// inside the 5 min sample period, so two never queue up).
constexpr uint32_t kMaxAwakeDeferTicks = 2U * 60U * 5U;

#pragma pack(push, 1)
struct StoredStats {
    uint32_t schema_version;
    uint32_t boot_count;
    uint32_t checkin_count;
    uint32_t cum_on_time_s;
    uint32_t charge_cycle_count;
    uint32_t min_vcell_mv;
    uint32_t max_vcell_mv;
    uint32_t records_written;
    uint32_t records_dropped;
    uint16_t crc16;
};
#pragma pack(pop)

static_assert(sizeof(StoredStats) == 38, "battery diary stats layout changed");

struct RtcState {
    uint32_t magic;
    uint32_t head;
    uint32_t count;
    uint32_t next_seq;
    uint32_t checkin_count;
    uint32_t dropped_pending;
    uint8_t boot_count_lo;
    uint8_t reserved[3];
    Record records[kRtcCapacity];
};

RTC_DATA_ATTR static RtcState s_rtc;

static Stats s_stats;
static bool s_ready = false;
static uint32_t s_awakeTicks = 0;
static uint32_t s_nextSampleTick = kFirstAwakeTicks;
// The awake sample waiting for silence (see AwakeWriteGate).
static AwakeWriteGate s_sampleGate;
static uint32_t s_heldTime = 0;
static int16_t s_heldMv = 0;
static int32_t s_heldSoc = 0;
static int32_t s_heldCrate = 0;
static uint8_t s_onTimeSubticks = 0;
static bool s_firstAwakeSample = true;
static ChargeCycleDetector s_cycleDetector;

// The cloud worker reads the upload snapshot while the loop task appends and
// rotates: one recursive lock around every public entry point that touches
// the ring, the stats or s_ready (recursive: entry points call each other).
// Held only for file work, never across a network call.
SemaphoreHandle_t diaryLock() {
    static SemaphoreHandle_t lock = nullptr;
    static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
    if (lock) return lock;
    SemaphoreHandle_t created = xSemaphoreCreateRecursiveMutex();
    portENTER_CRITICAL(&mux);
    if (!lock) {
        lock = created;
        created = nullptr;
    }
    portEXIT_CRITICAL(&mux);
    if (created) vSemaphoreDelete(created);
    return lock;
}

class Guard {
public:
    Guard() : lock_(diaryLock()) { if (lock_) xSemaphoreTakeRecursive(lock_, portMAX_DELAY); }
    ~Guard() { if (lock_) xSemaphoreGiveRecursive(lock_); }
    Guard(const Guard&) = delete;
    Guard& operator=(const Guard&) = delete;
private:
    SemaphoreHandle_t lock_;
};

void defaultStats(Stats* stats) {
    if (stats == nullptr) return;
    stats->schema_version = kStatsSchema;
    stats->boot_count = 0;
    stats->checkin_count = 0;
    stats->cum_on_time_s = 0;
    stats->charge_cycle_count = 0;
    stats->min_vcell_mv = UINT32_MAX;
    stats->max_vcell_mv = 0;
    stats->records_written = 0;
    stats->records_dropped = 0;
}

void encodeStats(const Stats& stats, uint8_t out[sizeof(StoredStats)]) {
    putLe32(out + 0, stats.schema_version);
    putLe32(out + 4, stats.boot_count);
    putLe32(out + 8, stats.checkin_count);
    putLe32(out + 12, stats.cum_on_time_s);
    putLe32(out + 16, stats.charge_cycle_count);
    putLe32(out + 20, stats.min_vcell_mv);
    putLe32(out + 24, stats.max_vcell_mv);
    putLe32(out + 28, stats.records_written);
    putLe32(out + 32, stats.records_dropped);
    putLe16(out + 36, crc16(out, 36));
}

bool decodeStats(const uint8_t in[sizeof(StoredStats)], Stats* stats) {
    if (stats == nullptr || getLe16(in + 36) != crc16(in, 36)) return false;
    stats->schema_version = getLe32(in + 0);
    stats->boot_count = getLe32(in + 4);
    stats->checkin_count = getLe32(in + 8);
    stats->cum_on_time_s = getLe32(in + 12);
    stats->charge_cycle_count = getLe32(in + 16);
    stats->min_vcell_mv = getLe32(in + 20);
    stats->max_vcell_mv = getLe32(in + 24);
    stats->records_written = getLe32(in + 28);
    stats->records_dropped = getLe32(in + 32);
    return stats->schema_version == kStatsSchema;
}

bool ensureDirectory() {
    if (!LittleFS.exists("/apps") && !LittleFS.mkdir("/apps")) return false;
    return LittleFS.exists(kDirectory) || LittleFS.mkdir(kDirectory);
}

bool loadStats(Stats* stats) {
    if (stats == nullptr) return false;
    File file = LittleFS.open(kStatsPath, FILE_READ);
    if (!file || file.size() != sizeof(StoredStats)) return false;
    uint8_t encoded[sizeof(StoredStats)];
    size_t got = file.read(encoded, sizeof(encoded));
    file.close();
    return got == sizeof(encoded) && decodeStats(encoded, stats);
}

bool writeStats() {
    if (!ensureDirectory()) return false;
    uint8_t encoded[sizeof(StoredStats)];
    encodeStats(s_stats, encoded);
    File file = LittleFS.open(kStatsPath, FILE_WRITE);
    if (!file) return false;
    size_t wrote = file.write(encoded, sizeof(encoded));
    file.close();
    return wrote == sizeof(encoded);
}

void resetRtc(uint32_t next_seq, uint32_t checkin_count, uint8_t boot_count_lo) {
    s_rtc.magic = kRtcMagic;
    s_rtc.head = 0;
    s_rtc.count = 0;
    s_rtc.next_seq = next_seq;
    s_rtc.checkin_count = checkin_count;
    s_rtc.dropped_pending = 0;
    s_rtc.boot_count_lo = boot_count_lo;
    s_rtc.reserved[0] = s_rtc.reserved[1] = s_rtc.reserved[2] = 0;
}

bool rtcValid() {
    return s_rtc.magic == kRtcMagic && s_rtc.head < kRtcCapacity &&
           s_rtc.count <= kRtcCapacity && s_rtc.next_seq != 0;
}

int32_t hundredths(float value) {
    float scaled = value * 100.0f;
    return (int32_t)(scaled + ((scaled >= 0.0f) ? 0.5f : -0.5f));
}

int16_t millivolts(float value) {
    if (!(value >= 2.0f && value <= 4.6f)) return -1;
    return (int16_t)(value * 1000.0f + 0.5f);
}

void updateVoltageStats(int16_t vcell_mv) {
    if (vcell_mv < 0) return;
    uint32_t mv = (uint32_t)vcell_mv;
    if (mv < s_stats.min_vcell_mv) s_stats.min_vcell_mv = mv;
    if (mv > s_stats.max_vcell_mv) s_stats.max_vcell_mv = mv;
}

uint8_t wakeCauseCode(const char* name) {
    if (name == nullptr) return WAKE_OTHER;
    if (strcmp(name, "power_on") == 0) return WAKE_POWER_ON;
    if (strcmp(name, "ext0") == 0) return WAKE_EXT0;
    if (strcmp(name, "ext1") == 0) return WAKE_EXT1;
    if (strcmp(name, "timer") == 0) return WAKE_TIMER;
    if (strcmp(name, "touchpad") == 0) return WAKE_TOUCH;
    if (strcmp(name, "ulp") == 0) return WAKE_ULP;
    return WAKE_OTHER;
}

Record makeNextRecord(Event event, uint32_t time_value, int16_t vcell_mv,
                      int32_t soc_hundredths, int32_t crate_hundredths) {
    if (!rtcValid()) resetRtc(1, s_stats.checkin_count,
                              (uint8_t)(s_stats.boot_count & 0xFFU));
    Record record = makeRecord(s_rtc.next_seq++, time_value, vcell_mv,
                               soc_hundredths, crate_hundredths, event,
                               s_rtc.boot_count_lo);
    return record;
}

void appendRtc(const Record& record) {
    if (!rtcValid()) resetRtc(record.seq, s_stats.checkin_count,
                              (uint8_t)(s_stats.boot_count & 0xFFU));
    if (s_rtc.count == kRtcCapacity) {
        s_rtc.head = (s_rtc.head + 1U) % kRtcCapacity;
        --s_rtc.count;
        ++s_rtc.dropped_pending;
    }
    uint32_t index = (s_rtc.head + s_rtc.count) % kRtcCapacity;
    s_rtc.records[index] = record;
    ++s_rtc.count;
}

bool rotateRing(uint32_t incoming_records) {
    File source = LittleFS.open(kRingPath, FILE_READ);
    uint32_t existing = source ? recordCountForBytes((uint32_t)source.size()) : 0;
    uint32_t drop = rotationDropCount(existing, incoming_records);
    if (drop == 0) {
        if (source) source.close();
        return true;
    }
    LittleFS.remove(kRingTempPath);
    File target = LittleFS.open(kRingTempPath, FILE_WRITE);
    if (!source || !target || !source.seek(drop * (uint32_t)kRecordSize, SeekSet)) {
        if (source) source.close();
        if (target) target.close();
        LittleFS.remove(kRingTempPath);
        return false;
    }
    uint8_t buffer[256];
    bool ok = true;
    while (source.available()) {
        size_t got = source.read(buffer, sizeof(buffer));
        if (got == 0) break;
        if (target.write(buffer, got) != got) {
            ok = false;
            break;
        }
    }
    source.close();
    target.close();
    if (!ok) {
        LittleFS.remove(kRingTempPath);
        return false;
    }
    LittleFS.remove(kRingPath);
    if (!LittleFS.rename(kRingTempPath, kRingPath)) return false;
    s_stats.records_dropped += drop;
    return true;
}

bool appendFlash(const Record& record) {
    if (!ensureDirectory() || !rotateRing(1)) return false;
    uint8_t encoded[kRecordSize];
    encode(record, encoded);
    File file = LittleFS.open(kRingPath, FILE_APPEND);
    if (!file) return false;
    size_t wrote = file.write(encoded, sizeof(encoded));
    file.close();
    if (wrote != sizeof(encoded)) return false;
    ++s_stats.records_written;
    updateVoltageStats(record.vcell_mv);
    return true;
}

bool flushRtcInternal() {
    if (!rtcValid()) return true;
    if (s_rtc.checkin_count > s_stats.checkin_count)
        s_stats.checkin_count = s_rtc.checkin_count;
    if (s_rtc.dropped_pending != 0) {
        s_stats.records_dropped += s_rtc.dropped_pending;
        s_rtc.dropped_pending = 0;
    }
    while (s_rtc.count != 0) {
        Record record = s_rtc.records[s_rtc.head];
        if (!appendFlash(record)) {
            writeStats();
            return false;
        }
        s_rtc.head = (s_rtc.head + 1U) % kRtcCapacity;
        --s_rtc.count;
    }
    s_rtc.head = 0;
    return writeStats();
}

bool appendDirect(Event event, uint32_t time_value, int16_t vcell_mv,
                  int32_t soc_hundredths, int32_t crate_hundredths) {
    Record record = makeNextRecord(event, time_value, vcell_mv,
                                   soc_hundredths, crate_hundredths);
    return appendFlash(record) && writeStats();
}

bool mountForTimerFlush() {
    // No format-on-fail here: this runs unattended on a sleeping device, and
    // a corrupted filesystem must not be reformatted hourly in the dark. On
    // mount failure the records simply stay in the RTC ring and the flush
    // retries at the next threshold crossing; overflow drops are counted in
    // dropped_pending. The normal boot path keeps the house begin(true).
    return LittleFS.begin(false) && ensureDirectory();
}

}  // namespace

bool begin(const char* wake_cause_name) {
    Guard guard;
    if (!ensureDirectory()) return false;
    if (!loadStats(&s_stats)) defaultStats(&s_stats);
    uint32_t nextSeq = s_stats.records_written + 1U;
    Preferences sequencePrefs;
    if (sequencePrefs.begin("upd", true)) {
        const uint32_t floor = sequencePrefs.getUInt("usage_floor", 1);
        if (floor > nextSeq) nextSeq = floor;
        sequencePrefs.end();
    }
    File ring = LittleFS.open(kRingPath, FILE_READ);
    if (ring && ring.size() >= kRecordSize && ring.seek(ring.size() - kRecordSize, SeekSet)) {
        uint8_t encoded[kRecordSize];
        if (ring.read(encoded, sizeof(encoded)) == sizeof(encoded)) {
            Record last;
            if (decode(encoded, &last) && last.seq < UINT32_MAX && last.seq + 1U > nextSeq)
                nextSeq = last.seq + 1U;
        }
    }
    ring.close();
    if (!rtcValid()) {
        resetRtc(nextSeq, s_stats.checkin_count,
                 (uint8_t)(s_stats.boot_count & 0xFFU));
    } else {
        if (s_rtc.next_seq < nextSeq) s_rtc.next_seq = nextSeq;
        if (s_rtc.checkin_count > s_stats.checkin_count)
            s_stats.checkin_count = s_rtc.checkin_count;
    }
    s_ready = true;
    if (!flushRtcInternal()) return false;
    ++s_stats.boot_count;
    s_rtc.boot_count_lo = (uint8_t)(s_stats.boot_count & 0xFFU);
    resetChargeCycleDetector(&s_cycleDetector, false);
    return appendDirect(BOOT, wakeCauseCode(wake_cause_name), -1, 0, 0);
}

void onAwakeTick(float vcell, float soc_pct, float crate_pct_hr, bool audioActive) {
    Guard guard;
    if (!s_ready) return;
    ++s_awakeTicks;
    if (++s_onTimeSubticks >= 5U) {
        s_onTimeSubticks = 0;
        ++s_stats.cum_on_time_s;
    }
    int16_t mv = millivolts(vcell);
    updateVoltageStats(mv);
    if (s_awakeTicks >= s_nextSampleTick) {
        s_nextSampleTick += kAwakeSampleTicks;
        int32_t soc = hundredths(soc_pct);
        int32_t crate = hundredths(crate_pct_hr);
        int8_t crate_qtr = clampCrateQuarterPctHr(crate);
        if (s_firstAwakeSample) {
            resetChargeCycleDetector(&s_cycleDetector, crate_qtr > 0);
            s_firstAwakeSample = false;
        }
        if (feedChargeCycle(&s_cycleDetector, crate_qtr, 5U * 60U))
            ++s_stats.charge_cycle_count;
        // Taken now (values and time); written when no audio plays.
        s_heldTime = millis();
        s_heldMv = mv;
        s_heldSoc = soc;
        s_heldCrate = crate;
        s_sampleGate.hold(s_awakeTicks);
    }
    if (s_sampleGate.writeNow(s_awakeTicks, audioActive, kMaxAwakeDeferTicks)) {
        s_sampleGate.pending = false;
        appendDirect(AWAKE_SAMPLE, s_heldTime, s_heldMv, s_heldSoc, s_heldCrate);
    }
}

// A held awake sample goes into the RTC ring (no flash write) ahead of a
// sleep or shutdown record, so it is not lost.
static void keepHeldSample() {
    if (!s_sampleGate.pending) return;
    s_sampleGate.pending = false;
    appendRtc(makeNextRecord(AWAKE_SAMPLE, s_heldTime, s_heldMv, s_heldSoc, s_heldCrate));
}

void onTimerCheckin(int32_t vcell_mv) {
    Guard guard;
    if (!rtcValid()) resetRtc(1, 0, 0);
    ++s_rtc.checkin_count;
    int16_t mv = (vcell_mv >= INT16_MIN && vcell_mv <= INT16_MAX)
                     ? (int16_t)vcell_mv : -1;
    appendRtc(makeNextRecord(TIMER_CHECKIN, s_rtc.checkin_count, mv, 0, 0));
}

bool timerFlushDue() {
    Guard guard;
    return rtcValid() && s_rtc.count >= kRtcFlushThreshold;
}

bool flushTimerCheckins() {
    Guard guard;
    if (!mountForTimerFlush()) return false;
    if (!loadStats(&s_stats)) defaultStats(&s_stats);
    bool ok = flushRtcInternal();
    LittleFS.end();
    s_ready = false;
    return ok;
}

void onTimerShutdown(int32_t vcell_mv) {
    Guard guard;
    if (!rtcValid()) resetRtc(1, 0, 0);
    int16_t mv = (vcell_mv >= INT16_MIN && vcell_mv <= INT16_MAX)
                     ? (int16_t)vcell_mv : -1;
    appendRtc(makeNextRecord(SHUTDOWN_SLEEPSIDE, s_rtc.checkin_count, mv, 0, 0));
    flushTimerCheckins();
}

void onSleepEnter(float vcell, float soc_pct, float crate_pct_hr) {
    Guard guard;
    if (!s_ready) return;
    keepHeldSample();
    appendRtc(makeNextRecord(SLEEP_ENTER, millis(), millivolts(vcell),
                             hundredths(soc_pct), hundredths(crate_pct_hr)));
    writeStats();
}

void onRuntimeShutdown(float vcell, float soc_pct, float crate_pct_hr) {
    Guard guard;
    if (!s_ready) return;
    keepHeldSample();
    appendRtc(makeNextRecord(SHUTDOWN_RUNTIME, millis(), millivolts(vcell),
                             hundredths(soc_pct), hundredths(crate_pct_hr)));
    flushRtcInternal();
}

void onFlushMarker(uint32_t app_index, float vcell, float soc_pct,
                   float crate_pct_hr) {
    Guard guard;
    if (!s_ready) return;
    appendDirect(FLUSH_MARKER, app_index, millivolts(vcell),
                 hundredths(soc_pct), hundredths(crate_pct_hr));
}

bool getStats(Stats* stats) {
    Guard guard;
    if (!s_ready || stats == nullptr) return false;
    *stats = s_stats;
    if (rtcValid() && s_rtc.checkin_count > stats->checkin_count)
        stats->checkin_count = s_rtc.checkin_count;
    return true;
}

size_t readLastRecords(Record* records, size_t capacity, uint32_t* total_records) {
    Guard guard;
    if (total_records != nullptr) *total_records = 0;
    if (!s_ready || records == nullptr || capacity == 0) return 0;
    File file = LittleFS.open(kRingPath, FILE_READ);
    if (!file) return 0;
    uint32_t total = recordCountForBytes((uint32_t)file.size());
    if (total_records != nullptr) *total_records = total;
    uint32_t slots = (total < capacity) ? total : (uint32_t)capacity;
    if (!file.seek((total - slots) * (uint32_t)kRecordSize, SeekSet)) {
        file.close();
        return 0;
    }
    size_t count = 0;
    for (uint32_t i = 0; i < slots; ++i) {
        uint8_t encoded[kRecordSize];
        if (file.read(encoded, sizeof(encoded)) != sizeof(encoded)) break;
        Record record;
        if (decode(encoded, &record)) records[count++] = record;
    }
    file.close();
    return count;
}

size_t readUploadSnapshot(Record* records, size_t capacity, uint32_t* total_records,
                          uint8_t stats_bytes[38]) {
    Guard guard;
    if (total_records) *total_records = 0;
    if (!records || !capacity || !stats_bytes) return 0;
    // Normal boots already own a mounted LittleFS through LoadoutStore.
    // The timer flush helper ends that shared mount, so flush in place here.
    const bool wasReady = s_ready;
    if (!wasReady && !mountForTimerFlush()) return 0;
    if (!wasReady && !loadStats(&s_stats)) defaultStats(&s_stats);
    s_ready = true;
    if (!flushRtcInternal()) {
        s_ready = wasReady;
        return 0;
    }
    File stats = LittleFS.open(kStatsPath, FILE_READ);
    Stats checked;
    const bool statsOk = stats && stats.size() == 38 &&
                         stats.read(stats_bytes, 38) == 38 && decodeStats(stats_bytes, &checked);
    stats.close();
    const size_t count = statsOk ? readLastRecords(records, capacity, total_records) : 0;
    s_ready = wasReady;
    return count;
}

bool clear() {
    Guard guard;
    if (!s_ready || !ensureDirectory()) return false;
    // Sequence is the server's de-duplication key. Keep its next value even
    // when the ring and the other counters are deliberately reset.
    const uint32_t nextSeq = rtcValid() ? s_rtc.next_seq : s_stats.records_written + 1U;
    Preferences sequencePrefs;
    if (!sequencePrefs.begin("upd", false)) return false;
    const bool saved = sequencePrefs.putUInt("usage_floor", nextSeq) != 0;
    sequencePrefs.end();
    if (!saved) return false;
    File ring = LittleFS.open(kRingPath, FILE_WRITE);
    if (!ring) return false;
    ring.close();
    uint32_t lifetime_boots = s_stats.boot_count;
    uint32_t lifetime_on_s = s_stats.cum_on_time_s;
    defaultStats(&s_stats);
    s_stats.boot_count = lifetime_boots;
    s_stats.cum_on_time_s = lifetime_on_s;
    resetRtc(nextSeq, 0, (uint8_t)(lifetime_boots & 0xFFU));
    s_awakeTicks = 0;
    s_nextSampleTick = kFirstAwakeTicks;
    s_sampleGate.pending = false;
    s_onTimeSubticks = 0;
    s_firstAwakeSample = true;
    resetChargeCycleDetector(&s_cycleDetector, false);
    return writeStats();
}

const char* eventName(uint8_t event) {
    switch (event) {
        case BOOT: return "boot";
        case TIMER_CHECKIN: return "timer";
        case AWAKE_SAMPLE: return "awake";
        case SLEEP_ENTER: return "sleep";
        case SHUTDOWN_SLEEPSIDE: return "shutdown_sleep";
        case SHUTDOWN_RUNTIME: return "shutdown_runtime";
        case FLUSH_MARKER: return "marker";
        default: return "unknown";
    }
}

}  // namespace BatteryDiary

#endif  // HOST_TEST
