// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

// Generated from .cfmesh.json. Edit the source asset, not this header.
#ifndef CF_GFX_SHIP_H
#define CF_GFX_SHIP_H

#include "cf_gfx.h"

namespace cf { namespace gfx { namespace ship {

inline constexpr MeshVertex ship_verts[] PROGMEM = {
    {0, 12, 0}, {0, -8, 2}, {-3, -6, -2}, {3, -6, -2},
    {-2, 1, 0}, {2, 1, 0}, {-17, -5, -1}, {17, -5, -1},
    {-17, 2, 5}, {17, 2, 5}, {0, 4, 4}, {-2, 2, 2},
    {2, 2, 2}, {-5, -7, -1}, {5, -7, -1}
};
inline constexpr uint8_t ship_edges[][2] PROGMEM = {
    {0, 2}, {0, 3}, {2, 1}, {3, 1}, {0, 1}, {4, 6},
    {6, 2}, {5, 7}, {7, 3}, {6, 8}, {7, 9}, {10, 11},
    {10, 12}, {11, 12}, {10, 0}, {2, 13}, {3, 14}, {13, 14}
};
inline constexpr Mesh ship PROGMEM = {"ship", ship_verts, 15, ship_edges, 18};

}}}  // namespace cf::gfx::ship

#endif  // CF_GFX_SHIP_H
