// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#ifndef RAGDOLL_FIDGIE_RIG_H
#define RAGDOLL_FIDGIE_RIG_H

#include "cf_gfx_rig.h"

namespace ragdoll_fidgie {

using namespace cf::gfx;

// Integer-grid re-authoring of the feasibility prototype. The fractional
// hip attachments [-2.5,+2.5] become the mirrored pair [-2,+2], and the
// breathe pose's rootDy 0.8 becomes 1. Both legs remain exactly symmetric
// at their attachment points.
inline constexpr RigBone bones[] PROGMEM = {
    {"pelvis", -1, 2, 0, 0, -90},
    {"spine", 0, 7, 2, 0, 0},
    {"head", 1, 5, 7, 0, 0},
    {"l_arm", 1, 5, 6, -3, 192},
    {"l_fore", 3, 5, 5, 0, -6},
    {"r_arm", 1, 5, 6, 3, 168},
    {"r_fore", 5, 5, 5, 0, 6},
    {"l_thigh", 0, 6, 0, -2, 185},
    {"l_shin", 7, 6, 6, 0, 4},
    {"r_thigh", 0, 6, 0, 2, 175},
    {"r_shin", 9, 6, 6, 0, -4},
};

inline constexpr int16_t restOffsets[] PROGMEM =
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
inline constexpr int16_t breatheOffsets[] PROGMEM =
    {0, 0, 3, 4, 0, -4, 0, 0, 0, 0, 0};
inline constexpr int16_t waveUpOffsets[] PROGMEM =
    {0, 0, 10, 8, 0, -138, -20, 0, 0, 0, 0};
inline constexpr int16_t waveDownOffsets[] PROGMEM =
    {0, 0, 10, 8, 0, -138, -62, 0, 0, 0, 0};
inline constexpr int16_t danceAOffsets[] PROGMEM =
    {0, 0, -8, 24, 0, 24, 0, 38, -52, 0, 0};
inline constexpr int16_t danceBOffsets[] PROGMEM =
    {0, 0, 8, -24, 0, -24, 0, 0, 0, -38, 52};
inline constexpr int16_t crouchOffsets[] PROGMEM =
    {0, 0, 6, 30, 0, -30, 0, 40, -62, -40, 62};
inline constexpr int16_t stretchOffsets[] PROGMEM =
    {0, 0, -2, 148, 6, -148, -6, 0, 0, 0, 0};

enum PoseIndex : uint8_t {
    POSE_REST,
    POSE_BREATHE,
    POSE_WAVE_UP,
    POSE_WAVE_DOWN,
    POSE_DANCE_A,
    POSE_DANCE_B,
    POSE_CROUCH,
    POSE_STRETCH,
};

inline constexpr RigPose poses[] PROGMEM = {
    {"rest", restOffsets, 0},
    {"breathe", breatheOffsets, 1},
    {"wave_up", waveUpOffsets, 0},
    {"wave_down", waveDownOffsets, 0},
    {"dance_a", danceAOffsets, -1},
    {"dance_b", danceBOffsets, -1},
    {"crouch", crouchOffsets, 6},
    {"stretch", stretchOffsets, -2},
};

inline constexpr const RigPose* idleKeys[] PROGMEM =
    {&poses[POSE_REST], &poses[POSE_BREATHE]};
inline constexpr const RigPose* waveKeys[] PROGMEM =
    {&poses[POSE_WAVE_UP], &poses[POSE_WAVE_DOWN]};
inline constexpr const RigPose* danceKeys[] PROGMEM =
    {&poses[POSE_DANCE_A], &poses[POSE_DANCE_B]};
inline constexpr uint16_t idleDurations[] PROGMEM = {950, 950};
inline constexpr uint16_t waveDurations[] PROGMEM = {230, 230};
inline constexpr uint16_t danceDurations[] PROGMEM = {250, 250};

inline constexpr RigAnimation animations[] PROGMEM = {
    {"idle", idleKeys, idleDurations, 2, true},
    {"wave", waveKeys, waveDurations, 2, true},
    {"dance", danceKeys, danceDurations, 2, true},
};

inline constexpr RigPart parts[] PROGMEM = {
    {7, RIG_RENDER_LINE, nullptr, 0, 0, 0, 0},
    {8, RIG_RENDER_LINE, nullptr, 0, 0, 0, 0},
    {9, RIG_RENDER_LINE, nullptr, 0, 0, 0, 0},
    {10, RIG_RENDER_LINE, nullptr, 0, 0, 0, 0},
    {3, RIG_RENDER_LINE, nullptr, 0, 0, 0, 0},
    {4, RIG_RENDER_LINE, nullptr, 0, 0, 0, 0},
    {5, RIG_RENDER_LINE, nullptr, 0, 0, 0, 0},
    {6, RIG_RENDER_LINE, nullptr, 0, 0, 0, 0},
    {1, RIG_RENDER_FLAT, nullptr, 0, 0, 0, 0},
    {2, RIG_RENDER_LINE, nullptr, 0, 0, 0, 0},
};

inline constexpr Rig rig PROGMEM = {
    "ragdoll-fidgie",
    bones,
    11,
    parts,
    10,
    poses,
    8,
    animations,
    3,
};

}  // namespace ragdoll_fidgie

#endif  // RAGDOLL_FIDGIE_RIG_H
