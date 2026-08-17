// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#ifndef UVLO_LOGIC_H
#define UVLO_LOGIC_H

#include <stdint.h>
#include <stdbool.h>

// These values are working targets pending battery discharge-curve measurement.
#ifndef CF_UVLO_SLEEP_THRESHOLD_MV
#define CF_UVLO_SLEEP_THRESHOLD_MV 3500
#endif

#ifndef CF_UVLO_RUNTIME_THRESHOLD_MV
#define CF_UVLO_RUNTIME_THRESHOLD_MV 3300
#endif

#ifndef CF_UVLO_CHECK_INTERVAL_S
#define CF_UVLO_CHECK_INTERVAL_S 3600
#endif

#ifndef CF_UVLO_RUNTIME_DEBOUNCE_MS
#define CF_UVLO_RUNTIME_DEBOUNCE_MS 3000
#endif

namespace UvloLogic {

enum class SleepDecision : uint8_t {
    Resleep,
    Shutdown
};

inline SleepDecision decideSleep(int32_t vcellMv, bool plausible) {
    if (!plausible) return SleepDecision::Resleep;
    return vcellMv < CF_UVLO_SLEEP_THRESHOLD_MV
        ? SleepDecision::Shutdown
        : SleepDecision::Resleep;
}

constexpr uint64_t checkIntervalUs() {
    return (uint64_t)CF_UVLO_CHECK_INTERVAL_S * 1000000ULL;
}

class RuntimeDebounce {
public:
    RuntimeDebounce()
        : belowSinceMs_(0), trackingBelow_(false), shutdownLatched_(false) {}

    bool feed(int32_t vcellMv, uint32_t nowMs, bool plausible) {
        if (!plausible || vcellMv >= CF_UVLO_RUNTIME_THRESHOLD_MV) {
            reset();
            return false;
        }

        if (shutdownLatched_) return true;

        if (!trackingBelow_) {
            belowSinceMs_ = nowMs;
            trackingBelow_ = true;
            return false;
        }

        if ((uint32_t)(nowMs - belowSinceMs_) >=
            (uint32_t)CF_UVLO_RUNTIME_DEBOUNCE_MS) {
            shutdownLatched_ = true;
            return true;
        }

        return false;
    }

    void reset() {
        belowSinceMs_ = 0;
        trackingBelow_ = false;
        shutdownLatched_ = false;
    }

private:
    uint32_t belowSinceMs_;
    bool trackingBelow_;
    bool shutdownLatched_;
};

}  // namespace UvloLogic

#endif  // UVLO_LOGIC_H
