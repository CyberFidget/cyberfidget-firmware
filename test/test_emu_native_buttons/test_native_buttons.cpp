// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2026 Dismo Industries LLC

#include <unity.h>
#include "../../lib/ButtonManager/ButtonManager.cpp"

unsigned long millis_NOW = 0;
unsigned long millis_APP_LASTINTERACTION = 0;
int buttonCounter[numButtons] = {};
int buttonCounterSaved[numButtons] = {};
int appActive = 0;
int appActiveSaved = 0;
uint8_t wasm_button_states[8] = {};
extern const int wasm_pin_map[8] = {};
uint32_t millis() { return 0; }

static const int pins[] = {36, 37, 38, 39, 34, 15};
static const bool pullups[] = {true, true, true, true, true, true};
static ButtonManager bm(pins, pullups, numButtons);
static int calls = 0;
static void record(const ButtonEvent&) { ++calls; }

void setUp() { bm.clearEdges(); bm.unregisterCallback(1); calls = 0; }
void tearDown() {}

static void test_native_edges_survive_until_app_update() {
    bm.registerCallback(1, record);
    for (int b = 0; b < 2; ++b) {
        const ButtonEvent event = {b, ButtonEvent_Pressed, 0};
        bm.latchEdge(event);
        TEST_ASSERT_TRUE(bm.wasPressed(b));
        TEST_ASSERT_FALSE(bm.wasReleased(b));
        TEST_ASSERT_FALSE(bm.isPressed(b)); // latching does not change physical state
    }
    TEST_ASSERT_EQUAL_INT(0, calls); // callbacks remain the runner's responsibility
    TEST_ASSERT_TRUE(bm.hasCallback(1));
    TEST_ASSERT_TRUE(bm.getCallback(1) == record);
    // Another event-loop pass before the app update must preserve the press.
    bm.latchEdge({0, ButtonEvent_Held, 1500});
    TEST_ASSERT_TRUE(bm.wasPressed(0));
    bm.clearEdges();
    for (int b = 0; b < 2; ++b) {
        TEST_ASSERT_FALSE(bm.wasPressed(b));
        bm.latchEdge({b, ButtonEvent_Released, 50});
        TEST_ASSERT_TRUE(bm.wasReleased(b));
        TEST_ASSERT_FALSE(bm.wasPressed(b));
    }
    bm.clearEdges();
    for (int b = 0; b < 2; ++b) TEST_ASSERT_FALSE(bm.wasReleased(b));
}

static void test_native_tap_latches_both_edges() {
    bm.latchEdge({5, ButtonEvent_Pressed, 0});
    bm.latchEdge({5, ButtonEvent_Released, 20});
    TEST_ASSERT_TRUE(bm.wasPressed(5));
    TEST_ASSERT_TRUE(bm.wasReleased(5));
    bm.clearEdges();
    TEST_ASSERT_FALSE(bm.wasPressed(5));
    TEST_ASSERT_FALSE(bm.wasReleased(5));
}

static void test_native_held_and_none_set_neither_edge() {
    bm.latchEdge({2, ButtonEvent_Held, 1500});
    bm.latchEdge({2, ButtonEvent_None, 0});
    TEST_ASSERT_FALSE(bm.wasPressed(2));
    TEST_ASSERT_FALSE(bm.wasReleased(2));
}

static void test_native_invalid_indices_are_ignored() {
    bm.latchEdge({-1, ButtonEvent_Pressed, 0});
    bm.latchEdge({numButtons, ButtonEvent_Released, 0});
    TEST_ASSERT_FALSE(bm.wasPressed(-1));
    TEST_ASSERT_FALSE(bm.wasPressed(numButtons));
    TEST_ASSERT_FALSE(bm.wasReleased(-1));
    TEST_ASSERT_FALSE(bm.wasReleased(numButtons));
    for (int b = 0; b < numButtons; ++b) {
        TEST_ASSERT_FALSE(bm.wasPressed(b));
        TEST_ASSERT_FALSE(bm.wasReleased(b));
    }
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_native_edges_survive_until_app_update);
    RUN_TEST(test_native_tap_latches_both_edges);
    RUN_TEST(test_native_held_and_none_set_neither_edge);
    RUN_TEST(test_native_invalid_indices_are_ignored);
    return UNITY_END();
}
