// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

// test/test_cfgraphics_collision/test_collision.cpp
//
// sampleSprite + pixelCollides (lib/CFGraphics). Pins the bit-addressing
// contract EXPLICITLY: XBM order — row-major, ((w + 7) / 8) bytes per row,
// LSB-first within each byte — the same order DisplayProxy::drawXbm
// renders and the .cfsprite v1 profile packs (cyberfidget_website/
// assets/js/models/cfsprite.mjs). T-112's converter output must satisfy
// these tests byte-for-byte; if they ever disagree, the converter (not
// this contract) is wrong.

#include <unity.h>

#include "cf_gfx.h"
#include "cf_gfx_test_asset.h"

using namespace cf::gfx;

// --- Local fixtures with hand-checkable bit patterns -------------------

// 8x2, LSB-first: row 0 = 0x01 -> only x=0 lit; row 1 = 0x80 -> only x=7.
static const uint8_t kLsbProbeBits[] = {0x01, 0x80};
static const Sprite kLsbProbe = {kLsbProbeBits, 8, 2, 0, 0, BO_LSB_FIRST};

// 10x2 -> 2 bytes per row (XBM row padding). Row 0: 0xFF,0x03 -> x=0..9
// all lit (second byte's bits 0..1 are x=8..9). Row 1: 0x00,0x02 -> only
// x=9 lit. Pins the ((w + 7) / 8) row stride for non-byte-multiple widths.
static const uint8_t kPaddedBits[] = {0xFF, 0x03, 0x00, 0x02};
static const Sprite kPadded = {kPaddedBits, 10, 2, 0, 0, BO_LSB_FIRST};

// Same single byte as row 0 of the LSB probe, declared MSB-first:
// 0x01 -> only x=7 lit.
static const uint8_t kMsbProbeBits[] = {0x01};
static const Sprite kMsbProbe = {kMsbProbeBits, 8, 1, 0, 0, BO_MSB_FIRST};

