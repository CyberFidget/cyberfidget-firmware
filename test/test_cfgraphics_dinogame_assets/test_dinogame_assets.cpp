// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#include <unity.h>

#include "cactus.h"
#include "cf_gfx.h"
#include "dino.h"
#include "ground.h"
#include "pterodactyl.h"

using namespace cf::gfx;

namespace {

const uint8_t kDinoStand[] = {
    0x0F,0x00, 0x1F,0x00, 0x19,0x80, 0x19,0x80,
    0x1F,0x80, 0x19,0x80, 0x19,0x80, 0x1F,0x00,
    0x18,0x00, 0x38,0x00, 0x38,0x00, 0x38,0x00,
    0x70,0x00, 0x30,0x00, 0x00,0x00, 0x00,0x00,
};
const uint8_t kDinoDuck[] = {
    0x00,0x00, 0x1F,0xC0, 0x3F,0xE0, 0x33,0x30,
    0x33,0x30, 0x3F,0xE0, 0x1F,0xC0, 0x00,0x00,
};
const uint8_t kCactus[] = {
    0x18, 0x18, 0x18, 0x7E, 0x7E, 0x18, 0x18, 0x18,
    0x18, 0x18, 0x18, 0x3C, 0x3C, 0x3C, 0x00, 0x00,
};
const uint8_t kPterodactyl[] = {
    0x00,0x00, 0x08,0x00, 0x18,0x00, 0x3C,0x00,
    0x7E,0x00, 0xFF,0x00, 0x3C,0xC0, 0x18,0x60,
    0x00,0x30, 0x00,0x18, 0x00,0x18, 0x00,0x30,
    0x00,0x60, 0x00,0xC0, 0x00,0x00, 0x00,0x00,
};
const uint8_t kGroundA[] = {
    0xC0,0x03, 0x80,0x01, 0x80,0x01, 0xC0,0x03,
    0xE0,0x07, 0xF0,0x0F, 0xF8,0x1F, 0xFC,0x3F,
};
const uint8_t kGroundB[] = {
    0xE0,0x07, 0xC0,0x03, 0xC0,0x03, 0xE0,0x07,
    0xF0,0x0F, 0xF0,0x0F, 0xF8,0x1F, 0xFC,0x3F,
};
const uint8_t kGroundC[] = {
    0xF0,0x0F, 0xE0,0x07, 0xC0,0x03, 0xC0,0x03,
    0xE0,0x07, 0xF0,0x0F, 0xF8,0x1F, 0xFC,0x3F,
};
const uint8_t kGroundD[] = {
    0xC0,0x03, 0xC0,0x03, 0xC0,0x03, 0xE0,0x07,
    0xE0,0x07, 0xF0,0x0F, 0xF8,0x1F, 0xFC,0x3F,
};

bool legacyPixel(const uint8_t* data, int width, int x, int y) {
    const int stride = (width + 7) / 8;
    return ((data[y * stride + x / 8] >> (7 - (x % 8))) & 1) != 0;
}

bool legacyCollides(const uint8_t* a, int aw, int ah, int ax, int ay,
                    const uint8_t* b, int bw, int bh, int bx, int by) {
    const int left = ax > bx ? ax : bx;
    const int right = (ax + aw) < (bx + bw) ? (ax + aw) : (bx + bw);
    const int top = ay > by ? ay : by;
    const int bottom = (ay + ah) < (by + bh) ? (ay + ah) : (by + bh);
    for (int y = top; y < bottom; ++y) {
        for (int x = left; x < right; ++x) {
            if (legacyPixel(a, aw, x - ax, y - ay)
                    && legacyPixel(b, bw, x - bx, y - by)) {
                return true;
            }
        }
    }
    return false;
}

void assertCroppedLegacy(const uint8_t* legacy, int width, int height,
                         const Sprite& generated) {
    const int stride = (width + 7) / 8;
    int minX = width;
    int minY = height;
    int maxX = -1;
    int maxY = -1;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            if ((legacy[y * stride + x / 8] >> (x % 8)) & 1) {
                if (x < minX) minX = x;
                if (x > maxX) maxX = x;
                if (y < minY) minY = y;
                if (y > maxY) maxY = y;
            }
        }
    }

    TEST_ASSERT_TRUE(maxX >= minX);
    TEST_ASSERT_EQUAL_UINT16(maxX - minX + 1, generated.w);
    TEST_ASSERT_EQUAL_UINT16(maxY - minY + 1, generated.h);
    TEST_ASSERT_EQUAL_INT8(width / 2 - minX, generated.pivotX);
    TEST_ASSERT_EQUAL_INT8(height - minY, generated.pivotY);
    TEST_ASSERT_EQUAL(BO_LSB_FIRST, generated.bitOrder);

    const int generatedStride = (generated.w + 7) / 8;
    for (int y = 0; y < generated.h; ++y) {
        for (int byteColumn = 0; byteColumn < generatedStride; ++byteColumn) {
            uint8_t expected = 0;
            for (int bit = 0; bit < 8; ++bit) {
                const int x = byteColumn * 8 + bit;
                if (x >= generated.w) continue;
                const int sourceX = minX + x;
                const int sourceY = minY + y;
                if ((legacy[sourceY * stride + sourceX / 8] >> (sourceX % 8)) & 1) {
                    expected |= static_cast<uint8_t>(1U << bit);
                }
            }
            TEST_ASSERT_EQUAL_HEX8(
                expected,
                pgm_read_byte(&generated.data[y * generatedStride + byteColumn]));
        }
    }
}

}  // namespace

