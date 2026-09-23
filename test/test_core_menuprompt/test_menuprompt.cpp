// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

// test/test_core_menuprompt/test_menuprompt.cpp
//
// Pure state behind the shared menu primitives: ModalPromptModel (zero-based
// selection, wrap at both ends, visible-window scrolling, edge-triggered
// select, optional idle timeout -> no choice) and ScrollLabel's marquee math
// (short / exact-width / long text, step timing, restart, reset). No display
// and no buttons; ModalPrompt.cpp / ScrollLabel::draw are not compiled here.

#include <unity.h>
#include <stdint.h>

#include "../../lib/MenuManager/ModalPromptModel.h"
#include "../../lib/MenuManager/ScrollLabel.h"

static const int kRows = 4;  // ModalPrompt::kVisibleRows

void setUp(void) {}
void tearDown(void) {}

// The focused option must always be inside the drawn window.
static void assertSelectionVisible(const ModalPromptModel& m) {
    TEST_ASSERT_TRUE(m.selected() >= m.windowStart());
    TEST_ASSERT_TRUE(m.selected() < m.windowStart() + m.windowCount());
    TEST_ASSERT_TRUE(m.windowStart() >= 0);
    TEST_ASSERT_TRUE(m.windowStart() + m.windowCount() <= m.optionCount());
}

// ---------------------------------------------------------------- selection

void test_open_starts_zero_based_with_no_result(void) {
    ModalPromptModel m;
    TEST_ASSERT_TRUE(m.open(3, kRows, 0, 0));
    TEST_ASSERT_TRUE(m.isOpen());
    TEST_ASSERT_EQUAL_INT(0, m.selected());
    TEST_ASSERT_EQUAL_INT(0, m.windowStart());
    TEST_ASSERT_EQUAL_INT(3, m.windowCount());
    TEST_ASSERT_EQUAL_INT(ModalPromptModel::kNoChoice, m.result());
}

void test_open_refuses_empty_option_list(void) {
    ModalPromptModel m;
    TEST_ASSERT_FALSE(m.open(0, kRows, 0, 0));
    TEST_ASSERT_FALSE(m.open(-1, kRows, 0, 0));
    TEST_ASSERT_FALSE(m.isOpen());
}

void test_down_walks_to_last_then_wraps_to_first(void) {
    ModalPromptModel m;
    m.open(3, kRows, 0, 0);
    m.moveDown(1);
    TEST_ASSERT_EQUAL_INT(1, m.selected());
    m.moveDown(2);
    TEST_ASSERT_EQUAL_INT(2, m.selected());
    m.moveDown(3);
    TEST_ASSERT_EQUAL_INT(0, m.selected());
}

void test_up_from_first_wraps_to_last(void) {
    ModalPromptModel m;
    m.open(3, kRows, 0, 0);
    m.moveUp(1);
    TEST_ASSERT_EQUAL_INT(2, m.selected());
    m.moveUp(2);
    TEST_ASSERT_EQUAL_INT(1, m.selected());
}

void test_single_option_never_moves(void) {
    ModalPromptModel m;
    m.open(1, kRows, 0, 0);
    m.moveUp(1);
    TEST_ASSERT_EQUAL_INT(0, m.selected());
    m.moveDown(2);
    TEST_ASSERT_EQUAL_INT(0, m.selected());
    TEST_ASSERT_EQUAL_INT(1, m.windowCount());
    assertSelectionVisible(m);
}

// ------------------------------------------------------------------ window

static void checkWindowNeverScrolls(int count) {
    ModalPromptModel m;
    m.open(count, kRows, 0, 0);
    TEST_ASSERT_EQUAL_INT(count, m.windowCount());
    for (int i = 0; i < 2 * count + 1; i++) {
        m.moveDown(i);
        TEST_ASSERT_EQUAL_INT(0, m.windowStart());
        assertSelectionVisible(m);
    }
    for (int i = 0; i < 2 * count + 1; i++) {
        m.moveUp(i);
        TEST_ASSERT_EQUAL_INT(0, m.windowStart());
        assertSelectionVisible(m);
    }
}

void test_window_two_options_fits(void)   { checkWindowNeverScrolls(2); }
void test_window_three_options_fits(void) { checkWindowNeverScrolls(3); }

