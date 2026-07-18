// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

// Generated from .cfsprite.json. Edit the source asset, not this header.
#ifndef CF_GFX_DINO_H
#define CF_GFX_DINO_H

#include "cf_gfx.h"

namespace cf { namespace gfx { namespace dino {

// Cels use XBM row stride and LSB-first bit order.
inline constexpr uint8_t cel_0_bits[] PROGMEM = {
    0x0f, 0x00, 0x1f, 0x00, 0x19, 0x80, 0x19, 0x80, 0x1f, 0x80, 0x19, 0x80,
    0x19, 0x80, 0x1f, 0x00, 0x18, 0x00, 0x38, 0x00, 0x38, 0x00, 0x38, 0x00,
    0x70, 0x00, 0x30, 0x00
};
inline constexpr uint8_t cel_1_bits[] PROGMEM = {
    0x1f, 0xc0, 0x3f, 0xe0, 0x33, 0x30, 0x33, 0x30, 0x3f, 0xe0, 0x1f, 0xc0
};

inline constexpr Sprite cel_0 PROGMEM = {cel_0_bits, 16, 14, 8, 16, BO_LSB_FIRST};
inline constexpr Sprite cel_1 PROGMEM = {cel_1_bits, 16, 6, 8, 7, BO_LSB_FIRST};

// Frame lists and non-uniform durations.
inline constexpr const Sprite* const run_frames[] PROGMEM = {&cel_0};
inline constexpr const Sprite* const duck_frames[] PROGMEM = {&cel_1};
inline constexpr const Sprite* const jump_frames[] PROGMEM = {&cel_0};

// One canonical array preserves Animation pointer identity.
inline constexpr Animation animations[] PROGMEM = {
    {"run", run_frames, nullptr, 0, 1, LOOP_LOOP},
    {"duck", duck_frames, nullptr, 0, 1, LOOP_LOOP},
    {"jump", jump_frames, nullptr, 0, 1, LOOP_LOOP}
};

inline constexpr const Animation* anim_run = &animations[0];
inline constexpr const Animation* anim_duck = &animations[1];
inline constexpr const Animation* anim_jump = &animations[2];

inline constexpr SpriteSheet sheet PROGMEM = {"Dino", animations, 3, "character"};

}}}  // namespace cf::gfx::dino

namespace cf { namespace gfx {
inline constexpr Character dino_character PROGMEM = {"Dino", &dino::sheet, nullptr};
}}  // namespace cf::gfx

#endif  // CF_GFX_DINO_H
