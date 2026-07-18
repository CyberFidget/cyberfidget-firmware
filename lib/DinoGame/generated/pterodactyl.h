// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

// Generated from .cfsprite.json. Edit the source asset, not this header.
#ifndef CF_GFX_PTERODACTYL_H
#define CF_GFX_PTERODACTYL_H

#include "cf_gfx.h"

namespace cf { namespace gfx { namespace pterodactyl {

// Cels use XBM row stride and LSB-first bit order.
inline constexpr uint8_t cel_0_bits[] PROGMEM = {
    0x08, 0x00, 0x18, 0x00, 0x3c, 0x00, 0x7e, 0x00, 0xff, 0x00, 0x3c, 0xc0,
    0x18, 0x60, 0x00, 0x30, 0x00, 0x18, 0x00, 0x18, 0x00, 0x30, 0x00, 0x60,
    0x00, 0xc0
};

inline constexpr Sprite cel_0 PROGMEM = {cel_0_bits, 16, 13, 8, 15, BO_LSB_FIRST};

// Frame lists and non-uniform durations.
inline constexpr const Sprite* const fly_frames[] PROGMEM = {&cel_0};

// One canonical array preserves Animation pointer identity.
inline constexpr Animation animations[] PROGMEM = {
    {"fly", fly_frames, nullptr, 0, 1, LOOP_LOOP}
};

inline constexpr const Animation* anim_fly = &animations[0];

inline constexpr SpriteSheet sheet PROGMEM = {"Pterodactyl", animations, 1, "sprite"};

}}}  // namespace cf::gfx::pterodactyl

namespace cf { namespace gfx {
inline constexpr Character pterodactyl_character PROGMEM = {"Pterodactyl", &pterodactyl::sheet, nullptr};
}}  // namespace cf::gfx

#endif  // CF_GFX_PTERODACTYL_H
