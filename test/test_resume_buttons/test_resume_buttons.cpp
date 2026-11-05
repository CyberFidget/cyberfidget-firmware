// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#include <unity.h>
#include "../../lib/ButtonManager/ButtonManager.cpp"

unsigned long millis_NOW = 0;
unsigned long millis_APP_LASTINTERACTION = 0;
int buttonCounter[numButtons] = {};
int buttonCounterSaved[numButtons] = {};
int appActive = 0;
int appActiveSaved = 0;
uint8_t wasm_button_states[8] = {};
extern const int wasm_pin_map[8] = {36, 37, 38, 39, 34, 15, -1, -1};
uint32_t millis() { return (uint32_t)millis_NOW; }
const int pins[] = {36, 37, 38, 39, 34, 15};
const bool pullups[] = {false, false, false, false, false, true};

void setUp() {
    millis_NOW = 0;
    for (int i = 0; i < 8; ++i) wasm_button_states[i] = 0;
}
void tearDown() {}

static void scan(ButtonManager& buttons, unsigned long at) {
    millis_NOW = at;
    buttons.update();
}
static void noGesture(ButtonManager& buttons) {
    ButtonEvent event;
    TEST_ASSERT_FALSE(buttons.getNextEvent(event));
    for (int i = 0; i < numButtons; ++i) {
        TEST_ASSERT_FALSE(buttons.isPressed(i));
        TEST_ASSERT_FALSE(buttons.wasPressed(i));
        TEST_ASSERT_FALSE(buttons.wasReleased(i));
    }
}
static void delivered(ButtonManager& buttons, int index, ButtonEventType type) {
    ButtonEvent event;
    TEST_ASSERT_TRUE(buttons.getNextEvent(event));
    TEST_ASSERT_EQUAL(index, event.buttonIndex);
    TEST_ASSERT_EQUAL((int)type, (int)event.eventType);
    buttons.latchEdge(event);
    TEST_ASSERT_FALSE(buttons.getNextEvent(event));
}

void test_every_initially_held_button_has_no_events_or_polling_edges() {
    for (int i = 0; i < numButtons; ++i) wasm_button_states[i] = 1;
    ButtonManager buttons(pins, pullups, numButtons);
    buttons.init();
    noGesture(buttons);
    for (unsigned long at : {30ul, 1500ul, 6000ul}) {
        scan(buttons, at);
        noGesture(buttons);
    }
    for (int i = 0; i < numButtons; ++i) wasm_button_states[i] = 0;
    scan(buttons, 6010); noGesture(buttons);
    wasm_button_states[5] = 1; // contact bounce restarts the release wait
    scan(buttons, 6020); noGesture(buttons);
    wasm_button_states[5] = 0;
    scan(buttons, 6030); noGesture(buttons);
    scan(buttons, 6051); noGesture(buttons);
    scan(buttons, 6052); noGesture(buttons);
    for (int i = 0; i < numButtons; ++i) {
        const unsigned long at = 6100 + i * 2000;
        wasm_button_states[i] = 1;
        scan(buttons, at);
        delivered(buttons, i, ButtonEvent_Pressed);
        TEST_ASSERT_TRUE(buttons.isPressed(i));
        TEST_ASSERT_TRUE(buttons.wasPressed(i));
        buttons.clearEdges();
        scan(buttons, at + 1600);
        delivered(buttons, i, ButtonEvent_Held);
        TEST_ASSERT_FALSE(buttons.wasPressed(i));
        TEST_ASSERT_FALSE(buttons.wasReleased(i));
        wasm_button_states[i] = 0;
        scan(buttons, at + 1700);
        delivered(buttons, i, ButtonEvent_Released);
        TEST_ASSERT_TRUE(buttons.wasReleased(i));
        buttons.clearEdges();
    }
}

void test_a_released_boot_button_delivers_the_first_tap() {
    ButtonManager buttons(pins, pullups, numButtons);
    buttons.init();
    wasm_button_states[5] = 1;
    scan(buttons, 50);
    delivered(buttons, 5, ButtonEvent_Pressed);
    wasm_button_states[5] = 0;
    scan(buttons, 100);
    delivered(buttons, 5, ButtonEvent_Released);
    TEST_ASSERT_TRUE(buttons.wasPressed(5));
    TEST_ASSERT_TRUE(buttons.wasReleased(5));
}

void test_cli_injected_hold_bypasses_the_physical_wake_gate() {
    wasm_button_states[5] = 1;
    ButtonManager buttons(pins, pullups, numButtons);
    buttons.init();
    scan(buttons, 2000); noGesture(buttons);
    buttons.injectEvent(5, ButtonEvent_Pressed);
    delivered(buttons, 5, ButtonEvent_Pressed);
    buttons.clearEdges();
    scan(buttons, 2010);
    TEST_ASSERT_TRUE(buttons.isPressed(5));
    ButtonEvent event;
    TEST_ASSERT_FALSE(buttons.getNextEvent(event));
    buttons.injectEvent(5, ButtonEvent_Held);
    delivered(buttons, 5, ButtonEvent_Held);
    buttons.injectEvent(5, ButtonEvent_Released);
    delivered(buttons, 5, ButtonEvent_Released);
    buttons.clearEdges();
    scan(buttons, 2100); noGesture(buttons);
    wasm_button_states[5] = 0;
    scan(buttons, 2200); noGesture(buttons);
    scan(buttons, 2221); noGesture(buttons);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_every_initially_held_button_has_no_events_or_polling_edges);
    RUN_TEST(test_a_released_boot_button_delivers_the_first_tap);
    RUN_TEST(test_cli_injected_hold_bypasses_the_physical_wake_gate);
    return UNITY_END();
}
