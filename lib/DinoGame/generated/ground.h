// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

// Generated from .cfsprite.json. Edit the source asset, not this header.
#ifndef CF_GFX_GROUND_H
#define CF_GFX_GROUND_H

#include "cf_gfx.h"

namespace cf { namespace gfx { namespace ground {

// Cels use XBM row stride and LSB-first bit order.
inline constexpr uint8_t cel_0_bits[] PROGMEM = {
    0xf0, 0x00, 0x60, 0x00, 0x60, 0x00, 0xf0, 0x00, 0xf8, 0x01, 0xfc, 0x03,
    0xfe, 0x07, 0xff, 0x0f
};
inline constexpr uint8_t cel_1_bits[] PROGMEM = {
    0xf8, 0x01, 0xf0, 0x00, 0xf0, 0x00, 0xf8, 0x01, 0xfc, 0x03, 0xfc, 0x03,
    0xfe, 0x07, 0xff, 0x0f
};
inline constexpr uint8_t cel_2_bits[] PROGMEM = {
    0xfc, 0x03, 0xf8, 0x01, 0xf0, 0x00, 0xf0, 0x00, 0xf8, 0x01, 0xfc, 0x03,
    0xfe, 0x07, 0xff, 0x0f
};
inline constexpr uint8_t cel_3_bits[] PROGMEM = {
    0xf0, 0x00, 0xf0, 0x00, 0xf0, 0x00, 0xf8, 0x01, 0xf8, 0x01, 0xfc, 0x03,
    0xfe, 0x07, 0xff, 0x0f
};

inline constexpr Sprite cel_0 PROGMEM = {cel_0_bits, 12, 8, 6, 8, BO_LSB_FIRST};
inline constexpr Sprite cel_1 PROGMEM = {cel_1_bits, 12, 8, 6, 8, BO_LSB_FIRST};
inline constexpr Sprite cel_2 PROGMEM = {cel_2_bits, 12, 8, 6, 8, BO_LSB_FIRST};
inline constexpr Sprite cel_3 PROGMEM = {cel_3_bits, 12, 8, 6, 8, BO_LSB_FIRST};

// Frame lists and non-uniform durations.
inline constexpr const Sprite* const tile_a_frames[] PROGMEM = {&cel_0};
inline constexpr const Sprite* const tile_b_frames[] PROGMEM = {&cel_1};
inline constexpr const Sprite* const tile_c_frames[] PROGMEM = {&cel_2};
inline constexpr const Sprite* const tile_d_frames[] PROGMEM = {&cel_3};

// One canonical array preserves Animation pointer identity.
inline constexpr Animation animations[] PROGMEM = {
    {"tile_a", tile_a_frames, nullptr, 0, 1, LOOP_LOOP},
    {"tile_b", tile_b_frames, nullptr, 0, 1, LOOP_LOOP},
    {"tile_c", tile_c_frames, nullptr, 0, 1, LOOP_LOOP},
    {"tile_d", tile_d_frames, nullptr, 0, 1, LOOP_LOOP}
};

inline constexpr const Animation* anim_tile_a = &animations[0];
inline constexpr const Animation* anim_tile_b = &animations[1];
inline constexpr const Animation* anim_tile_c = &animations[2];
inline constexpr const Animation* anim_tile_d = &animations[3];

inline constexpr SpriteSheet sheet PROGMEM = {"Ground", animations, 4, "tile"};

}}}  // namespace cf::gfx::ground

namespace cf { namespace gfx {
inline constexpr Character ground_character PROGMEM = {"Ground", &ground::sheet, nullptr};
}}  // namespace cf::gfx

#endif  // CF_GFX_GROUND_H
