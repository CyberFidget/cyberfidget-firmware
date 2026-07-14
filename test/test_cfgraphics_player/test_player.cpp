// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

// test/test_cfgraphics_player/test_player.cpp
//
// AnimationPlayer time -> frame state machine (lib/CFGraphics). Pins the
// timing contract documented in cf_gfx_actor.h: frame advance at exactly
// epoch + duration, multi-frame catch-up, all four LoopModes including
// the once-hold and ping-pong edges, fps-0 holds, stop/restart semantics.
// Fixture asset: lib/CFGraphics/test/fixtures/cf_gfx_test_asset.h.

#include <unity.h>

#include "cf_gfx.h"
#include "cf_gfx_test_asset.h"

using namespace cf::gfx;
using namespace cf::gfx::testasset;

void test_initial_state(void) {
    AnimationPlayer p;
    TEST_ASSERT_NULL(p.sprite());
    TEST_ASSERT_NULL(p.animation());
    TEST_ASSERT_TRUE(p.finished());  // "nothing in flight" reads as finished
    TEST_ASSERT_FALSE(p.update(12345));
}

void test_play_starts_at_frame0(void) {
    AnimationPlayer p;
    p.play(anim_walk, 1000);
    TEST_ASSERT_EQUAL_PTR(anim_walk, p.animation());
    TEST_ASSERT_EQUAL_UINT8(0, p.frameIndex());
    TEST_ASSERT_EQUAL_PTR(&cel_a, p.sprite());
    TEST_ASSERT_FALSE(p.finished());
}

void test_loop_advances_at_exact_boundary(void) {
    AnimationPlayer p;
    p.play(anim_walk, 1000);  // uniform 100 ms
    TEST_ASSERT_FALSE(p.update(1099));
    TEST_ASSERT_EQUAL_UINT8(0, p.frameIndex());
    TEST_ASSERT_TRUE(p.update(1100));  // change happens AT epoch + duration
    TEST_ASSERT_EQUAL_UINT8(1, p.frameIndex());
    TEST_ASSERT_EQUAL_PTR(&cel_b, p.sprite());
}

void test_loop_wraps_to_zero(void) {
    AnimationPlayer p;
    p.play(anim_walk, 0);
    TEST_ASSERT_TRUE(p.update(100));
    TEST_ASSERT_TRUE(p.update(200));
    TEST_ASSERT_EQUAL_UINT8(2, p.frameIndex());
    TEST_ASSERT_TRUE(p.update(300));  // wrap
    TEST_ASSERT_EQUAL_UINT8(0, p.frameIndex());
    TEST_ASSERT_FALSE(p.finished());  // LOOP never finishes
}

void test_loop_catches_up_multiple_frames(void) {
    AnimationPlayer p;
    p.play(anim_walk, 1000);
    TEST_ASSERT_TRUE(p.update(1250));  // 2.5 frames elapsed -> frame 2
    TEST_ASSERT_EQUAL_UINT8(2, p.frameIndex());
    // Residual 50 ms carries over: next boundary is at 1300, not 1350.
    TEST_ASSERT_FALSE(p.update(1299));
    TEST_ASSERT_TRUE(p.update(1300));
    TEST_ASSERT_EQUAL_UINT8(0, p.frameIndex());
}

void test_loop_full_cycle_reports_unchanged(void) {
    AnimationPlayer p;
    p.play(anim_walk, 0);
    // Exactly one full cycle (3 x 100 ms): ends back on frame 0 — the
    // sprite to draw did not change, so update() reports false.
    TEST_ASSERT_FALSE(p.update(300));
    TEST_ASSERT_EQUAL_UINT8(0, p.frameIndex());
    // ...but the epochs were consumed: next advance is at 400.
    TEST_ASSERT_TRUE(p.update(400));
    TEST_ASSERT_EQUAL_UINT8(1, p.frameIndex());
}

void test_per_frame_durations(void) {
    AnimationPlayer p;
    p.play(anim_blink, 0);  // durations {50, 150}, LOOP_ONCE
    TEST_ASSERT_FALSE(p.update(49));
    TEST_ASSERT_TRUE(p.update(50));  // frame 0 lasts 50 ms
    TEST_ASSERT_EQUAL_UINT8(1, p.frameIndex());
    TEST_ASSERT_FALSE(p.update(199));  // frame 1 lasts 150 ms
    TEST_ASSERT_FALSE(p.finished());
}

