// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#ifndef SCROLL_LABEL_H
#define SCROLL_LABEL_H

#include <stdint.h>

/**
 * @brief One-line text label that marquees when it is wider than its box.
 *
 * Lifted from the Music Player's now-playing title so every screen scrolls
 * long text the same way:
 *  - Text that fits (width <= box width, exact fit included) never moves.
 *  - Longer text steps kStepPx to the left every time more than kStepMs
 *    has passed since the last step. Once it has travelled kEndPadPx past
 *    the point where its tail is fully visible, it jumps to kRestartPx
 *    (a small rightward lead-in) and scrolls again.
 *  - reset() puts the text back at its start position. Like the original,
 *    it does not touch the step clock.
 *
 * The state math (tick/offset) is pure and native-tested; draw() is the
 * only part that touches the display.
 */
class ScrollLabel {
public:
    static constexpr int      kStepPx    = 6;
    static constexpr uint32_t kStepMs    = 300;
    static constexpr int      kEndPadPx  = 30;
    static constexpr int      kRestartPx = -20;

    /** Text scrolls only when strictly wider than its box. */
    static bool needsScroll(int textWidth, int boxWidth) {
        return textWidth > boxWidth;
    }

    /** Back to the start position (call when the text changes). */
    void reset() { offset_ = 0; }

    /**
     * @brief Advance the marquee for a label of textWidth pixels in a box of
     * boxWidth pixels at time nowMs. Does nothing for text that fits.
     */
    void tick(int textWidth, int boxWidth, uint32_t nowMs) {
        if (!needsScroll(textWidth, boxWidth)) return;
        if (nowMs - lastStepMs_ > kStepMs) {
            lastStepMs_ = nowMs;
            offset_ += kStepPx;
            if (offset_ > textWidth - boxWidth + kEndPadPx) {
                offset_ = kRestartPx;
            }
        }
    }

    /** Pixels the text is shifted left of the box's left edge. */
    int offset() const { return offset_; }

    /**
     * @brief Tick with millis() and draw text on the current display font.
     * Text that fits is centered in the box when centerWhenShort is true,
     * otherwise left-aligned at x. Long text is left-aligned at
     * x - offset(). Leaves the text alignment set to whatever it last used;
     * callers that depend on an alignment set it again afterwards.
     */
    void draw(int x, int y, int boxWidth, const char* text, bool centerWhenShort);

private:
    int      offset_     = 0;
    uint32_t lastStepMs_ = 0;
};

#endif
