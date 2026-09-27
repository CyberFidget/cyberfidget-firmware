// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#ifndef FACTORY_RESET_POLICY_H
#define FACTORY_RESET_POLICY_H

#include <stdint.h>

namespace FactoryResetPolicy {

enum class Refusal { None, PendingImage, ArmedSession };

inline Refusal refusal(bool imagePending, bool sessionArmed) {
    if (imagePending) return Refusal::PendingImage;
    if (sessionArmed) return Refusal::ArmedSession;
    return Refusal::None;
}

// What a start-up does with the "reset in progress" mark. The mark is
// written just before the apps are erased and goes with the settings erase
// that follows, so a mark seen at start-up means a reset was cut off
// between the two (power loss): the start-up finishes it before anything
// reads WiFi, the account link or the apps. A just-installed image still
// on probation never erases (the reset refuses then too); the mark waits
// for a later start. The mark can only have been written when the reset
// was allowed, so this is a guard, not an expected path.
enum class BootStep { Normal, Finish, Wait };

inline BootStep bootStep(bool markSet, bool imagePending) {
    if (!markSet) return BootStep::Normal;
    if (imagePending) return BootStep::Wait;
    return BootStep::Finish;
}

class Hold {
public:
    static constexpr uint32_t kDurationMs = 3000;

    void press(uint32_t now) { started_ = now; pressed_ = true; complete_ = false; }
    void release() { pressed_ = false; started_ = 0; complete_ = false; }
    void back() { release(); complete_ = false; }
    bool tick(uint32_t now) {
        if (pressed_ && static_cast<uint32_t>(now - started_) >= kDurationMs)
            complete_ = true;
        return complete_;
    }
    uint32_t progress(uint32_t now) const {
        if (!pressed_) return 0;
        const uint32_t elapsed = static_cast<uint32_t>(now - started_);
        return elapsed < kDurationMs ? elapsed : kDurationMs;
    }
    bool pressed() const { return pressed_; }

private:
    uint32_t started_ = 0;
    bool pressed_ = false;
    bool complete_ = false;
};

} // namespace FactoryResetPolicy

#endif
