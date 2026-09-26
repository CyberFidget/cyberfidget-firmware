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

// Whether an upload attempt stores its time (upd.usage_at) and so waits the
// full gap before the next one. Any answer from the site does - accepted,
// refused (non-2xx), or an answer the device could not use - so a site that
// refuses is not re-sent up to 98 KB every automatic session. Only no answer
// at all (status 0: no connection, timeout) tries again next session.
inline bool uploadStartsWait(int httpStatus) { return httpStatus >= 100; }

struct UploadWindow {
    size_t first = 0;
    size_t count = 0;
    bool complete = true;
};

// Input is the chronological tail returned by readLastRecords. The selected
// records are compacted in place to remove repeated or decreasing sequences.
// Gaps before that tail cannot be recovered after ring rotation.
inline UploadWindow uploadWindow(Record* records, size_t count,
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
        // Oldest unsent first: the next upload continues after this one's
        // acknowledged sequence, so nothing is skipped while the ring holds it.
        out.count = maxRecords;
        out.complete = false;
    }
    const size_t end = out.first + out.count;
    size_t kept = out.first;
    uint32_t previous = ackedSeq;
    for (size_t i = out.first; i < end; ++i) {
        if (records[i].seq <= previous) {
            out.complete = false;
            continue;
        }
        if (previous && records[i].seq != previous + 1U) out.complete = false;
        if (kept != i) records[kept] = records[i];
        previous = records[kept++].seq;
    }
    out.count = kept - out.first;
    return out;
}

} // namespace BatteryDiary
#endif
