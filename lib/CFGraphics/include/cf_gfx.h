// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

// lib/CFGraphics/include/cf_gfx.h
//
// Umbrella header for the cf::gfx sprite & animation runtime (T-111,
// design N-033, invariants REQ-042). Apps include this one header plus
// their own generated asset headers (lib/<App>/generated/*.h, emitted by
// the T-112 converter):
//
//   #include "cf_gfx.h"
//   #include "generated/dino.h"
//   using namespace cf::gfx;
//
//   Actor player;
//   player.setSheet(dino_character.sheet);
//   player.play("run", millis());
//   ...
//   player.update(millis());
//   player.draw(display);
//
// Runtime only — no assets live in this library (REQ-042.2).

#ifndef CF_GFX_H
#define CF_GFX_H

#include "cf_gfx_sprite.h"
#include "cf_gfx_actor.h"
#include "cf_gfx_mesh.h"
#include "cf_gfx_collision.h"
#include "cf_gfx_icons.h"

#endif  // CF_GFX_H
