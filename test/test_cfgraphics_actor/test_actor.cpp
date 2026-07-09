// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

// test/test_cfgraphics_actor/test_actor.cpp
//
// Actor (lib/CFGraphics): Q8.8 fixed-point position math, sheet binding,
// name/pointer play() with idempotency, findAnimation hit/miss, and
// pivot-aware draw math against the HOST_TEST recording DisplayProxy
// (asserts the exact coordinates handed to drawXbm, REQ-OUT-001).

#include <unity.h>

#include "cf_gfx.h"
#include "cf_gfx_test_asset.h"

using namespace cf::gfx;
using namespace cf::gfx::testasset;

// ---------------------------------------------------------------------
// Q8.8 position math
// ---------------------------------------------------------------------

void test_default_state(void) {
    Actor a;
    TEST_ASSERT_EQUAL_INT16(0, a.x());
    TEST_ASSERT_EQUAL_INT16(0, a.y());
    TEST_ASSERT_NULL(a.sheet());
    TEST_ASSERT_NULL(a.sprite());
    TEST_ASSERT_NULL(a.animation());
    TEST_ASSERT_TRUE(a.finished());
    TEST_ASSERT_FALSE(a.play("walk", 0));  // no sheet bound
    TEST_ASSERT_FALSE(a.update(100));
}

void test_setpos_roundtrip(void) {
    Actor a;
    a.setPos(10, 48);
    TEST_ASSERT_EQUAL_INT16(10, a.x());
    TEST_ASSERT_EQUAL_INT16(48, a.y());
    TEST_ASSERT_EQUAL_INT32(10 * 256, a.xQ8());
    TEST_ASSERT_EQUAL_INT32(48 * 256, a.yQ8());
    a.setPos(-5, -64);
    TEST_ASSERT_EQUAL_INT16(-5, a.x());
    TEST_ASSERT_EQUAL_INT16(-64, a.y());
}

void test_q8_subpixel_truncates_toward_neg_infinity(void) {
    Actor a;
    a.setPosQ8(10 * 256 + 128, -128);  // (10.5, -0.5)
    TEST_ASSERT_EQUAL_INT16(10, a.x());  // floor(10.5) = 10
    TEST_ASSERT_EQUAL_INT16(-1, a.y());  // floor(-0.5) = -1 (arithmetic shift)
    a.setPosQ8(-384, 255);  // (-1.5, 0.996)
    TEST_ASSERT_EQUAL_INT16(-2, a.x());  // floor(-1.5) = -2
    TEST_ASSERT_EQUAL_INT16(0, a.y());
}

void test_q8_accumulation(void) {
    // The Q8 getters/setters support fixed-point motion: += 0.25 px/tick
    // crosses a pixel boundary every 4th tick.
    Actor a;
    a.setPos(20, 0);
    for (int i = 0; i < 3; ++i) {
        a.setPosQ8(a.xQ8() + 64, a.yQ8());
        TEST_ASSERT_EQUAL_INT16(20, a.x());
    }
    a.setPosQ8(a.xQ8() + 64, a.yQ8());
    TEST_ASSERT_EQUAL_INT16(21, a.x());
}

// ---------------------------------------------------------------------
// findAnimation + play()
// ---------------------------------------------------------------------

void test_find_animation_hit_and_miss(void) {
    TEST_ASSERT_EQUAL_PTR(anim_walk, findAnimation(sheet, "walk"));
    TEST_ASSERT_EQUAL_PTR(anim_pace, findAnimation(sheet, "pace"));
    TEST_ASSERT_NULL(findAnimation(sheet, "nope"));
    TEST_ASSERT_NULL(findAnimation(sheet, ""));
    TEST_ASSERT_NULL(findAnimation(sheet, nullptr));
}

void test_play_by_name_hit(void) {
    Actor a;
    a.setSheet(&sheet);
    TEST_ASSERT_TRUE(a.play("walk", 0));
    TEST_ASSERT_EQUAL_PTR(anim_walk, a.animation());
    TEST_ASSERT_EQUAL_PTR(&cel_a, a.sprite());
    TEST_ASSERT_FALSE(a.finished());
}

void test_play_by_name_miss_is_noop(void) {
    Actor a;
    a.setSheet(&sheet);
    a.play("walk", 0);
    a.update(100);  // frame 1
    TEST_ASSERT_FALSE(a.play("wlak", 150));  // typo: silent no-op
    TEST_ASSERT_EQUAL_PTR(anim_walk, a.animation());
    TEST_ASSERT_EQUAL_PTR(&cel_b, a.sprite());  // playback untouched
}

void test_play_by_name_idempotent(void) {
    Actor a;
    a.setSheet(&sheet);
    TEST_ASSERT_TRUE(a.play("walk", 0));
    a.update(100);  // frame 1
    TEST_ASSERT_TRUE(a.play("walk", 150));  // same name twice: no restart
    TEST_ASSERT_EQUAL_PTR(&cel_b, a.sprite());
    TEST_ASSERT_TRUE(a.update(200));  // original timing still governs
    TEST_ASSERT_EQUAL_PTR(&cel_c, a.sprite());
}

void test_play_by_pointer(void) {
    Actor a;  // no sheet needed for the pointer overload
    TEST_ASSERT_TRUE(a.play(anim_blink, 0));
    TEST_ASSERT_EQUAL_PTR(anim_blink, a.animation());
    TEST_ASSERT_FALSE(a.play((const Animation*)nullptr, 10));
    TEST_ASSERT_EQUAL_PTR(anim_blink, a.animation());  // nullptr: no-op
}

