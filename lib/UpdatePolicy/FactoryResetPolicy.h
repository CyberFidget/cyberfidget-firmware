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
