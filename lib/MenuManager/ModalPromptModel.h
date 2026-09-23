// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#ifndef MODAL_PROMPT_MODEL_H
#define MODAL_PROMPT_MODEL_H

#include <stdint.h>

/**
 * @brief Pure selection / window / timeout state behind ModalPrompt.
 *
 * No display, no buttons, no clock: callers pass the time in. Native tests
 * drive this directly; ModalPrompt owns one and draws from it.
 *
 * - Selection is zero-based and starts on option 0.
 * - Up/Down WRAP (Up from the first option lands on the last, Down from
 *   the last lands on the first), matching the main menu's
 *   moveHighlightUp/moveHighlightDown.
 * - The visible window is the smallest scroll that keeps the selection on
 *   screen; a wrap jumps the window to the matching end of the list.
 * - Select is edge-triggered: only a release that follows a press seen
 *   while the prompt was open chooses, so a button already held when the
 *   prompt opened cannot pick an option.
 * - The timeout is optional (0 = none) and counts from the last Up/Down
 *   or from opening, so it never fires while someone is navigating. It
 *   closes the prompt with kNoChoice and never selects an option.
 */
class ModalPromptModel {
public:
    static constexpr int kNoChoice = -1;

    /** Opens with the selection on option 0. Refuses a count below 1. */
    bool open(int optionCount, int visibleRows, uint32_t nowMs, uint32_t timeoutMs) {
        if (optionCount < 1 || visibleRows < 1) return false;
        count_       = optionCount;
        rows_        = visibleRows;
        selected_    = 0;
        windowStart_ = 0;
        timeoutMs_   = timeoutMs;
        lastInputMs_ = nowMs;
        selectArmed_ = false;
        result_      = kNoChoice;
        open_        = true;
        return true;
    }

    void moveUp(uint32_t nowMs) {
        if (!open_) return;
        selected_ = (selected_ > 0) ? selected_ - 1 : count_ - 1;
        lastInputMs_ = nowMs;
        followSelection();
    }

    void moveDown(uint32_t nowMs) {
        if (!open_) return;
        selected_ = (selected_ < count_ - 1) ? selected_ + 1 : 0;
        lastInputMs_ = nowMs;
        followSelection();
    }

    /** Select button went down while the prompt was open. */
    void selectPressed() {
        if (open_) selectArmed_ = true;
    }

    /**
     * @brief Select button came up. Chooses the current option (and closes)
     * only when armed by a press; returns true when it chose.
     */
    bool selectReleased() {
        if (!open_ || !selectArmed_) return false;
        selectArmed_ = false;
        result_ = selected_;
        open_ = false;
        return true;
    }

    /** Returns true exactly when this call timed the prompt out. */
    bool tick(uint32_t nowMs) {
        if (!open_ || timeoutMs_ == 0) return false;
        if (nowMs - lastInputMs_ < timeoutMs_) return false;
        result_ = kNoChoice;
        open_ = false;
        selectArmed_ = false;
        return true;
    }

    /** Close without a choice (e.g. the host tears the prompt down). */
    void dismiss() {
        open_ = false;
        selectArmed_ = false;
        result_ = kNoChoice;
    }

    bool isOpen() const      { return open_; }
    int  optionCount() const { return count_; }
    int  selected() const    { return selected_; }
    int  windowStart() const { return windowStart_; }
    /** Rows actually shown: min(option count, visible rows). */
    int  windowCount() const { return count_ < rows_ ? count_ : rows_; }
    /** Chosen index after closing, or kNoChoice. */
    int  result() const      { return result_; }

private:
    void followSelection() {
        if (selected_ < windowStart_) {
            windowStart_ = selected_;
        } else if (selected_ >= windowStart_ + rows_) {
            windowStart_ = selected_ - rows_ + 1;
        }
    }

    int      count_       = 0;
    int      rows_        = 1;
    int      selected_    = 0;
    int      windowStart_ = 0;
    uint32_t timeoutMs_   = 0;
    uint32_t lastInputMs_ = 0;
    bool     selectArmed_ = false;
    bool     open_        = false;
    int      result_      = kNoChoice;
};

#endif