void test_window_eight_options_scrolls_minimally(void) {
    ModalPromptModel m;
    m.open(8, kRows, 0, 0);
    TEST_ASSERT_EQUAL_INT(kRows, m.windowCount());

    // Down within the first page: window stays put.
    for (int i = 1; i <= 3; i++) {
        m.moveDown(i);
        TEST_ASSERT_EQUAL_INT(i, m.selected());
        TEST_ASSERT_EQUAL_INT(0, m.windowStart());
    }
    // Each further Down scrolls by exactly one row.
    for (int i = 4; i <= 7; i++) {
        m.moveDown(i);
        TEST_ASSERT_EQUAL_INT(i, m.selected());
        TEST_ASSERT_EQUAL_INT(i - kRows + 1, m.windowStart());
    }
    // Down from the last wraps to the top page.
    m.moveDown(8);
    TEST_ASSERT_EQUAL_INT(0, m.selected());
    TEST_ASSERT_EQUAL_INT(0, m.windowStart());

    // Up from the first wraps to the bottom page.
    m.moveUp(9);
    TEST_ASSERT_EQUAL_INT(7, m.selected());
    TEST_ASSERT_EQUAL_INT(4, m.windowStart());

    // Up within the bottom page: window stays until the top row is passed.
    m.moveUp(10); m.moveUp(11); m.moveUp(12);
    TEST_ASSERT_EQUAL_INT(4, m.selected());
    TEST_ASSERT_EQUAL_INT(4, m.windowStart());
    m.moveUp(13);
    TEST_ASSERT_EQUAL_INT(3, m.selected());
    TEST_ASSERT_EQUAL_INT(3, m.windowStart());
}

void test_window_eight_options_selection_always_visible(void) {
    ModalPromptModel m;
    m.open(8, kRows, 0, 0);
    // Mixed walk crossing both wrap points several times.
    const char walk[] = "DDDDDDDDDDUUUUUUUUUUUUDDUDUUUDDDDDDDDDD";
    for (int i = 0; walk[i]; i++) {
        if (walk[i] == 'D') m.moveDown(i); else m.moveUp(i);
        assertSelectionVisible(m);
    }
}

// ------------------------------------------------------------------ select

void test_release_without_press_does_not_choose(void) {
    ModalPromptModel m;
    m.open(3, kRows, 0, 0);
    TEST_ASSERT_FALSE(m.selectReleased());
    TEST_ASSERT_TRUE(m.isOpen());
    TEST_ASSERT_EQUAL_INT(ModalPromptModel::kNoChoice, m.result());
}

void test_select_first_and_last(void) {
    ModalPromptModel m;
    m.open(8, kRows, 0, 0);
    m.selectPressed(0);
    TEST_ASSERT_TRUE(m.selectReleased());
    TEST_ASSERT_FALSE(m.isOpen());
    TEST_ASSERT_EQUAL_INT(0, m.result());

    m.open(8, kRows, 0, 0);
    m.moveUp(1);  // wrap to the last
    m.selectPressed(0);
    TEST_ASSERT_TRUE(m.selectReleased());
    TEST_ASSERT_EQUAL_INT(7, m.result());
}

void test_closed_prompt_ignores_input(void) {
    ModalPromptModel m;
    m.open(3, kRows, 0, 0);
    m.moveDown(1);
    m.selectPressed(0);
    m.selectReleased();
    TEST_ASSERT_EQUAL_INT(1, m.result());
    m.moveDown(2);
    m.selectPressed(0);
    TEST_ASSERT_FALSE(m.selectReleased());
    TEST_ASSERT_EQUAL_INT(1, m.result());
    TEST_ASSERT_EQUAL_INT(1, m.selected());
}

void test_reopen_clears_previous_result_and_arming(void) {
    ModalPromptModel m;
    m.open(3, kRows, 0, 0);
    m.selectPressed(0);      // armed, then the prompt is reopened
    m.open(3, kRows, 0, 0);
    TEST_ASSERT_FALSE(m.selectReleased());
    TEST_ASSERT_EQUAL_INT(ModalPromptModel::kNoChoice, m.result());
}

// ----------------------------------------------------------------- timeout

void test_no_timeout_by_default(void) {
    ModalPromptModel m;
    m.open(3, kRows, 0, 0);
    TEST_ASSERT_FALSE(m.tick(0xFFFFFFFFu));
    TEST_ASSERT_TRUE(m.isOpen());
}

void test_timeout_closes_with_no_choice(void) {
    ModalPromptModel m;
    m.open(3, kRows, 1000, 5000);
    m.moveDown(1000);            // an option is focused...
    TEST_ASSERT_FALSE(m.tick(5999));
    TEST_ASSERT_TRUE(m.isOpen());
    TEST_ASSERT_TRUE(m.tick(6000));
    TEST_ASSERT_FALSE(m.isOpen());
    TEST_ASSERT_EQUAL_INT(ModalPromptModel::kNoChoice, m.result());  // ...but not chosen
    TEST_ASSERT_FALSE(m.tick(7000));  // reports the timeout once
}

void test_timeout_counts_from_last_navigation(void) {
    ModalPromptModel m;
    m.open(3, kRows, 0, 1000);
    m.moveDown(900);
    TEST_ASSERT_FALSE(m.tick(1500));
    m.moveUp(1800);
    TEST_ASSERT_FALSE(m.tick(2799));
    TEST_ASSERT_TRUE(m.tick(2800));
    TEST_ASSERT_EQUAL_INT(ModalPromptModel::kNoChoice, m.result());
}

