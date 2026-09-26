// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC
#ifndef BATTERY_USAGE_UPLOAD_H
#define BATTERY_USAGE_UPLOAD_H

#include "BatteryDiary.h"

namespace BatteryDiary {

// Under the 24 h daily spacing (a slow session must not push the next upload
// to every other day) and over the site's 20 h acceptance cap.
constexpr uint32_t kUploadGapSec = 21U * 60U * 60U;
// More than a fully awake day of samples (288), so one missed day loses none.
constexpr size_t kUploadRecords = 1024;

inline bool uploadDue(bool enabled, bool linked, bool batteryEligible,
                      bool automatic, bool checkinSucceeded, uint32_t now,
                      uint32_t lastAccepted, uint32_t remainingMs) {
    return enabled && linked && batteryEligible && automatic && checkinSucceeded &&
           now > 1577836800U &&
           (lastAccepted == 0 || (now >= lastAccepted && now - lastAccepted >= kUploadGapSec)) &&
           remainingMs >= 8000U;
}

struct UploadWindow {
    size_t first = 0;
    size_t count = 0;
    bool complete = true;
};

// Input is the chronological tail returned by readLastRecords. Gaps before
// that tail cannot be recovered after ring rotation and make it partial.
inline UploadWindow uploadWindow(const Record* records, size_t count,
                                 uint32_t ackedSeq, bool tailTruncated,
                                 size_t maxRecords = kUploadRecords) {
    UploadWindow out;
    if (!records || !count || !maxRecords) return out;
    size_t first = 0;
    while (first < count && records[first].seq <= ackedSeq) ++first;
    if (first == count) return out;
    out.first = first;
    out.count = count - first;
    out.complete = !tailTruncated || records[first].seq == ackedSeq + 1U;
    if (out.count > maxRecords) {
        out.first += out.count - maxRecords;
        out.count = maxRecords;
        out.complete = false;
    }
    if (ackedSeq && records[out.first].seq > ackedSeq + 1U) out.complete = false;
    for (size_t i = out.first + 1; i < out.first + out.count; ++i)
        if (records[i].seq != records[i - 1].seq + 1U) out.complete = false;
    return out;
}

} // namespace BatteryDiary
#endif
