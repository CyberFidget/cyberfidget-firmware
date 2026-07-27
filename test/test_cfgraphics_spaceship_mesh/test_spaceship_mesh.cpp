// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#include <math.h>
#include <stdio.h>
#include <unity.h>

#include "cf_gfx.h"
#include "../../lib/Spaceship/generated/ship.h"

using namespace cf::gfx;

namespace {

// Mirrors SHIP_MODEL_SCALE_Q8 in Spaceship.cpp, which is file-static and so
// cannot be imported. Change both together or these tests silently pin a
// value the app no longer uses.
const uint16_t kShipScaleQ8 = 112;
const int16_t kAnchorX = 64;
const int16_t kAnchorY = 32;
const float kPi = 3.14159265358979323846f;

uint8_t bankToYaw(float bank) {
    if (bank < -0.6f) bank = -0.6f;
    if (bank > 0.6f) bank = 0.6f;
    return (uint8_t)lroundf(bank * 256.0f / (2.0f * kPi));
}

MeshActor shipActor(float bank) {
    MeshActor actor;
    actor.setMesh(&ship::ship);
    actor.setPos(kAnchorX, kAnchorY);
    actor.setScaleQ8(kShipScaleQ8);
    actor.setRotation(bankToYaw(bank), 0, 0);
    return actor;
}

void projectVertex(const MeshActor& actor, uint8_t index,
                   int16_t& x, int16_t& y) {
    const MeshVertex& vertex = ship::ship.verts[index];
    TEST_ASSERT_TRUE(actor.projectPoint(vertex.x, vertex.y, vertex.z, x, y));
}

bool framebufferPixel(const DisplayProxy& display, int x, int y) {
    return (display.fb[x + (y >> 3) * 128] & (1U << (y & 7))) != 0;
}

void printFrame(float bank) {
    DisplayProxy display;
    MeshActor actor = shipActor(bank);
    actor.draw(display);

    printf("\nship bank %+.1f\n", (double)bank);
    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 128; ++x) {
            putchar(framebufferPixel(display, x, y) ? '#' : '.');
        }
        putchar('\n');
    }
    fflush(stdout);
}

}  // namespace

void test_ship_nose_screen_x_stays_anchored_across_full_bank_range(void) {
    for (int step = -6; step <= 6; ++step) {
        const MeshActor actor = shipActor((float)step / 10.0f);
        int16_t x = 0;
        int16_t y = 0;
        projectVertex(actor, 0, x, y);
        TEST_ASSERT_EQUAL_INT16(kAnchorX, x);
    }
}

void test_ship_wingtip_tilt_flips_with_bank_direction(void) {
    int16_t positiveLeftX = 0;
    int16_t positiveLeftY = 0;
    int16_t positiveRightX = 0;
    int16_t positiveRightY = 0;
    const MeshActor positive = shipActor(0.3f);
    projectVertex(positive, 6, positiveLeftX, positiveLeftY);
    projectVertex(positive, 7, positiveRightX, positiveRightY);

    // Compare each tip with their midpoint without rounding a half pixel.
    const int positiveCentreTwice = positiveLeftY + positiveRightY;
    TEST_ASSERT_GREATER_THAN_INT(positiveCentreTwice,
                                 2 * positiveLeftY);
    TEST_ASSERT_LESS_THAN_INT(positiveCentreTwice,
                              2 * positiveRightY);

    int16_t negativeLeftX = 0;
    int16_t negativeLeftY = 0;
    int16_t negativeRightX = 0;
    int16_t negativeRightY = 0;
    const MeshActor negative = shipActor(-0.3f);
    projectVertex(negative, 6, negativeLeftX, negativeLeftY);
    projectVertex(negative, 7, negativeRightX, negativeRightY);

    const int negativeCentreTwice = negativeLeftY + negativeRightY;
    TEST_ASSERT_LESS_THAN_INT(negativeCentreTwice,
                              2 * negativeLeftY);
    TEST_ASSERT_GREATER_THAN_INT(negativeCentreTwice,
                                 2 * negativeRightY);
}

void test_ship_ascii_bank_frames_are_printed(void) {
    static const float banks[] = {-0.6f, -0.3f, 0.0f, 0.3f, 0.6f};
    for (float bank : banks) printFrame(bank);
}

void setUp(void) {}
void tearDown(void) {}

int main(int /*argc*/, char** /*argv*/) {
    UNITY_BEGIN();
    RUN_TEST(test_ship_nose_screen_x_stays_anchored_across_full_bank_range);
    RUN_TEST(test_ship_wingtip_tilt_flips_with_bank_direction);
    RUN_TEST(test_ship_ascii_bank_frames_are_printed);
    return UNITY_END();
}