void test_timeout_never_fires_while_select_held(void) {
    ModalPromptModel m;
    m.open(3, kRows, 0, 1000);
    m.selectPressed(500);
    TEST_ASSERT_FALSE(m.tick(1500));
    TEST_ASSERT_FALSE(m.tick(60000));
    TEST_ASSERT_TRUE(m.isOpen());
    TEST_ASSERT_TRUE(m.selectReleased());   // the release still chooses
    TEST_ASSERT_EQUAL_INT(0, m.result());
}

void test_release_after_timeout_chooses_nothing(void) {
    ModalPromptModel m;
    m.open(3, kRows, 0, 1000);
    TEST_ASSERT_TRUE(m.tick(1000));
    m.selectPressed(1001);
    TEST_ASSERT_FALSE(m.selectReleased());
    TEST_ASSERT_EQUAL_INT(ModalPromptModel::kNoChoice, m.result());
}

// ------------------------------------------------------------ button guard

void test_guard_swallows_release_and_held_of_button_pressed_inside(void) {
    PromptButtonGuard g;
    g.beginOpen();
    g.noteWhileOpen(5, PromptButtonGuard::Press);   // Enter held at close
    g.noteWhileOpen(0, PromptButtonGuard::Press);   // Up pressed and released
    g.noteWhileOpen(0, PromptButtonGuard::Release);
    g.armOnClose(0);
    TEST_ASSERT_FALSE(g.consume(0, PromptButtonGuard::Release));
    TEST_ASSERT_TRUE(g.consume(5, PromptButtonGuard::Held));
    TEST_ASSERT_TRUE(g.consume(5, PromptButtonGuard::Held));
    TEST_ASSERT_TRUE(g.consume(5, PromptButtonGuard::Release));
    // Only that one release: the next press/release pair reaches the app.
    TEST_ASSERT_FALSE(g.consume(5, PromptButtonGuard::Press));
    TEST_ASSERT_FALSE(g.consume(5, PromptButtonGuard::Release));
}

void test_guard_swallows_buttons_still_down_at_close(void) {
    PromptButtonGuard g;
    g.beginOpen();
    g.armOnClose((1u << 1) | (1u << 4));   // held since before the prompt
    TEST_ASSERT_TRUE(g.consume(1, PromptButtonGuard::Release));
    TEST_ASSERT_TRUE(g.consume(4, PromptButtonGuard::Held));
    TEST_ASSERT_TRUE(g.consume(4, PromptButtonGuard::Release));
    TEST_ASSERT_EQUAL_UINT32(0, g.pendingMask());
}

void test_guard_fresh_press_clears_mark(void) {
    PromptButtonGuard g;
    g.beginOpen();
    g.noteWhileOpen(2, PromptButtonGuard::Press);
    g.armOnClose(0);
    // A missed release followed by a new press: deliver the press and stop
    // swallowing, so the new press's release is not lost.
    TEST_ASSERT_FALSE(g.consume(2, PromptButtonGuard::Press));
    TEST_ASSERT_FALSE(g.consume(2, PromptButtonGuard::Release));
}

void test_guard_ignores_out_of_range_buttons(void) {
    PromptButtonGuard g;
    g.beginOpen();
    g.noteWhileOpen(-1, PromptButtonGuard::Press);
    g.noteWhileOpen(99, PromptButtonGuard::Press);
    g.armOnClose(0);
    TEST_ASSERT_FALSE(g.consume(-1, PromptButtonGuard::Release));
    TEST_ASSERT_FALSE(g.consume(99, PromptButtonGuard::Release));
    TEST_ASSERT_EQUAL_UINT32(0, g.pendingMask());
}

void test_timeout_survives_clock_wrap(void) {
    ModalPromptModel m;
    m.open(3, kRows, 0xFFFFFF00u, 0x200);
    TEST_ASSERT_FALSE(m.tick(0x000000FFu));
    TEST_ASSERT_TRUE(m.tick(0x00000100u));
}

// ------------------------------------------------------------- ScrollLabel

void test_scroll_label_short_text_stays_still(void) {
    ScrollLabel s;
    for (uint32_t t = 0; t < 5000; t += 50) {
        s.tick(80, 120, t);
        TEST_ASSERT_EQUAL_INT(0, s.offset());
    }
}

void test_scroll_label_exact_width_stays_still(void) {
    TEST_ASSERT_FALSE(ScrollLabel::needsScroll(120, 120));
    TEST_ASSERT_TRUE(ScrollLabel::needsScroll(121, 120));
    ScrollLabel s;
    for (uint32_t t = 0; t < 5000; t += 50) {
        s.tick(120, 120, t);
        TEST_ASSERT_EQUAL_INT(0, s.offset());
    }
}