// 8x8 fully solid block.
static const uint8_t kSolidBits[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
static const Sprite kSolid = {kSolidBits, 8, 8, 0, 0, BO_LSB_FIRST};

// 8x8 with ONLY the top-left pixel (0,0) lit.
static const uint8_t kDotTLBits[] = {0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
static const Sprite kDotTL = {kDotTLBits, 8, 8, 0, 0, BO_LSB_FIRST};

// 8x8 with ONLY the bottom-right pixel (7,7) lit.
static const uint8_t kDotBRBits[] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80};
static const Sprite kDotBR = {kDotBRBits, 8, 8, 0, 0, BO_LSB_FIRST};

// Solid 8x8 with a bottom-center pivot (4, 8), for the Actor overload.
static const Sprite kSolidPivoted = {kSolidBits, 8, 8, 4, 8, BO_LSB_FIRST};

// ---------------------------------------------------------------------
// sampleSprite — bit addressing
// ---------------------------------------------------------------------

void test_sample_lsb_first_bit_addressing(void) {
    // Byte 0x01: bit 0 (LSB) is pixel x=0 — NOT x=7.
    TEST_ASSERT_TRUE(sampleSprite(kLsbProbe, 0, 0));
    for (int16_t x = 1; x < 8; ++x) {
        TEST_ASSERT_FALSE(sampleSprite(kLsbProbe, x, 0));
    }
    // Byte 0x80: bit 7 (MSB) is pixel x=7.
    TEST_ASSERT_TRUE(sampleSprite(kLsbProbe, 7, 1));
    for (int16_t x = 0; x < 7; ++x) {
        TEST_ASSERT_FALSE(sampleSprite(kLsbProbe, x, 1));
    }
}

void test_sample_row_stride_is_padded_to_bytes(void) {
    // w=10 -> 2 bytes per row; row 1 starts at byte 2, not at bit 10.
    for (int16_t x = 0; x < 10; ++x) {
        TEST_ASSERT_TRUE(sampleSprite(kPadded, x, 0));
    }
    TEST_ASSERT_TRUE(sampleSprite(kPadded, 9, 1));   // 0x02: bit 1 -> x = 8+1
    TEST_ASSERT_FALSE(sampleSprite(kPadded, 8, 1));
    for (int16_t x = 0; x < 8; ++x) {
        TEST_ASSERT_FALSE(sampleSprite(kPadded, x, 1));
    }
}

void test_sample_out_of_bounds_is_off(void) {
    TEST_ASSERT_FALSE(sampleSprite(kLsbProbe, -1, 0));
    TEST_ASSERT_FALSE(sampleSprite(kLsbProbe, 0, -1));
    TEST_ASSERT_FALSE(sampleSprite(kLsbProbe, 8, 0));   // x == w
    TEST_ASSERT_FALSE(sampleSprite(kLsbProbe, 0, 2));   // y == h
    TEST_ASSERT_FALSE(sampleSprite(kPadded, 10, 0));    // x == w (padded row)
}

void test_sample_msb_first_legacy_order(void) {
    // Same byte 0x01 read as MSB-first flips horizontally: only x=7 lit.
    TEST_ASSERT_TRUE(sampleSprite(kMsbProbe, 7, 0));
    TEST_ASSERT_FALSE(sampleSprite(kMsbProbe, 0, 0));
}

void test_sample_null_data_is_off(void) {
    const Sprite empty = {nullptr, 8, 8, 0, 0, BO_LSB_FIRST};
    TEST_ASSERT_FALSE(sampleSprite(empty, 0, 0));
}

// ---------------------------------------------------------------------
// pixelCollides(Sprite, Sprite) — top-left coordinates
// ---------------------------------------------------------------------

void test_collide_solid_overlap_and_separation(void) {
    // Two solid 8x8 blocks; A at origin spans x=0..7, y=0..7.
    TEST_ASSERT_TRUE(pixelCollides(kSolid, 0, 0, kSolid, 7, 7));   // 1px corner
    TEST_ASSERT_FALSE(pixelCollides(kSolid, 0, 0, kSolid, 8, 0));  // 1px apart (x)
    TEST_ASSERT_FALSE(pixelCollides(kSolid, 0, 0, kSolid, 0, 8));  // 1px apart (y)
    TEST_ASSERT_TRUE(pixelCollides(kSolid, 0, 0, kSolid, 7, 0));   // edge column
    TEST_ASSERT_FALSE(pixelCollides(kSolid, 0, 0, kSolid, -8, 0)); // apart (left)
}

void test_collide_aabb_overlap_but_pixels_miss(void) {
    // AABBs fully overlap, but the single lit pixels don't coincide.
    TEST_ASSERT_FALSE(pixelCollides(kDotTL, 0, 0, kDotBR, 0, 0));
    // Shift B so its (7,7) pixel lands on A's (0,0) pixel: exact hit.
    TEST_ASSERT_TRUE(pixelCollides(kDotTL, 0, 0, kDotBR, -7, -7));
    // One pixel off in each axis: miss again.
    TEST_ASSERT_FALSE(pixelCollides(kDotTL, 0, 0, kDotBR, -6, -7));
    TEST_ASSERT_FALSE(pixelCollides(kDotTL, 0, 0, kDotBR, -7, -6));
}

void test_collide_single_pixel_dots(void) {
    // Two top-left dots collide only at identical positions.
    TEST_ASSERT_TRUE(pixelCollides(kDotTL, 5, 5, kDotTL, 5, 5));
    TEST_ASSERT_FALSE(pixelCollides(kDotTL, 5, 5, kDotTL, 6, 5));
}

void test_collide_null_or_empty_is_false(void) {
    const Sprite empty = {nullptr, 8, 8, 0, 0, BO_LSB_FIRST};
    const Sprite zeroW = {kSolidBits, 0, 8, 0, 0, BO_LSB_FIRST};
    TEST_ASSERT_FALSE(pixelCollides(empty, 0, 0, kSolid, 0, 0));
    TEST_ASSERT_FALSE(pixelCollides(kSolid, 0, 0, zeroW, 0, 0));
}

// ---------------------------------------------------------------------
// pixelCollides(Actor, Actor) — pivot-anchored positions
// ---------------------------------------------------------------------

static void playSolidPivoted(Actor& a, int16_t x, int16_t y) {
    static const Sprite* const frames[] = {&kSolidPivoted};
    static const Animation anim = {"solid", frames, nullptr, 100, 1, LOOP_LOOP};
    a.play(&anim, 0);
    a.setPos(x, y);
}

void test_actor_collision_matches_manual_pivot_adjustment(void) {
    // N-033 test list: the Actor overload must equal the Sprite overload
    // called with manually pivot-adjusted (pos - pivot) top-left coords.
    struct Case { int16_t ax, ay, bx, by; };
    static const Case kCases[] = {
        {50, 32, 57, 32},  // touching at the last overlap column
        {50, 32, 58, 32},  // one px apart
        {50, 32, 50, 32},  // fully stacked
        {50, 32, 57, 39},  // corner
        {50, 32, 42, 25},  // offset up-left
    };
    for (const Case& c : kCases) {
        Actor a, b;
        playSolidPivoted(a, c.ax, c.ay);
        playSolidPivoted(b, c.bx, c.by);
        const bool viaActors = pixelCollides(a, b);
        const bool manual = pixelCollides(
            kSolidPivoted, (int16_t)(c.ax - kSolidPivoted.pivotX),
            (int16_t)(c.ay - kSolidPivoted.pivotY),
            kSolidPivoted, (int16_t)(c.bx - kSolidPivoted.pivotX),
            (int16_t)(c.by - kSolidPivoted.pivotY));
        TEST_ASSERT_EQUAL(manual, viaActors);
    }
}

void test_actor_collision_boundary_with_pivots(void) {
    // Pivoted solids are 8 wide (pivot x=4): pivots 8 apart -> 1px overlap
    // at the shared edge; 9 apart -> clean miss.
    Actor a, b;
    playSolidPivoted(a, 50, 32);
    playSolidPivoted(b, 57, 32);
    TEST_ASSERT_TRUE(pixelCollides(a, b));
    playSolidPivoted(b, 58, 32);
    TEST_ASSERT_FALSE(pixelCollides(a, b));
}

void test_actor_collision_without_sprites_is_false(void) {
    Actor a, b;
    a.setPos(10, 10);
    b.setPos(10, 10);
    TEST_ASSERT_FALSE(pixelCollides(a, b));  // neither has a sprite
    playSolidPivoted(a, 10, 10);
    TEST_ASSERT_FALSE(pixelCollides(a, b));  // one still has none
}

void test_actor_collision_after_once_finished_is_false(void) {
    // A finished LOOP_ONCE animation shows nothing, so it collides with
    // nothing — collision agrees with what's on screen.
    using namespace cf::gfx::testasset;
    Actor a, b;
    a.setSheet(&sheet);
    a.play("blink", 0);  // LOOP_ONCE
    a.setPos(10, 10);
    playSolidPivoted(b, 10, 10);
    TEST_ASSERT_TRUE(pixelCollides(a, b));  // while playing: diamond vs solid
    a.update(50);
    a.update(200);  // finished -> sprite() == nullptr
    TEST_ASSERT_FALSE(pixelCollides(a, b));
}

void setUp(void)    {}
void tearDown(void) {}

int main(int /*argc*/, char** /*argv*/) {
    UNITY_BEGIN();
    RUN_TEST(test_sample_lsb_first_bit_addressing);
    RUN_TEST(test_sample_row_stride_is_padded_to_bytes);
    RUN_TEST(test_sample_out_of_bounds_is_off);
    RUN_TEST(test_sample_msb_first_legacy_order);
    RUN_TEST(test_sample_null_data_is_off);
    RUN_TEST(test_collide_solid_overlap_and_separation);
    RUN_TEST(test_collide_aabb_overlap_but_pixels_miss);
    RUN_TEST(test_collide_single_pixel_dots);
    RUN_TEST(test_collide_null_or_empty_is_false);
    RUN_TEST(test_actor_collision_matches_manual_pivot_adjustment);
    RUN_TEST(test_actor_collision_boundary_with_pivots);
    RUN_TEST(test_actor_collision_without_sprites_is_false);
    RUN_TEST(test_actor_collision_after_once_finished_is_false);
    return UNITY_END();
}
