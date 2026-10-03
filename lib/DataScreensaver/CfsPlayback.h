// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2026 Dismo Industries LLC

/**
 * CfsPlayback - plays an open Cfs::Reader through cf::gfx::AnimationPlayer:
 * which frame is on screen, when it changes, and where it is drawn
 * (centered when smaller than 128x64). No clock or display of its own: the
 * caller passes the time and the DisplayProxy, so the native tests drive it
 * with a fake clock and the recording DisplayProxy of cf_gfx_sprite.h.
 *
 * Order of use (the first frame is always shown, however late the first
 * update comes):
 *   begin(reader, display)   frame 0 loaded and drawn (caller then pushes it)
 *   startClock(now)          frame 0's duration counts from here
 *   update(now) -> Redraw    caller clears, draw(display), pushes
 */

#ifndef CFS_PLAYBACK_H
#define CFS_PLAYBACK_H

#include <stdint.h>

#include "CfsFormat.h"
#include "cf_gfx.h"

namespace Cfs {

class Playback {
public:
    enum class Step : uint8_t {
        Unchanged,  ///< the screen already shows the right thing
        Redraw,     ///< another frame (or blank, after a play-once item) is due
        Failed,     ///< the frame could not be read
    };

    /// `reader` must be open and outlive the playback. Loads frame 0 and
    /// draws it. False when frame 0 cannot be read.
    bool begin(Reader& reader, DisplayProxy& d);

    /// Start the frame timing (call right after frame 0 is on screen).
    void startClock(uint32_t nowMs);

    /// Advance to `nowMs`, loading the frame that is now due.
    Step update(uint32_t nowMs);

    /// Draw what is shown now (nothing for a finished play-once item).
    void draw(DisplayProxy& d) const;

    int     shownIndex() const { return shown_; }  ///< -1 = blank
    int16_t x() const { return x_; }
    int16_t y() const { return y_; }

private:
    Reader*                   reader_ = nullptr;
    cf::gfx::Sprite           sprite_ = {};
    const cf::gfx::Sprite*    frames_[kMaxFrames] = {};
    cf::gfx::Animation        anim_ = {};
    cf::gfx::AnimationPlayer  player_;
    int16_t                   x_ = 0;
    int16_t                   y_ = 0;
    int                       shown_ = -1;
    bool                      started_ = false;
};

}  // namespace Cfs

#endif  // CFS_PLAYBACK_H