void test_once_stops_and_hides_sprite(void) {
    AnimationPlayer p;
    p.play(anim_blink, 0);
    TEST_ASSERT_TRUE(p.update(50));
    TEST_ASSERT_TRUE(p.update(200));  // last frame's duration elapsed
    TEST_ASSERT_TRUE(p.finished());
    TEST_ASSERT_NULL(p.sprite());     // LOOP_ONCE: "then stop" — draw nothing
    // animation() still reports what finished (setIfChanged stays a no-op).
    TEST_ASSERT_EQUAL_PTR(anim_blink, p.animation());
    TEST_ASSERT_FALSE(p.update(9999));
}

void test_once_hold_keeps_last_frame(void) {
    AnimationPlayer p;
    p.play(anim_pose, 0);  // 3 frames, uniform 80 ms, LOOP_ONCE_HOLD
    TEST_ASSERT_TRUE(p.update(80));
    TEST_ASSERT_TRUE(p.update(160));
    TEST_ASSERT_EQUAL_UINT8(2, p.frameIndex());
    TEST_ASSERT_FALSE(p.finished());
    // Last frame's duration elapses: finished, but the sprite HOLDS —
    // nothing visible changed, so update() reports false.
    TEST_ASSERT_FALSE(p.update(240));
    TEST_ASSERT_TRUE(p.finished());
    TEST_ASSERT_EQUAL_UINT8(2, p.frameIndex());
    TEST_ASSERT_EQUAL_PTR(&cel_c, p.sprite());
    TEST_ASSERT_FALSE(p.update(100000));  // holds forever
    TEST_ASSERT_EQUAL_PTR(&cel_c, p.sprite());
}

void test_pingpong_sequence(void) {
    // 5 frames, uniform 60 ms: 0,1,2,3,4,3,2,1,0,1,... — each end is shown
    // for exactly one duration per pass (N-033: "0..N-1..1 then repeat").
    static const uint8_t kExpected[] = {1, 2, 3, 4, 3, 2, 1, 0, 1, 2, 3, 4, 3, 2};
    AnimationPlayer p;
    p.play(anim_pace, 0);
    TEST_ASSERT_EQUAL_UINT8(0, p.frameIndex());
    uint32_t t = 0;
    for (unsigned i = 0; i < sizeof(kExpected); ++i) {
        t += 60;
        TEST_ASSERT_TRUE(p.update(t));
        TEST_ASSERT_EQUAL_UINT8(kExpected[i], p.frameIndex());
    }
    TEST_ASSERT_FALSE(p.finished());  // PINGPONG never finishes
}

void test_pingpong_two_frames_alternates(void) {
    static const Sprite* const frames[] = {&cel_a, &cel_b};
    static const Animation anim = {"pp2", frames, nullptr, 10, 2, LOOP_PINGPONG};
    AnimationPlayer p;
    p.play(&anim, 0);
    static const uint8_t kExpected[] = {1, 0, 1, 0};
    uint32_t t = 0;
    for (unsigned i = 0; i < sizeof(kExpected); ++i) {
        t += 10;
        TEST_ASSERT_TRUE(p.update(t));
        TEST_ASSERT_EQUAL_UINT8(kExpected[i], p.frameIndex());
    }
}

void test_pingpong_single_frame_holds(void) {
    static const Sprite* const frames[] = {&cel_a};
    static const Animation anim = {"pp1", frames, nullptr, 10, 1, LOOP_PINGPONG};
    AnimationPlayer p;
    p.play(&anim, 0);
    TEST_ASSERT_FALSE(p.update(1000));  // degenerate: nothing can change
    TEST_ASSERT_EQUAL_UINT8(0, p.frameIndex());
    TEST_ASSERT_EQUAL_PTR(&cel_a, p.sprite());
    TEST_ASSERT_FALSE(p.finished());
}

void test_zero_duration_holds_indefinitely(void) {
    AnimationPlayer p;
    p.play(anim_duck, 0);  // uniformMs = 0 (fps-0 pose), LOOP_ONCE_HOLD
    TEST_ASSERT_FALSE(p.update(1000000));
    TEST_ASSERT_EQUAL_UINT8(0, p.frameIndex());
    TEST_ASSERT_EQUAL_PTR(&cel_duck, p.sprite());
    TEST_ASSERT_FALSE(p.finished());
}

