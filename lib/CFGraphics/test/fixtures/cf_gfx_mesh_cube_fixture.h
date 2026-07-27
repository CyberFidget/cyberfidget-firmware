// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

// lib/CFGraphics/test/fixtures/cf_gfx_mesh_cube_fixture.h
//
// Hand-checkable cube mesh for exact quarter-turn and projection tests.

#ifndef CF_GFX_MESH_CUBE_FIXTURE_H
#define CF_GFX_MESH_CUBE_FIXTURE_H

#include "cf_gfx_mesh.h"

namespace cf { namespace gfx { namespace meshfixtures {

inline constexpr MeshVertex cube_verts[] PROGMEM = {
    {-8, -8, -8},
    { 8, -8, -8},
    { 8,  8, -8},
    {-8,  8, -8},
    {-8, -8,  8},
    { 8, -8,  8},
    { 8,  8,  8},
    {-8,  8,  8},
};

inline constexpr uint8_t cube_edges[][2] PROGMEM = {
    {0, 1}, {1, 2}, {2, 3}, {3, 0},
    {4, 5}, {5, 6}, {6, 7}, {7, 4},
    {0, 4}, {1, 5}, {2, 6}, {3, 7},
};

inline constexpr Mesh cube_mesh PROGMEM = {
    "cube", cube_verts, 8, cube_edges, 12};

}}}  // namespace cf::gfx::meshfixtures

#endif  // CF_GFX_MESH_CUBE_FIXTURE_H
