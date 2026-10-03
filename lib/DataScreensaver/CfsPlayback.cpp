// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2026 Dismo Industries LLC

// lib/DataScreensaver/CfsPlayback.cpp - see CfsPlayback.h.

#include "CfsPlayback.h"

namespace Cfs {

bool Playback::begin(Reader& reader, DisplayProxy& d) {
    reader_  = &reader;
    started_ = false;
    shown_   = -1;
    if (!reader.isOpen() || !reader.loadFrame(0)) return false;
    const Header& h = reader.header();
    // Every frame is the one Sprite over the reader's frame buffer; update()
    // loads the frame that is due into that buffer.
    sprite_ = cf::gfx::Sprite{reader.frame(), h.width, h.height, 0, 0, cf::gfx::BO_LSB_FIRST};
    for (uint16_t i = 0; i < h.frameCount; ++i) frames_[i] = &sprite_;
    anim_ = cf::gfx::Animation{"cfs", frames_, reader.durations(), 0,
                               (uint8_t)h.frameCount, (cf::gfx::LoopMode)h.loopMode};
    x_ = (int16_t)((kMaxWidth - (int)h.width) / 2);
    y_ = (int16_t)((kMaxHeight - (int)h.height) / 2);
    shown_ = 0;
    draw(d);
    return true;
}

void Playback::startClock(uint32_t nowMs) {
    if (!reader_ || shown_ < 0) return;
    player_.play(&anim_, nowMs);
    started_ = true;
}

Playback::Step Playback::update(uint32_t nowMs) {
    if (!started_) return Step::Unchanged;
    player_.update(nowMs);
    const int want = player_.sprite() ? (int)player_.frameIndex() : -1;
    if (want == shown_) return Step::Unchanged;
    if (want >= 0 && want != reader_->loadedIndex()) {
        if (!reader_->loadFrame((uint16_t)want)) return Step::Failed;
    }
    shown_ = want;
    return Step::Redraw;
}

void Playback::draw(DisplayProxy& d) const {
    if (shown_ >= 0) cf::gfx::drawSprite(d, sprite_, x_, y_);
}

}  // namespace Cfs