void test_generated_cels_are_cropped_legacy_xbm_byte_for_byte(void) {
    assertCroppedLegacy(kDinoStand, 16, 16, dino::cel_0);
    assertCroppedLegacy(kDinoDuck, 16, 8, dino::cel_1);
    assertCroppedLegacy(kCactus, 8, 16, cactus::cel_0);
    assertCroppedLegacy(kPterodactyl, 16, 16, pterodactyl::cel_0);
    assertCroppedLegacy(kGroundA, 16, 8, ground::cel_0);
    assertCroppedLegacy(kGroundB, 16, 8, ground::cel_1);
    assertCroppedLegacy(kGroundC, 16, 8, ground::cel_2);
    assertCroppedLegacy(kGroundD, 16, 8, ground::cel_3);

    TEST_ASSERT_EQUAL_PTR(&dino::cel_0, dino::anim_run->frames[0]);
    TEST_ASSERT_EQUAL_PTR(&dino::cel_1, dino::anim_duck->frames[0]);
    TEST_ASSERT_EQUAL_PTR(&dino::cel_0, dino::anim_jump->frames[0]);
    TEST_ASSERT_EQUAL_UINT8(4, ground::sheet.animationCount);
}

void test_collision_uses_the_pixels_drawXbm_renders_not_legacy_mirrors(void) {
    // A renders at world x=0; B's x=7 pixel also lands at world x=0.
    // The legacy MSB-first sampler mirrors A to x=7 and B to world x=-7.
    static const uint8_t aBits[] = {0x01};
    static const uint8_t bBits[] = {0x80};
    static const Sprite a = {aBits, 8, 1, 0, 0, BO_LSB_FIRST};
    static const Sprite b = {bBits, 8, 1, 0, 0, BO_LSB_FIRST};

    DisplayProxy display;
    drawSprite(display, a, 0, 0);
    drawSprite(display, b, -7, 0);
    TEST_ASSERT_EQUAL(2, display.callCount);
    TEST_ASSERT_TRUE(sampleSprite(a, 0, 0));
    TEST_ASSERT_TRUE(sampleSprite(b, 7, 0));
    TEST_ASSERT_TRUE(pixelCollides(a, 0, 0, b, -7, 0));
    TEST_ASSERT_FALSE(legacyCollides(aBits, 8, 1, 0, 0, bBits, 8, 1, -7, 0));
}

void test_dino_actor_play_transitions_run_duck_jump_run(void) {
    Actor actor;
    actor.setSheet(&dino::sheet);

    TEST_ASSERT_TRUE(actor.play("run", 0));
    TEST_ASSERT_EQUAL_PTR(dino::anim_run, actor.animation());
    TEST_ASSERT_EQUAL_PTR(&dino::cel_0, actor.sprite());

    TEST_ASSERT_TRUE(actor.play("duck", 10));
    TEST_ASSERT_EQUAL_PTR(dino::anim_duck, actor.animation());
    TEST_ASSERT_EQUAL_PTR(&dino::cel_1, actor.sprite());
    TEST_ASSERT_TRUE(actor.play("duck", 20));
    TEST_ASSERT_EQUAL_PTR(dino::anim_duck, actor.animation());

    TEST_ASSERT_TRUE(actor.play("jump", 30));
    TEST_ASSERT_EQUAL_PTR(dino::anim_jump, actor.animation());
    TEST_ASSERT_EQUAL_PTR(&dino::cel_0, actor.sprite());

    TEST_ASSERT_TRUE(actor.play("run", 40));
    TEST_ASSERT_EQUAL_PTR(dino::anim_run, actor.animation());
}

void setUp(void) {}
void tearDown(void) {}

int main(int /*argc*/, char** /*argv*/) {
    UNITY_BEGIN();
    RUN_TEST(test_generated_cels_are_cropped_legacy_xbm_byte_for_byte);
    RUN_TEST(test_collision_uses_the_pixels_drawXbm_renders_not_legacy_mirrors);
    RUN_TEST(test_dino_actor_play_transitions_run_duck_jump_run);
    return UNITY_END();
}