void test_set_if_changed_same_anim_no_restart(void) {
    AnimationPlayer p;
    p.play(anim_walk, 0);
    p.update(100);
    TEST_ASSERT_EQUAL_UINT8(1, p.frameIndex());
    p.setIfChanged(anim_walk, 150);  // same Animation: must NOT restart
    TEST_ASSERT_EQUAL_UINT8(1, p.frameIndex());
    TEST_ASSERT_TRUE(p.update(200));  // original epoch still governs
    TEST_ASSERT_EQUAL_UINT8(2, p.frameIndex());
}

void test_set_if_changed_new_anim_restarts(void) {
    AnimationPlayer p;
    p.play(anim_walk, 0);
    p.update(100);
    p.setIfChanged(anim_pose, 130);
    TEST_ASSERT_EQUAL_PTR(anim_pose, p.animation());
    TEST_ASSERT_EQUAL_UINT8(0, p.frameIndex());
    TEST_ASSERT_FALSE(p.update(209));  // new epoch is 130
    TEST_ASSERT_TRUE(p.update(210));
}

void test_play_always_restarts(void) {
    AnimationPlayer p;
    p.play(anim_blink, 0);
    p.update(50);
    p.update(200);  // finished
    TEST_ASSERT_TRUE(p.finished());
    p.play(anim_blink, 300);  // explicit play() restarts even the same anim
    TEST_ASSERT_FALSE(p.finished());
    TEST_ASSERT_EQUAL_UINT8(0, p.frameIndex());
    TEST_ASSERT_EQUAL_PTR(&cel_a, p.sprite());
}

void test_stop_clears_playback(void) {
    AnimationPlayer p;
    p.play(anim_walk, 0);
    p.update(100);
    p.stop();
    TEST_ASSERT_NULL(p.sprite());
    TEST_ASSERT_NULL(p.animation());
    TEST_ASSERT_TRUE(p.finished());
    TEST_ASSERT_FALSE(p.update(1000));
}

void test_play_nullptr_is_stop(void) {
    AnimationPlayer p;
    p.play(anim_walk, 0);
    p.play(nullptr, 100);
    TEST_ASSERT_NULL(p.sprite());
    TEST_ASSERT_TRUE(p.finished());
    TEST_ASSERT_FALSE(p.update(1000));
}

void test_large_gap_catchup_lands_on_modulo_frame(void) {
    AnimationPlayer p;
    p.play(anim_walk, 0);          // 3 frames x 100 ms
    TEST_ASSERT_TRUE(p.update(10000));  // 100 advances; 100 % 3 == 1
    TEST_ASSERT_EQUAL_UINT8(1, p.frameIndex());
}

void setUp(void)    {}
void tearDown(void) {}

int main(int /*argc*/, char** /*argv*/) {
    UNITY_BEGIN();
    RUN_TEST(test_initial_state);
    RUN_TEST(test_play_starts_at_frame0);
    RUN_TEST(test_loop_advances_at_exact_boundary);
    RUN_TEST(test_loop_wraps_to_zero);
    RUN_TEST(test_loop_catches_up_multiple_frames);
    RUN_TEST(test_loop_full_cycle_reports_unchanged);
    RUN_TEST(test_per_frame_durations);
    RUN_TEST(test_once_stops_and_hides_sprite);
    RUN_TEST(test_once_hold_keeps_last_frame);
    RUN_TEST(test_pingpong_sequence);
    RUN_TEST(test_pingpong_two_frames_alternates);
    RUN_TEST(test_pingpong_single_frame_holds);
    RUN_TEST(test_zero_duration_holds_indefinitely);
    RUN_TEST(test_set_if_changed_same_anim_no_restart);
    RUN_TEST(test_set_if_changed_new_anim_restarts);
    RUN_TEST(test_play_always_restarts);
    RUN_TEST(test_stop_clears_playback);
    RUN_TEST(test_play_nullptr_is_stop);
    RUN_TEST(test_large_gap_catchup_lands_on_modulo_frame);
    return UNITY_END();
}
