// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

// lib/CFGraphics/src/cf_gfx_actor.cpp — AnimationPlayer + Actor.
// Timing/loop semantics are documented in cf_gfx_actor.h and pinned by
// test/test_cfgraphics_player + test/test_cfgraphics_actor.

#include "cf_gfx_actor.h"

namespace cf { namespace gfx {

// ---------------------------------------------------------------------
// AnimationPlayer
// ---------------------------------------------------------------------

AnimationPlayer::AnimationPlayer()
    : anim_(nullptr), epochMs_(0), frameIdx_(0), forward_(true), finished_(true) {}

void AnimationPlayer::play(const Animation* anim, uint32_t nowMs) {
    anim_     = anim;
    epochMs_  = nowMs;
    frameIdx_ = 0;
    forward_  = true;
    finished_ = (anim == nullptr || anim->frameCount == 0);
}

void AnimationPlayer::setIfChanged(const Animation* anim, uint32_t nowMs) {
    if (anim == anim_) return;  // pointer comparison only — O(1), tick-safe
    play(anim, nowMs);
}

void AnimationPlayer::stop() {
    anim_     = nullptr;
    frameIdx_ = 0;
    forward_  = true;
    finished_ = true;
}

const Sprite* AnimationPlayer::sprite() const {
    if (anim_ == nullptr || anim_->frameCount == 0) return nullptr;
    if (finished_ && anim_->loop == LOOP_ONCE) return nullptr;  // "then stop"
    return anim_->frames[frameIdx_];
}

uint16_t AnimationPlayer::frameDurationMs(uint8_t idx) const {
    if (anim_->durations_ms != nullptr) return anim_->durations_ms[idx];
    return anim_->uniformMs;
}

void AnimationPlayer::stepFrame() {
    const uint8_t last = (uint8_t)(anim_->frameCount - 1);
    switch (anim_->loop) {
        case LOOP_ONCE:
        case LOOP_ONCE_HOLD:
            if (frameIdx_ < last) {
                ++frameIdx_;
            } else {
                finished_ = true;  // frame stays on `last`; sprite() decides
            }
            break;
        case LOOP_LOOP:
            frameIdx_ = (uint8_t)((frameIdx_ >= last) ? 0 : frameIdx_ + 1);
            break;
        case LOOP_PINGPONG:
            // last == 0 is guarded in update(); each end is shown for
            // exactly one duration per pass: 0,1,..,N-1,N-2,..,1,0,1,..
            if (forward_) {
                if (frameIdx_ >= last) {
                    forward_ = false;
                    --frameIdx_;
                } else {
                    ++frameIdx_;
                }
            } else {
                if (frameIdx_ == 0) {
                    forward_ = true;
                    ++frameIdx_;
                } else {
                    --frameIdx_;
                }
            }
            break;
    }
}

bool AnimationPlayer::update(uint32_t nowMs) {
    if (anim_ == nullptr || finished_ || anim_->frameCount == 0) return false;

    // Single-frame LOOP/PINGPONG can never change frame; skip the catch-up
    // walk entirely (also guards stepFrame()'s pingpong math against N==1).
    if (anim_->frameCount == 1 &&
        (anim_->loop == LOOP_LOOP || anim_->loop == LOOP_PINGPONG)) {
        return false;
    }

    const uint8_t startIdx = frameIdx_;
    uint16_t dur = frameDurationMs(frameIdx_);
    // Catch-up walk: linear in elapsed frames. Fine at app tick rates
    // (~50 Hz); don't park a playing Animation for hours between updates.
    // A 0 ms duration holds the current frame indefinitely (fps-0 pose).
    while (dur != 0 && !finished_ && (uint32_t)(nowMs - epochMs_) >= dur) {
        epochMs_ += dur;
        stepFrame();
        dur = frameDurationMs(frameIdx_);
    }

    if (finished_ && anim_->loop == LOOP_ONCE) return true;  // sprite -> nullptr
    return frameIdx_ != startIdx;
}

// ---------------------------------------------------------------------
// Actor
// ---------------------------------------------------------------------

Actor::Actor() : sheet_(nullptr), player_(), xQ8_(0), yQ8_(0) {}

void Actor::setSheet(const SpriteSheet* sheet) {
    sheet_ = sheet;
}

bool Actor::play(const char* animName, uint32_t nowMs) {
    if (sheet_ == nullptr) return false;
    const Animation* anim = findAnimation(*sheet_, animName);
    if (anim == nullptr) return false;  // silent no-op on miss — check the result
    player_.setIfChanged(anim, nowMs);
    return true;
}

bool Actor::play(const Animation* anim, uint32_t nowMs) {
    if (anim == nullptr) return false;
    player_.setIfChanged(anim, nowMs);
    return true;
}

bool Actor::update(uint32_t nowMs) {
    return player_.update(nowMs);
}

void Actor::draw(DisplayProxy& d) const {
    const Sprite* s = player_.sprite();
    if (s == nullptr) return;
    drawSpritePivoted(d, *s, x(), y());
}

void Actor::setPosQ8(int32_t xQ8, int32_t yQ8) {
    xQ8_ = xQ8;
    yQ8_ = yQ8;
}

}}  // namespace cf::gfx
