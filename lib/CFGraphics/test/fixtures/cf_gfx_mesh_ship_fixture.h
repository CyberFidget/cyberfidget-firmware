// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

// lib/CFGraphics/test/fixtures/cf_gfx_mesh_ship_fixture.h
//
// Hand-authored true-3D ship mesh in the emitted header-only fixture style.

#ifndef CF_GFX_MESH_SHIP_FIXTURE_H
#define CF_GFX_MESH_SHIP_FIXTURE_H

#include "cf_gfx_mesh.h"

namespace cf { namespace gfx { namespace meshfixtures {

inline constexpr MeshVertex ship_verts[] PROGMEM = {
    {  0,  0, -12},
    {  0,  2,   8},
    { -3, -2,   6},
    {  3, -2,   6},
    { -2,  0,  -1},
    {  2,  0,  -1},
    {-17, -1,   5},
    { 17, -1,   5},
    {-17,  5,  -2},
    { 17,  5,  -2},
    {  0,  4,  -4},
    { -2,  2,  -2},
    {  2,  2,  -2},
    { -5, -1,   7},
    {  5, -1,   7},
};

inline constexpr uint8_t ship_edges[][2] PROGMEM = {
    {0, 2}, {0, 3}, {2, 1}, {3, 1}, {0, 1},
    {4, 6}, {6, 2}, {5, 7}, {7, 3},
    {6, 8}, {7, 9},
    {10, 11}, {10, 12}, {11, 12}, {10, 0},
    {2, 13}, {3, 14}, {13, 14},
};

inline constexpr Mesh ship_mesh PROGMEM = {
    "ship", ship_verts, 15, ship_edges, 18};

}}}  // namespace cf::gfx::meshfixtures

#endif  // CF_GFX_MESH_SHIP_FIXTURE_H
