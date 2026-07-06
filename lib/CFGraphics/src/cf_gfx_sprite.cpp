// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

// lib/CFGraphics/src/cf_gfx_sprite.cpp — drawSpritePivoted + findAnimation.

#include "cf_gfx_sprite.h"

namespace cf { namespace gfx {

void drawSpritePivoted(DisplayProxy& d, const Sprite& s, int16_t x, int16_t y) {
    if (s.data == nullptr) return;
    d.drawXbm((int16_t)(x - s.pivotX), (int16_t)(y - s.pivotY),
              (int16_t)s.w, (int16_t)s.h, s.data);
}

const Animation* findAnimation(const SpriteSheet& sheet, const char* name) {
    if (name == nullptr || sheet.animations == nullptr) return nullptr;
    for (uint8_t i = 0; i < sheet.animationCount; ++i) {
        const Animation& anim = sheet.animations[i];
        if (anim.name != nullptr && strcmp(anim.name, name) == 0) {
            return &anim;
        }
    }
    return nullptr;
}

}}  // namespace cf::gfx
