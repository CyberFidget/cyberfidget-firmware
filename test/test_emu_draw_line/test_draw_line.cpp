// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2023-2026 Dismo Industries LLC

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <unity.h>

class String {
public:
    const char* c_str() const { return ""; }
    size_t length() const { return 0; }
};

#define pgm_read_byte(address) (*(address))
#include "SSD1306Wire.h"

namespace {

constexpr int kWidth = 128;
constexpr int kHeight = 64;
constexpr int kDeviceBufferSize = kWidth * kHeight / 8;

struct Line {
    int16_t x0;
    int16_t y0;
    int16_t x1;
    int16_t y1;
};

void referenceSetPixel(uint8_t* buffer, int16_t x, int16_t y) {
    if (x >= 0 && x < kWidth && y >= 0 && y < kHeight) {
        buffer[x + (y / 8) * kWidth] |= static_cast<uint8_t>(1U << (y & 7));
    }
}

void swapInt16(int16_t& a, int16_t& b) {
    const int16_t t = a;
    a = b;
    b = t;
}

// Direct transcription of ThingPulse OLEDDisplay::drawLine from
// .pio/libdeps/local/ESP8266 and ESP32 OLED driver for SSD1306 displays/
// src/OLEDDisplay.cpp:169-206.
void referenceDrawLine(
        uint8_t* buffer, int16_t x0, int16_t y0, int16_t x1, int16_t y1) {
    int16_t steep = abs(y1 - y0) > abs(x1 - x0);
    if (steep) {
        swapInt16(x0, y0);
        swapInt16(x1, y1);
    }

    if (x0 > x1) {
        swapInt16(x0, x1);
        swapInt16(y0, y1);
    }

    int16_t dx, dy;
    dx = x1 - x0;
    dy = abs(y1 - y0);

    int16_t err = dx / 2;
    int16_t ystep;

    if (y0 < y1) {
        ystep = 1;
    } else {
        ystep = -1;
    }

    for (; x0 <= x1; x0++) {
        if (steep) {
            referenceSetPixel(buffer, y0, x0);
        } else {
            referenceSetPixel(buffer, x0, y0);
        }
        err -= dy;
        if (err < 0) {
            y0 += ystep;
            err += dx;
        }
    }
}

void packShimBuffer(const SSD1306Wire& display, uint8_t* packed) {
    const uint8_t* pixels = display.getBuffer();
    memset(packed, 0, kDeviceBufferSize);
    for (int16_t y = 0; y < kHeight; ++y) {
        for (int16_t x = 0; x < kWidth; ++x) {
            if (pixels[y * kWidth + x] != 0) {
                packed[x + (y / 8) * kWidth] |=
                    static_cast<uint8_t>(1U << (y & 7));
            }
        }
    }
}

void assertDirectionMatches(const Line& line, const char* message) {
    uint8_t expected[kDeviceBufferSize] = {};
    uint8_t actual[kDeviceBufferSize] = {};
    SSD1306Wire display;

    referenceDrawLine(expected, line.x0, line.y0, line.x1, line.y1);
    display.drawLine(line.x0, line.y0, line.x1, line.y1);
    packShimBuffer(display, actual);

    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(
        expected, actual, kDeviceBufferSize, message);
}

void assertBothDirectionsMatch(const Line& line, const char* message) {
    assertDirectionMatches(line, message);
    assertDirectionMatches(
        Line{line.x1, line.y1, line.x0, line.y0}, message);
}

#define DEFINE_LINE_TEST(name, x0, y0, x1, y1) \
    void test_##name() { \
        assertBothDirectionsMatch(Line{x0, y0, x1, y1}, #name); \
    }

DEFINE_LINE_TEST(horizontal, 9, 17, 103, 17)
DEFINE_LINE_TEST(vertical, 71, 5, 71, 58)
DEFINE_LINE_TEST(shallow_positive_slope, 7, 8, 94, 39)
DEFINE_LINE_TEST(shallow_negative_slope, 7, 51, 94, 20)
DEFINE_LINE_TEST(steep_positive_slope, 18, 3, 43, 60)
DEFINE_LINE_TEST(steep_negative_slope, 18, 60, 43, 3)
DEFINE_LINE_TEST(diagonal_positive_45_degrees, 11, 7, 57, 53)
DEFINE_LINE_TEST(diagonal_negative_45_degrees, 11, 53, 57, 7)
DEFINE_LINE_TEST(zero_length, 63, 31, 63, 31)
DEFINE_LINE_TEST(single_pixel_at_corner, 127, 63, 127, 63)
DEFINE_LINE_TEST(fully_off_screen_left, -20, 4, -2, 55)
DEFINE_LINE_TEST(fully_off_screen_right, 130, 4, 150, 55)
DEFINE_LINE_TEST(fully_off_screen_top, 4, -20, 120, -2)
DEFINE_LINE_TEST(fully_off_screen_bottom, 4, 66, 120, 82)
DEFINE_LINE_TEST(crosses_left_edge, -24, 7, 40, 51)
DEFINE_LINE_TEST(crosses_right_edge, 90, 11, 151, 52)
DEFINE_LINE_TEST(crosses_top_edge, 17, -23, 88, 31)
DEFINE_LINE_TEST(crosses_bottom_edge, 19, 42, 101, 85)

#undef DEFINE_LINE_TEST

}  // namespace

void setUp() {}
void tearDown() {}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_horizontal);
    RUN_TEST(test_vertical);
    RUN_TEST(test_shallow_positive_slope);
    RUN_TEST(test_shallow_negative_slope);
    RUN_TEST(test_steep_positive_slope);
    RUN_TEST(test_steep_negative_slope);
    RUN_TEST(test_diagonal_positive_45_degrees);
    RUN_TEST(test_diagonal_negative_45_degrees);
    RUN_TEST(test_zero_length);
    RUN_TEST(test_single_pixel_at_corner);
    RUN_TEST(test_fully_off_screen_left);
    RUN_TEST(test_fully_off_screen_right);
    RUN_TEST(test_fully_off_screen_top);
    RUN_TEST(test_fully_off_screen_bottom);
    RUN_TEST(test_crosses_left_edge);
    RUN_TEST(test_crosses_right_edge);
    RUN_TEST(test_crosses_top_edge);
    RUN_TEST(test_crosses_bottom_edge);
    return UNITY_END();
}
