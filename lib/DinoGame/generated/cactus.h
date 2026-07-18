// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

// Generated from .cfsprite.json. Edit the source asset, not this header.
#ifndef CF_GFX_CACTUS_H
#define CF_GFX_CACTUS_H

#include "cf_gfx.h"

namespace cf { namespace gfx { namespace cactus {

// Cels use XBM row stride and LSB-first bit order.
inline constexpr uint8_t cel_0_bits[] PROGMEM = {
    0x0c, 0x0c, 0x0c, 0x3f, 0x3f, 0x0c, 0x0c, 0x0c, 0x0c, 0x0c, 0x0c, 0x1e,
    0x1e, 0x1e
};

inline constexpr Sprite cel_0 PROGMEM = {cel_0_bits, 6, 14, 3, 16, BO_LSB_FIRST};

// Frame lists and non-uniform durations.
inline constexpr const Sprite* const idle_frames[] PROGMEM = {&cel_0};

// One canonical array preserves Animation pointer identity.
inline constexpr Animation animations[] PROGMEM = {
    {"idle", idle_frames, nullptr, 0, 1, LOOP_LOOP}
};

inline constexpr const Animation* anim_idle = &animations[0];

inline constexpr SpriteSheet sheet PROGMEM = {"Cactus", animations, 1, "sprite"};

}}}  // namespace cf::gfx::cactus

namespace cf { namespace gfx {
inline constexpr Character cactus_character PROGMEM = {"Cactus", &cactus::sheet, nullptr};
}}  // namespace cf::gfx

#endif  // CF_GFX_CACTUS_H
