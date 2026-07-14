// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

// lib/CFGraphics/include/cf_gfx_icons.h
//
// Semantic aliases for menu/app icons (T-111, design N-033). Icons are
// just Sprites/Animations — no new core types. Menu and AppDef icon
// integration lands in a later phase (N-033 Phase 5); the aliases exist
// now so that code reads as what it means.

#ifndef CF_GFX_ICONS_H
#define CF_GFX_ICONS_H

#include "cf_gfx_sprite.h"

namespace cf { namespace gfx {

using Icon         = Sprite;     // a static 1-bit icon
using AnimatedIcon = Animation;  // an animated icon (e.g. hourglass)

}}  // namespace cf::gfx

#endif  // CF_GFX_ICONS_H
