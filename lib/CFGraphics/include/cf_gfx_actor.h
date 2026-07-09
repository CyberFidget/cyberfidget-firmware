// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

// lib/CFGraphics/include/cf_gfx_actor.h
//
// AnimationPlayer (time -> current frame state machine), Actor (Q8
// fixed-point position + player + sheet binding) and the optional
// Character skin abstraction. Part of the cf::gfx runtime (T-111,
// design N-033, invariants REQ-042).

#ifndef CF_GFX_ACTOR_H
#define CF_GFX_ACTOR_H

#include "cf_gfx_sprite.h"

namespace cf { namespace gfx {

/**
 * Answers "given a clock and an Animation, what frame should I be on?"
 * so apps stop hand-rolling `if (millis() - lastFrameTime > 100)` timers.
 *
 * Timing contract (pinned by test/test_cfgraphics_player):
 * - play() starts at frame 0; a frame advances once its full duration has
 *   elapsed (change happens at exactly epoch + duration).
 * - update() catches up over large gaps (advances multiple frames) and
 *   returns true iff the sprite to draw changed during the call.
 * - A frame duration of 0 ms holds that frame indefinitely (fps-0 poses).
 * - LOOP_ONCE: after the last frame's duration elapses, finished() flips
 *   true and sprite() returns nullptr (draw nothing). animation() still
 *   reports what finished, so setIfChanged() with the same Animation stays
 *   a no-op rather than restarting it.
 * - LOOP_ONCE_HOLD: finished() flips true, sprite() keeps returning the
 *   last frame forever.
 * - Time must be monotonic (millis()-style); uint32 wraparound in the
 *   caller's clock is handled by unsigned subtraction.
 */
class AnimationPlayer {
public:
    AnimationPlayer();

    /// Start (or restart) an Animation at frame 0. play(nullptr) == stop().
    void play(const Animation* anim, uint32_t nowMs);

    /// play(), but a no-op when `anim` is already the current Animation —
    /// pointer comparison only, O(1), safe to call every tick.
    void setIfChanged(const Animation* anim, uint32_t nowMs);

    /// Advance the clock. Returns true iff the sprite to draw changed.
    bool update(uint32_t nowMs);

    /// Clear playback entirely: sprite()/animation() return nullptr,
    /// finished() reports true.
    void stop();

    /// Current frame's Sprite, or nullptr (nothing playing, or a LOOP_ONCE
    /// Animation that has finished).
    const Sprite* sprite() const;

    uint8_t frameIndex() const { return frameIdx_; }
    bool finished() const { return finished_; }
    const Animation* animation() const { return anim_; }

private:
    uint16_t frameDurationMs(uint8_t idx) const;
    void stepFrame();  // advance one frame per loop policy (or set finished_)

    const Animation* anim_;
    uint32_t epochMs_;   // when the current frame started
    uint8_t  frameIdx_;
    bool     forward_;   // ping-pong direction
    bool     finished_;
};

/**
 * Actor = Q8.8 fixed-point position + AnimationPlayer + optional
 * SpriteSheet binding. Position is where the current sprite's PIVOT lands
 * on screen (draw() and the Actor pixelCollides overload both apply the
 * pivot offset for you — see cf_gfx_collision.h for the mixing warning).
 *
 * Both play() overloads use setIfChanged semantics: calling play with the
 * Animation that is already current is a no-op (no restart). Name lookup
 * is O(N) strcmp over the sheet — fine on state transitions, not per tick;
 * hot loops should pass Animation pointers.
 */
class Actor {
public:
    Actor();

    /// Bind the sheet used by name-based play(). Does not stop playback.
    void setSheet(const SpriteSheet* sheet);
    const SpriteSheet* sheet() const { return sheet_; }

    /// Play by name (looked up in the bound sheet). Returns false — and
    /// leaves current playback untouched — when there is no sheet or the
    /// name misses (a typo'd name is a silent no-op; check the result).
    bool play(const char* animName, uint32_t nowMs);

    /// Play by pointer (no sheet needed). Returns false on nullptr.
    bool play(const Animation* anim, uint32_t nowMs);

    /// Advance the player. Returns true iff the sprite to draw changed.
    bool update(uint32_t nowMs);

    /// Pivot-aware draw at the current position (wraps drawXbm through
    /// drawSpritePivoted). No-op when there is no sprite to show.
    void draw(DisplayProxy& d) const;

    // --- Q8.8 fixed-point position -----------------------------------
    // 1 px == 256 units. x()/y() floor toward negative infinity
    // (arithmetic shift), so -0.5 px reads back as -1.
    void setPosQ8(int32_t xQ8, int32_t yQ8);
    void setPos(int16_t x, int16_t y) {
        setPosQ8((int32_t)x << 8, (int32_t)y << 8);
    }
    int32_t xQ8() const { return xQ8_; }
    int32_t yQ8() const { return yQ8_; }
    int16_t x() const { return (int16_t)(xQ8_ >> 8); }
    int16_t y() const { return (int16_t)(yQ8_ >> 8); }

    const Sprite*    sprite()    const { return player_.sprite(); }
    const Animation* animation() const { return player_.animation(); }
    bool             finished()  const { return player_.finished(); }

private:
    const SpriteSheet* sheet_;
    AnimationPlayer    player_;
    int32_t xQ8_;
    int32_t yQ8_;
};

// OPTIONAL skin abstraction: swap the whole look of an app by pointing at
// a different Character (dino -> robot -> samurai). Apps that don't swap
// skins never touch this. POD, PROGMEM-friendly like the asset types.
struct Character {
    const char*        name;
    const SpriteSheet* sheet;
    const char*        skeleton;  // optional contract id, e.g. "dino_runner_v1";
                                  // nullptr = no contract (validated by the
                                  // T-112 converter, not at runtime)
};

static_assert(std::is_trivial<Character>::value && std::is_standard_layout<Character>::value,
              "Character must stay POD (PROGMEM-friendly)");

}}  // namespace cf::gfx

#endif  // CF_GFX_ACTOR_H
