// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

// lib/CFGraphics/src/cf_gfx_collision.cpp — pixel-perfect collision.
// AABB-then-overlap-scan structure ported from lib/DinoGame's
// DinoGame::pixelCollides; bit sampling goes through sampleSprite so it
// follows the canonical LSB-first XBM order (see cf_gfx_collision.h for
// the note on DinoGame's legacy MSB-first sampling).

#include "cf_gfx_collision.h"

namespace cf { namespace gfx {

namespace {
inline int16_t cfMin(int16_t a, int16_t b) { return (a < b) ? a : b; }
inline int16_t cfMax(int16_t a, int16_t b) { return (a > b) ? a : b; }
}  // namespace

bool sampleSprite(const Sprite& s, int16_t x, int16_t y) {
    if (s.data == nullptr) return false;
    if (x < 0 || y < 0 || (uint16_t)x >= s.w || (uint16_t)y >= s.h) return false;

    const uint16_t bytesPerRow = (uint16_t)((s.w + 7u) / 8u);  // XBM row stride
    const uint32_t byteIndex   = (uint32_t)y * bytesPerRow + ((uint16_t)x >> 3);
    const uint8_t  byte        = pgm_read_byte(&s.data[byteIndex]);
    // Canonical order is LSB-first (bit x&7) — matches drawXbm and the
    // .cfsprite v1 wire profile. BO_MSB_FIRST is honored for legacy data.
    const uint8_t bit = (s.bitOrder == BO_MSB_FIRST) ? (uint8_t)(7 - (x & 7))
                                                     : (uint8_t)(x & 7);
    return ((byte >> bit) & 1u) != 0;
}

bool pixelCollides(const Sprite& a, int16_t ax, int16_t ay,
                   const Sprite& b, int16_t bx, int16_t by) {
    if (a.data == nullptr || b.data == nullptr) return false;
    if (a.w == 0 || a.h == 0 || b.w == 0 || b.h == 0) return false;

    // Bounding-box check (inclusive edges) — same structure as DinoGame's.
    const int16_t aLeft   = ax;
    const int16_t aRight  = (int16_t)(ax + (int16_t)a.w - 1);
    const int16_t aTop    = ay;
    const int16_t aBottom = (int16_t)(ay + (int16_t)a.h - 1);
    const int16_t bLeft   = bx;
    const int16_t bRight  = (int16_t)(bx + (int16_t)b.w - 1);
    const int16_t bTop    = by;
    const int16_t bBottom = (int16_t)(by + (int16_t)b.h - 1);

    if (aRight < bLeft || aLeft > bRight || aBottom < bTop || aTop > bBottom) {
        return false;
    }

    const int16_t overlapLeft   = cfMax(aLeft, bLeft);
    const int16_t overlapRight  = cfMin(aRight, bRight);
    const int16_t overlapTop    = cfMax(aTop, bTop);
    const int16_t overlapBottom = cfMin(aBottom, bBottom);

    for (int16_t py = overlapTop; py <= overlapBottom; ++py) {
        for (int16_t px = overlapLeft; px <= overlapRight; ++px) {
            if (sampleSprite(a, (int16_t)(px - aLeft), (int16_t)(py - aTop)) &&
                sampleSprite(b, (int16_t)(px - bLeft), (int16_t)(py - bTop))) {
                return true;
            }
        }
    }
    return false;
}

bool pixelCollides(const Actor& a, const Actor& b) {
    const Sprite* sa = a.sprite();
    const Sprite* sb = b.sprite();
    if (sa == nullptr || sb == nullptr) return false;
    // Actor positions are pivot-anchored; convert to top-left here so the
    // caller never has to do pos - pivot math by hand.
    return pixelCollides(*sa, (int16_t)(a.x() - sa->pivotX), (int16_t)(a.y() - sa->pivotY),
                         *sb, (int16_t)(b.x() - sb->pivotX), (int16_t)(b.y() - sb->pivotY));
}

}}  // namespace cf::gfx
