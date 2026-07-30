// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

// lib/CFGraphics/test/fixtures/cf_gfx_rig_golden.h
// Do not hand-edit; transcribed from cfsprite_pose_golden.json
// (website repo, cross-language pose contract).
// Drift guard:
//   node lib/CFGraphics/test/fixtures/check_rig_golden_sync.mjs \
//     .codex-ref/cfsprite_pose_golden.json \
//     lib/CFGraphics/test/fixtures/cf_gfx_rig_golden.h

#ifndef CF_GFX_RIG_GOLDEN_H
#define CF_GFX_RIG_GOLDEN_H

#include <stdint.h>

namespace cf { namespace gfx { namespace riggolden {

struct GoldenBone {
    const char* name;
    int8_t parent;
    int16_t length;
    int16_t atX;
    int16_t atY;
    int16_t restDegrees;
};

struct GoldenPose {
    const char* name;
    int16_t rootDy;
    int16_t offsets[6];
};

struct GoldenTransform {
    int16_t startX;
    int16_t startY;
    int16_t endX;
    int16_t endY;
    uint8_t angle;
};

struct GoldenCase {
    const char* name;
    uint8_t poseA;
    int8_t poseB;
    uint8_t blend;
    const GoldenTransform* expected;
};

// Format is intentionally regular; check_rig_golden_sync.mjs parses these
// emitted tables with regex so native tests need no JSON library.
inline constexpr GoldenBone bones[] = {
    {"pelvis", -1, 2, 0, 0, 0},
    {"spine", 0, 7, 2, 0, -90},
    {"head", 1, 3, 7, 0, 0},
    {"l_arm", 1, 5, 5, -1, -90},
    {"r_arm", 1, 5, 5, 1, 90},
    {"leg", 0, 6, 0, 0, 90},
};

inline constexpr GoldenPose poses[] = {
    {"rest", 0, {0, 0, 0, 0, 0, 0}},
    {"raised_arm", 0, {0, 0, 12, 0, -135, 0}},
    {"crouch", 5, {0, 18, 0, 0, 0, 35}},
    {"stretch", 0, {0, -15, -10, 70, -70, 0}},
    {"reach", 0, {0, 22, 25, -35, 150, 0}},
};

inline constexpr GoldenTransform case_0_expected[] = {
    {0, 0, 2, 0, 0},
    {2, 0, 2, -7, 192},
    {2, -7, 2, -10, 192},
    {1, -5, -4, -5, 128},
    {3, -5, 8, -5, 0},
    {0, 0, 0, 6, 64},
};

inline constexpr GoldenTransform case_1_expected[] = {
    {0, 0, 2, 0, 0},
    {2, 0, 2, -7, 192},
    {2, -7, 2, -10, 201},
    {1, -5, -4, -5, 128},
    {3, -5, -1, -9, 160},
    {0, 0, 0, 6, 64},
};

inline constexpr GoldenTransform case_2_expected[] = {
    {0, 5, 2, 5, 0},
    {2, 5, 4, -2, 205},
    {4, -2, 4, -5, 205},
    {2, -1, -3, -3, 141},
    {4, 0, 8, 1, 13},
    {0, 5, -4, 9, 89},
};

inline constexpr GoldenTransform case_3_expected[] = {
    {0, 0, 2, 0, 0},
    {2, 0, 0, -7, 181},
    {0, -7, -2, -10, 174},
    {-1, -5, -4, -10, 167},
    {1, -6, 1, -11, 195},
    {0, 0, 0, 6, 64},
};

inline constexpr GoldenTransform case_4_expected[] = {
    {0, 0, 2, 0, 0},
    {2, 0, 1, -7, 191},
    {1, -7, 1, -10, 193},
    {0, -5, -5, -8, 150},
    {2, -6, -1, -11, 169},
    {0, 0, 0, 6, 64},
};

inline constexpr GoldenCase cases[] = {
    {"rest", 0, -1, 0, case_0_expected},
    {"raised arm", 1, -1, 0, case_1_expected},
    {"crouch", 2, -1, 0, case_2_expected},
    {"stretch", 3, -1, 0, case_3_expected},
    {"non-trivial blend", 3, 4, 93, case_4_expected},
};

inline constexpr uint8_t boneCount = 6;
inline constexpr uint8_t poseCount = 5;
inline constexpr uint8_t caseCount = 5;

}}}  // namespace cf::gfx::riggolden

#endif  // CF_GFX_RIG_GOLDEN_H