void test_play_by_pointer_idempotent(void) {
    Actor a;
    a.play(anim_walk, 0);
    a.update(100);  // frame 1
    TEST_ASSERT_TRUE(a.play(anim_walk, 150));  // same pointer: no restart
    TEST_ASSERT_EQUAL_PTR(&cel_b, a.sprite());
}

void test_play_name_and_pointer_share_identity(void) {
    // Fixture aliases anim_* INTO the sheet's animations[] array, so the
    // name path and pointer path resolve to the same address — switching
    // between them must not restart playback.
    Actor a;
    a.setSheet(&sheet);
    a.play("walk", 0);
    a.update(100);  // frame 1
    TEST_ASSERT_TRUE(a.play(anim_walk, 150));
    TEST_ASSERT_EQUAL_PTR(&cel_b, a.sprite());
}

void test_play_switches_animation(void) {
    Actor a;
    a.setSheet(&sheet);
    a.play("walk", 0);
    a.update(100);
    TEST_ASSERT_TRUE(a.play("pose", 150));  // different anim: restart at 0
    TEST_ASSERT_EQUAL_PTR(anim_pose, a.animation());
    TEST_ASSERT_EQUAL_PTR(&cel_a, a.sprite());
}

// ---------------------------------------------------------------------
// Pivot-aware draw (recording DisplayProxy)
// ---------------------------------------------------------------------

void test_draw_applies_pivot(void) {
    DisplayProxy d;
    Actor a;
    a.setSheet(&sheet);
    a.play("walk", 0);       // cel_a: 8x8, pivot (4, 8)
    a.setPos(50, 32);        // pivot lands at (50, 32)
    a.draw(d);
    TEST_ASSERT_EQUAL_INT(1, d.callCount);
    TEST_ASSERT_EQUAL_INT16(46, d.calls[0].x);  // 50 - 4
    TEST_ASSERT_EQUAL_INT16(24, d.calls[0].y);  // 32 - 8
    TEST_ASSERT_EQUAL_INT16(8, d.calls[0].w);
    TEST_ASSERT_EQUAL_INT16(8, d.calls[0].h);
    TEST_ASSERT_EQUAL_PTR(cel_a_bits, d.calls[0].data);
}

void test_draw_short_cel_same_ground_anchor(void) {
    // The pivot's whole point (N-033 Q&A): a shorter "duck" cel drawn at
    // the same Actor position keeps its feet on the same ground line.
    DisplayProxy d;
    Actor a;
    a.setSheet(&sheet);
    a.setPos(50, 48);        // feet at y=48
    a.play("duck", 0);       // cel_duck: 8x4, pivot (4, 4)
    a.draw(d);
    TEST_ASSERT_EQUAL_INT(1, d.callCount);
    TEST_ASSERT_EQUAL_INT16(46, d.calls[0].x);
    TEST_ASSERT_EQUAL_INT16(44, d.calls[0].y);  // 48 - 4: bottom edge at 48
}

void test_draw_without_animation_draws_nothing(void) {
    DisplayProxy d;
    Actor a;
    a.setPos(10, 10);
    a.draw(d);
    TEST_ASSERT_EQUAL_INT(0, d.callCount);
}

void test_draw_after_once_finished_draws_nothing(void) {
    DisplayProxy d;
    Actor a;
    a.setSheet(&sheet);
    a.play("blink", 0);  // LOOP_ONCE
    a.update(50);
    a.update(200);  // finished -> sprite() == nullptr
    TEST_ASSERT_TRUE(a.finished());
    a.draw(d);
    TEST_ASSERT_EQUAL_INT(0, d.callCount);
}

void test_draw_sprite_free_functions(void) {
    DisplayProxy d;
    drawSprite(d, cel_a, 30, 20);  // top-left form: pivot ignored
    TEST_ASSERT_EQUAL_INT16(30, d.calls[0].x);
    TEST_ASSERT_EQUAL_INT16(20, d.calls[0].y);
    drawSpritePivoted(d, cel_a, 30, 20);  // pivot (4, 8) applied
    TEST_ASSERT_EQUAL_INT16(26, d.calls[1].x);
    TEST_ASSERT_EQUAL_INT16(12, d.calls[1].y);
    TEST_ASSERT_EQUAL_INT(2, d.callCount);
}

void test_character_binding(void) {
    // Character -> sheet -> Actor: the skin-swap entry point.
    Actor a;
    a.setSheet(testasset_character.sheet);
    TEST_ASSERT_TRUE(a.play("walk", 0));
    TEST_ASSERT_EQUAL_STRING("testasset", testasset_character.name);
    TEST_ASSERT_NULL(testasset_character.skeleton);
}

void setUp(void)    {}
void tearDown(void) {}

int main(int /*argc*/, char** /*argv*/) {
    UNITY_BEGIN();
    RUN_TEST(test_default_state);
    RUN_TEST(test_setpos_roundtrip);
    RUN_TEST(test_q8_subpixel_truncates_toward_neg_infinity);
    RUN_TEST(test_q8_accumulation);
    RUN_TEST(test_find_animation_hit_and_miss);
    RUN_TEST(test_play_by_name_hit);
    RUN_TEST(test_play_by_name_miss_is_noop);
    RUN_TEST(test_play_by_name_idempotent);
    RUN_TEST(test_play_by_pointer);
    RUN_TEST(test_play_by_pointer_idempotent);
    RUN_TEST(test_play_name_and_pointer_share_identity);
    RUN_TEST(test_play_switches_animation);
    RUN_TEST(test_draw_applies_pivot);
    RUN_TEST(test_draw_short_cel_same_ground_anchor);
    RUN_TEST(test_draw_without_animation_draws_nothing);
    RUN_TEST(test_draw_after_once_finished_draws_nothing);
    RUN_TEST(test_draw_sprite_free_functions);
    RUN_TEST(test_character_binding);
    return UNITY_END();
}
