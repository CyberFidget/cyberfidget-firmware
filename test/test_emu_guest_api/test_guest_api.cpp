// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2026 Dismo Industries LLC

#include <unity.h>
#include "../../wasm/device_module/shims/HAL.h"

float sliderPosition_Percentage_Filtered = 0;
float sliderPosition_Percentage_Inverted_Filtered = 0;
int sliderPosition_8Bits_Filtered = 0;
int sliderPosition_8Bits_Inverted_Filtered = 0;
float sliderPosition_12Bits_Filtered = 0;
float sliderPosition_12Bits_Inverted_Filtered = 0;
extern "C" uint32_t cf_millis(void) { return 0; }

static int ledArgs[5];
static int ledCalls = 0;
void HAL::setRgbLed(int index, uint8_t r, uint8_t g, uint8_t b, uint8_t w) {
    ++ledCalls;
    ledArgs[0] = index;
    ledArgs[1] = r;
    ledArgs[2] = g;
    ledArgs[3] = b;
    ledArgs[4] = w;
}

void setUp() { ledCalls = 0; }
void tearDown() {}

static void checkSlider(float pct, int bits8, float bits12, int expectedPct,
                        int expected8, int expected12) {
    sliderPosition_Percentage_Inverted_Filtered = pct;
    sliderPosition_8Bits_Inverted_Filtered = bits8;
    sliderPosition_12Bits_Inverted_Filtered = bits12;
    // The non-inverted readings must not affect these helpers.
    sliderPosition_Percentage_Filtered = 100.0f - pct;
    sliderPosition_8Bits_Filtered = 255 - bits8;
    sliderPosition_12Bits_Filtered = 4095.0f - bits12;
    TEST_ASSERT_EQUAL_INT(expectedPct, sliderPercent());
    TEST_ASSERT_EQUAL_INT(expected8, slider255());
    TEST_ASSERT_EQUAL_INT(expected12, slider4095());
}

static void test_slider_endpoints() {
    checkSlider(0, 0, 0, 0, 0, 0);
    checkSlider(100, 255, 4095, 100, 255, 4095);
}

static void test_slider_rounding() {
    checkSlider(49.49f, 127, 2047.49f, 49, 127, 2047);
    checkSlider(49.5f, 128, 2047.5f, 50, 128, 2048);
    checkSlider(50.5f, 129, 2048.5f, 51, 129, 2049);
}

static void test_slider_clamping() {
    checkSlider(-0.5f, -1, -0.5f, 0, 0, 0);
    checkSlider(100.5f, 256, 4095.5f, 100, 255, 4095);
    checkSlider(-1.0e20f, -1000, -1.0e20f, 0, 0, 0);
    checkSlider(1.0e20f, 1000, 1.0e20f, 100, 255, 4095);
}

static void test_set_led_forwards_every_argument() {
    setLed(-2, 0, 17, 128, 255);
    const int expected[] = {-2, 0, 17, 128, 255};
    TEST_ASSERT_EQUAL_INT(1, ledCalls);
    TEST_ASSERT_EQUAL_INT_ARRAY(expected, ledArgs, 5);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_slider_endpoints);
    RUN_TEST(test_slider_rounding);
    RUN_TEST(test_slider_clamping);
    RUN_TEST(test_set_led_forwards_every_argument);
    return UNITY_END();
}