void test_scroll_label_long_text_steps_after_more_than_300ms(void) {
    ScrollLabel s;
    s.tick(150, 120, 300);   // exactly 300 ms since the (zero) clock: no step
    TEST_ASSERT_EQUAL_INT(0, s.offset());
    s.tick(150, 120, 301);
    TEST_ASSERT_EQUAL_INT(6, s.offset());
    s.tick(150, 120, 601);
    TEST_ASSERT_EQUAL_INT(6, s.offset());
    s.tick(150, 120, 602);
    TEST_ASSERT_EQUAL_INT(12, s.offset());
}

void test_scroll_label_long_text_restarts_past_the_end(void) {
    // Limit = 150 - 120 + 30 = 60: offsets climb 6..60, then jump to -20.
    ScrollLabel s;
    uint32_t t = 0;
    for (int expect = 6; expect <= 60; expect += 6) {
        t += 301;
        s.tick(150, 120, t);
        TEST_ASSERT_EQUAL_INT(expect, s.offset());
    }
    t += 301;
    s.tick(150, 120, t);
    TEST_ASSERT_EQUAL_INT(-20, s.offset());
    t += 301;
    s.tick(150, 120, t);
    TEST_ASSERT_EQUAL_INT(-14, s.offset());
}

void test_scroll_label_restart_holds_one_full_step(void) {
    ScrollLabel s;
    s.tick(200, 120, 301);
    s.tick(200, 120, 602);
    TEST_ASSERT_EQUAL_INT(12, s.offset());
    s.restart(5000);          // e.g. a newly focused prompt row
    TEST_ASSERT_EQUAL_INT(0, s.offset());
    s.tick(200, 120, 5300);   // exactly one step period: still holding
    TEST_ASSERT_EQUAL_INT(0, s.offset());
    s.tick(200, 120, 5301);
    TEST_ASSERT_EQUAL_INT(6, s.offset());
}

void test_scroll_label_reset_returns_to_start_keeping_step_clock(void) {
    ScrollLabel s;
    s.tick(200, 120, 301);
    s.tick(200, 120, 602);
    TEST_ASSERT_EQUAL_INT(12, s.offset());
    s.reset();
    TEST_ASSERT_EQUAL_INT(0, s.offset());
    s.tick(200, 120, 700);   // clock untouched by reset: 98 ms since last step
    TEST_ASSERT_EQUAL_INT(0, s.offset());
    s.tick(200, 120, 903);
    TEST_ASSERT_EQUAL_INT(6, s.offset());
}

int main(int /*argc*/, char** /*argv*/) {
    UNITY_BEGIN();
    RUN_TEST(test_open_starts_zero_based_with_no_result);
    RUN_TEST(test_open_refuses_empty_option_list);
    RUN_TEST(test_down_walks_to_last_then_wraps_to_first);
    RUN_TEST(test_up_from_first_wraps_to_last);
    RUN_TEST(test_single_option_never_moves);
    RUN_TEST(test_window_two_options_fits);
    RUN_TEST(test_window_three_options_fits);
    RUN_TEST(test_window_eight_options_scrolls_minimally);
    RUN_TEST(test_window_eight_options_selection_always_visible);
    RUN_TEST(test_release_without_press_does_not_choose);
    RUN_TEST(test_select_first_and_last);
    RUN_TEST(test_closed_prompt_ignores_input);
    RUN_TEST(test_reopen_clears_previous_result_and_arming);
    RUN_TEST(test_no_timeout_by_default);
    RUN_TEST(test_timeout_closes_with_no_choice);
    RUN_TEST(test_timeout_counts_from_last_navigation);
    RUN_TEST(test_timeout_never_fires_while_select_held);
    RUN_TEST(test_release_after_timeout_chooses_nothing);
    RUN_TEST(test_guard_swallows_release_and_held_of_button_pressed_inside);
    RUN_TEST(test_guard_swallows_buttons_still_down_at_close);
    RUN_TEST(test_guard_fresh_press_clears_mark);
    RUN_TEST(test_guard_ignores_out_of_range_buttons);
    RUN_TEST(test_timeout_survives_clock_wrap);
    RUN_TEST(test_scroll_label_short_text_stays_still);
    RUN_TEST(test_scroll_label_exact_width_stays_still);
    RUN_TEST(test_scroll_label_long_text_steps_after_more_than_300ms);
    RUN_TEST(test_scroll_label_long_text_restarts_past_the_end);
    RUN_TEST(test_scroll_label_restart_holds_one_full_step);
    RUN_TEST(test_scroll_label_reset_returns_to_start_keeping_step_clock);
    return UNITY_END();
}
