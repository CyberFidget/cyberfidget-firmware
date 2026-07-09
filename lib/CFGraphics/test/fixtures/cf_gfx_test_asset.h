// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

// lib/CFGraphics/test/fixtures/cf_gfx_test_asset.h
//
// Hand-authored test asset in the exact style the T-112 converter will
// emit into lib/<App>/generated/*.h — header-only `inline constexpr`
// PROGMEM data (N-033 § Generated output). Written by hand BEFORE the
// converter exists to validate that the pattern compiles and links.
// Consumed only by test/test_cfgraphics_*; no shipping app uses it.
//
// NOTE for T-112's codegen: emit ONE canonical `animations[]` array and
// alias per-animation pointers into it (as below). Do NOT emit duplicate
// standalone Animation objects — Actor::play / AnimationPlayer::setIfChanged
// compare Animation ADDRESSES, so duplicates would defeat the
// no-restart-when-unchanged semantics.

#ifndef CF_GFX_TEST_ASSET_H
#define CF_GFX_TEST_ASSET_H

#include "cf_gfx.h"

namespace cf { namespace gfx { namespace testasset {

// --- Cels (XBM: LSB-first, 8x8 -> 1 byte per row) ---------------------
inline constexpr uint8_t cel_a_bits[] PROGMEM = {
    0x18, 0x24, 0x42, 0x81, 0x81, 0x42, 0x24, 0x18};  // diamond
inline constexpr uint8_t cel_b_bits[] PROGMEM = {
    0x00, 0x3C, 0x24, 0x24, 0x24, 0x3C, 0x00, 0x00};  // small box
inline constexpr uint8_t cel_c_bits[] PROGMEM = {
    0xFF, 0x81, 0x81, 0x81, 0x81, 0x81, 0x81, 0xFF};  // frame
inline constexpr uint8_t cel_duck_bits[] PROGMEM = {
    0x3C, 0x7E, 0x7E, 0x3C};                          // 8x4 blob

// Pivot bottom-center (4, 8): "feet on the ground" anchoring, so the
// short duck cel (pivot 4, 4) lines up with the tall cels at the same
// Actor position — the scenario pivots exist for (N-033 Q&A).
inline constexpr Sprite cel_a PROGMEM = {cel_a_bits, 8, 8, 4, 8, BO_LSB_FIRST};
inline constexpr Sprite cel_b PROGMEM = {cel_b_bits, 8, 8, 4, 8, BO_LSB_FIRST};
inline constexpr Sprite cel_c PROGMEM = {cel_c_bits, 8, 8, 4, 8, BO_LSB_FIRST};
// Distinct Sprites may share a byte array (converter de-dups within an app).
inline constexpr Sprite cel_d PROGMEM = {cel_a_bits, 8, 8, 4, 8, BO_LSB_FIRST};
inline constexpr Sprite cel_e PROGMEM = {cel_b_bits, 8, 8, 4, 8, BO_LSB_FIRST};
inline constexpr Sprite cel_duck PROGMEM = {cel_duck_bits, 8, 4, 4, 4, BO_LSB_FIRST};

// --- Frame lists -------------------------------------------------------
inline constexpr const Sprite* const walk_frames[] PROGMEM = {&cel_a, &cel_b, &cel_c};
inline constexpr const Sprite* const blink_frames[] PROGMEM = {&cel_a, &cel_b};
inline constexpr uint16_t blink_durations[] PROGMEM = {50, 150};
inline constexpr const Sprite* const pose_frames[] PROGMEM = {&cel_a, &cel_b, &cel_c};
inline constexpr const Sprite* const pace_frames[] PROGMEM = {&cel_a, &cel_b, &cel_c,
                                                              &cel_d, &cel_e};
inline constexpr const Sprite* const duck_frames[] PROGMEM = {&cel_duck};

// --- Canonical Animation array (what findAnimation scans) --------------
inline constexpr Animation animations[] PROGMEM = {
    {"walk",  walk_frames,  nullptr,         100, 3, LOOP_LOOP},
    {"blink", blink_frames, blink_durations,   0, 2, LOOP_ONCE},
    {"pose",  pose_frames,  nullptr,          80, 3, LOOP_ONCE_HOLD},
    {"pace",  pace_frames,  nullptr,          60, 5, LOOP_PINGPONG},
    {"duck",  duck_frames,  nullptr,           0, 1, LOOP_ONCE_HOLD},  // fps-0 hold
};

// Pointer aliases INTO the array (see codegen note in the file header).
inline constexpr const Animation* anim_walk  = &animations[0];
inline constexpr const Animation* anim_blink = &animations[1];
inline constexpr const Animation* anim_pose  = &animations[2];
inline constexpr const Animation* anim_pace  = &animations[3];
inline constexpr const Animation* anim_duck  = &animations[4];

inline constexpr SpriteSheet sheet PROGMEM = {"testasset", animations, 5, "sprite"};

}}}  // namespace cf::gfx::testasset

namespace cf { namespace gfx {
inline constexpr Character testasset_character PROGMEM = {
    "testasset", &testasset::sheet, nullptr};
}}  // namespace cf::gfx

#endif  // CF_GFX_TEST_ASSET_H
