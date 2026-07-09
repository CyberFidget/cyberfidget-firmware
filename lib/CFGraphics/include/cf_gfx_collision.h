// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

// lib/CFGraphics/include/cf_gfx_collision.h
//
// Pixel-perfect collision for 1-bit sprites. AABB-then-bitmap structure
// ported from lib/DinoGame's pixelCollides (T-111, design N-033).
//
// NOTE on bit order: sampling here follows Sprite::bitOrder, and the
// framework's canonical order is BO_LSB_FIRST — bit (x & 7) of byte
// data[y * ((w + 7) / 8) + (x >> 3)], the same XBM order drawXbm renders.
// That means collision agrees with what's on screen. (DinoGame's legacy
// in-app helper sampled MSB-first while drawing LSB-first — see T-112
// migration notes before expecting bit-identical parity with it.)

#ifndef CF_GFX_COLLISION_H
#define CF_GFX_COLLISION_H

#include "cf_gfx_actor.h"
#include "cf_gfx_sprite.h"

namespace cf { namespace gfx {

// Sample one pixel from a Sprite's 1-bit bitmap. (x, y) are sprite-local
// coordinates; out-of-bounds sampling returns false (off).
bool sampleSprite(const Sprite& s, int16_t x, int16_t y);

// Pixel-perfect AABB-then-bitmap collision. Caller supplies TOP-LEFT
// screen positions (pivots are NOT applied) — same contract as
// DinoGame's original helper.
bool pixelCollides(const Sprite& a, int16_t ax, int16_t ay,
                   const Sprite& b, int16_t bx, int16_t by);

// Actor overload: positions are pivot-anchored (like Actor::draw), and the
// pivot -> top-left conversion happens internally. WARNING: do not mix the
// two forms — calling the Sprite overload with actor.x()/actor.y() on a
// pivoted sprite gives wrong-by-pivot results; use this overload instead.
bool pixelCollides(const Actor& a, const Actor& b);

}}  // namespace cf::gfx

#endif  // CF_GFX_COLLISION_H
