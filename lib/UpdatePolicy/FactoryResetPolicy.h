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
enum class BootStep { Normal, Finish, Wait, Unreadable };

inline BootStep bootStep(bool markSet, bool imagePending) {
    if (!markSet) return BootStep::Normal;
    if (imagePending) return BootStep::Wait;
    return BootStep::Finish;
}

// Reading the mark can fail for reasons other than "not there" (a settings
// store fault). The caller reads again once; a mark that is still
// unreadable starts normally (logged) and is not treated as a finished or
// absent reset: erasing on a guess could wipe a device that was never
// reset, and the mark, if it is there, stays for a later start-up.
enum class MarkRead { Clear, Set, Error };

inline BootStep bootStep(MarkRead mark, bool imagePending) {
    if (mark == MarkRead::Error) return BootStep::Unreadable;
    return bootStep(mark == MarkRead::Set, imagePending);
}

// The reset only starts once the mark is written AND reads back as set;
// otherwise nothing is erased (a power cut would leave apps erased with
// WiFi and the account link kept, and nothing to finish the job).
inline bool markConfirmed(bool written, MarkRead readBack) {
    return written && readBack == MarkRead::Set;
}

// Start-up finish: the first two tries since power-on format the app
// storage; later tries skip it so a crash in the format cannot loop.
constexpr uint32_t kFinishFormatTries = 2;
inline bool finishFormats(uint32_t tries) { return tries <= kFinishFormatTries; }

// A finish that did not format the app storage (skipped or failed) is
// partial, never done: the settings still go (that clears the mark and
// ends any loop) and the next start-up tells the owner to reset again.
enum class FinishResult { Done, Partial };
inline FinishResult finishResult(bool formatted) {
    return formatted ? FinishResult::Done : FinishResult::Partial;
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
