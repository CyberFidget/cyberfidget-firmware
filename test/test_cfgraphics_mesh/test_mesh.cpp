// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

// test/test_cfgraphics_mesh/test_mesh.cpp
//
// Hand-derived proofs for the integer mesh transform, projection, visibility,
// bounded-work rules, and the host's canonical line rasterizer.

#include <unity.h>

#include "cf_gfx.h"
#include "cf_gfx_mesh_cube_fixture.h"

using namespace cf::gfx;
using namespace cf::gfx::meshfixtures;

namespace {

struct ScreenPoint {
    int16_t x;
    int16_t y;
};

void assertProjected(const MeshActor& actor, const MeshVertex& vertex,
                     const ScreenPoint& expected) {
    int16_t x = 0;
    int16_t y = 0;
    TEST_ASSERT_TRUE(actor.projectPoint(vertex.x, vertex.y, vertex.z, x, y));
    TEST_ASSERT_EQUAL_INT16(expected.x, x);
    TEST_ASSERT_EQUAL_INT16(expected.y, y);
}

int countLitPixels(const DisplayProxy& display) {
    int count = 0;
    for (int i = 0; i < 1024; ++i) {
        uint8_t value = display.fb[i];
        for (uint8_t bit = 0; bit < 8; ++bit) {
            if ((value & (uint8_t)(1U << bit)) != 0) ++count;
        }
    }
    return count;
}

}  // namespace

void test_default_state(void) {
    MeshActor actor;
    TEST_ASSERT_NULL(actor.mesh());
    TEST_ASSERT_EQUAL_INT16(0, actor.x());
    TEST_ASSERT_EQUAL_INT16(0, actor.y());
    TEST_ASSERT_EQUAL_UINT16(256, actor.scaleQ8());
    TEST_ASSERT_EQUAL_UINT8(0, actor.yaw());
    TEST_ASSERT_EQUAL_UINT8(0, actor.pitch());
    TEST_ASSERT_EQUAL_UINT8(0, actor.roll());
    TEST_ASSERT_EQUAL_UINT8(48, actor.perspective());
}

void test_sine_table_contract(void) {
    TEST_ASSERT_EQUAL_INT16(0, kSinQ14[0]);
    TEST_ASSERT_EQUAL_INT16(16384, kSinQ14[64]);
    TEST_ASSERT_EQUAL_INT16(0, kSinQ14[128]);
    TEST_ASSERT_EQUAL_INT16(-16384, kSinQ14[192]);
    TEST_ASSERT_EQUAL_INT16(16384, cosQ14(0));
    for (int i = 0; i < 256; ++i) {
        TEST_ASSERT_GREATER_OR_EQUAL_INT16(-16384, kSinQ14[i]);
        TEST_ASSERT_LESS_OR_EQUAL_INT16(16384, kSinQ14[i]);
        TEST_ASSERT_EQUAL_INT16(-kSinQ14[(i + 128) & 255], kSinQ14[i]);
    }
}

void test_yaw_quarter_turn_vertex_by_vertex(void) {
    // The pinned yaw expressions give (x,y,z) -> (-z,y,x) at angle 64.
    // With focal 110 and depth 48, denominators are 40 or 56, so
    // trunc(8*110/40)=22 and trunc(8*110/56)=15.
    static const ScreenPoint expected[] = {
        { 22,  22}, { 15,  15}, { 15, -15}, { 22, -22},
        {-22,  22}, {-15,  15}, {-15, -15}, {-22, -22},
    };
    MeshActor actor;
    actor.setRotation(64, 0, 0);
    for (uint8_t i = 0; i < cube_mesh.vertCount; ++i) {
        assertProjected(actor, cube_mesh.verts[i], expected[i]);
    }
}

void test_pitch_quarter_turn_vertex_by_vertex(void) {
    // Pitch 64 gives (x,y,z) -> (x,-z,y). The same hand divisions
    // 880/40=22 and trunc(880/56)=15 determine every coordinate.
    static const ScreenPoint expected[] = {
        {-22, -22}, { 22, -22}, { 15, -15}, {-15, -15},
        {-22,  22}, { 22,  22}, { 15,  15}, {-15,  15},
    };
    MeshActor actor;
    actor.setRotation(0, 64, 0);
    for (uint8_t i = 0; i < cube_mesh.vertCount; ++i) {
        assertProjected(actor, cube_mesh.verts[i], expected[i]);
    }
}

void test_roll_quarter_turn_vertex_by_vertex(void) {
    // Roll 64 gives (x,y,z) -> (-y,x,z). Original z selects denominator
    // 40 or 56; the screen y subtraction negates the transformed x.
    static const ScreenPoint expected[] = {
        { 22,  22}, { 22, -22}, {-22, -22}, {-22,  22},
        { 15,  15}, { 15, -15}, {-15, -15}, {-15,  15},
    };
    MeshActor actor;
    actor.setRotation(0, 0, 64);
    for (uint8_t i = 0; i < cube_mesh.vertCount; ++i) {
        assertProjected(actor, cube_mesh.verts[i], expected[i]);
    }
}

void test_yaw_half_turn_vertex_by_vertex(void) {
    // Yaw 128 multiplies x and z by -16384, exactly negating both.
    static const ScreenPoint expected[] = {
        { 15,  15}, {-15,  15}, {-15, -15}, { 15, -15},
        { 22,  22}, {-22,  22}, {-22, -22}, { 22, -22},
    };
    MeshActor actor;
    actor.setRotation(128, 0, 0);
    for (uint8_t i = 0; i < cube_mesh.vertCount; ++i) {
        assertProjected(actor, cube_mesh.verts[i], expected[i]);
    }
}

void test_rotation_wraparound(void) {
    MeshActor actor;
    actor.setPerspective(0);
    actor.setPos(20, 20);
    actor.setRotation(255, 0, 0);
    actor.spin(2, -1, -2);
    TEST_ASSERT_EQUAL_UINT8(1, actor.yaw());
    TEST_ASSERT_EQUAL_UINT8(255, actor.pitch());
    TEST_ASSERT_EQUAL_UINT8(254, actor.roll());

    int16_t zeroX = 0;
    int16_t zeroY = 0;
    int16_t wrappedX = 0;
    int16_t wrappedY = 0;
    actor.setRotation(0, 0, 0);
    TEST_ASSERT_TRUE(actor.projectPoint(3, -5, 7, zeroX, zeroY));
    actor.setRotation(static_cast<uint8_t>(256), static_cast<uint8_t>(256),
                      static_cast<uint8_t>(256));
    TEST_ASSERT_TRUE(actor.projectPoint(3, -5, 7, wrappedX, wrappedY));
    TEST_ASSERT_EQUAL_INT16(zeroX, wrappedX);
    TEST_ASSERT_EQUAL_INT16(zeroY, wrappedY);

    for (int i = 0; i < 256; ++i) actor.spin(1, 1, 1);
    TEST_ASSERT_TRUE(actor.projectPoint(3, -5, 7, wrappedX, wrappedY));
    TEST_ASSERT_EQUAL_INT16(zeroX, wrappedX);
    TEST_ASSERT_EQUAL_INT16(zeroY, wrappedY);
}

void test_scale_q8_hand_values(void) {
    // Orthographic projection exposes scale directly. For x=3:
    // 3*256>>8=3, 3*512>>8=6, and 3*128>>8=1. For x=-3 at half
    // scale, arithmetic shift floors -1.5 to -2.
    MeshActor actor;
    actor.setPerspective(0);
    int16_t x = 0;
    int16_t y = 0;

    actor.setScaleQ8(256);
    TEST_ASSERT_TRUE(actor.projectPoint(3, 0, 0, x, y));
    TEST_ASSERT_EQUAL_INT16(3, x);
    actor.setScaleQ8(512);
    TEST_ASSERT_TRUE(actor.projectPoint(3, 0, 0, x, y));
    TEST_ASSERT_EQUAL_INT16(6, x);
    actor.setScaleQ8(128);
    TEST_ASSERT_TRUE(actor.projectPoint(3, 0, 0, x, y));
    TEST_ASSERT_EQUAL_INT16(1, x);
    TEST_ASSERT_TRUE(actor.projectPoint(-3, 0, 0, x, y));
    TEST_ASSERT_EQUAL_INT16(-2, x);
}

void test_orthographic_anchor_and_screen_y(void) {
    // Cube vertex (8,-8,-8), identity scale: anchor (64,32) gives
    // x=64+8=72 and y=32-(-8)=40. Depth zero ignores z entirely.
    MeshActor actor;
    actor.setPerspective(0);
    actor.setPos(64, 32);
    int16_t x = 0;
    int16_t y = 0;
    TEST_ASSERT_TRUE(actor.projectPoint(8, -8, -8, x, y));
    TEST_ASSERT_EQUAL_INT16(72, x);
    TEST_ASSERT_EQUAL_INT16(40, y);
    TEST_ASSERT_TRUE(actor.projectPoint(8, -8, 120, x, y));
    TEST_ASSERT_EQUAL_INT16(72, x);
    TEST_ASSERT_EQUAL_INT16(40, y);
}

void test_behind_camera_guard_and_edge_skip(void) {
    // At default depth 48, z=-41 gives zc=7 (<8) and is culled;
    // z=-40 gives zc=8 and remains visible.
    int16_t x = 0;
    int16_t y = 0;
    MeshActor actor;
    TEST_ASSERT_FALSE(actor.projectPoint(0, 0, -41, x, y));
    TEST_ASSERT_TRUE(actor.projectPoint(0, 0, -40, x, y));

    static const MeshVertex verts[] = {{0, 0, -41}, {1, 0, 0}};
    static const uint8_t edges[][2] = {{0, 1}};
    static const Mesh mesh = {"culled-edge", verts, 2, edges, 1};
    DisplayProxy display;
    actor.setMesh(&mesh);
    actor.draw(display);
    TEST_ASSERT_EQUAL_INT(0, display.lineCount);
}

void test_trivial_reject_fully_off_one_side(void) {
    static const MeshVertex verts[] = {{-20, 0, 0}, {-10, 1, 0}};
    static const uint8_t edges[][2] = {{0, 1}};
    static const Mesh mesh = {"off-left", verts, 2, edges, 1};
    MeshActor actor;
    actor.setMesh(&mesh);
    actor.setPerspective(0);
    DisplayProxy display;
    actor.draw(display);
    TEST_ASSERT_EQUAL_INT(0, display.lineCount);
}

void test_crossing_edge_coordinates_are_clamped(void) {
    // At scale 65535, -127 scales to -32512 and +127 to +32511.
    // They straddle the display, so rejection does not apply and the issued
    // endpoints must be clamped to -1024 and 1023.
    static const MeshVertex verts[] = {{-127, 0, 0}, {127, 0, 0}};
    static const uint8_t edges[][2] = {{0, 1}};
    static const Mesh mesh = {"crossing", verts, 2, edges, 1};
    MeshActor actor;
    actor.setMesh(&mesh);
    actor.setPerspective(0);
    actor.setScaleQ8(65535);
    DisplayProxy display;
    actor.draw(display);
    TEST_ASSERT_EQUAL_INT(1, display.lineCount);
    TEST_ASSERT_EQUAL_INT16(-1024, display.lines[0].x0);
    TEST_ASSERT_EQUAL_INT16(0, display.lines[0].y0);
    TEST_ASSERT_EQUAL_INT16(1023, display.lines[0].x1);
    TEST_ASSERT_EQUAL_INT16(0, display.lines[0].y1);
}

void test_null_and_zero_edge_mesh_draw_nothing(void) {
    DisplayProxy display;
    MeshActor actor;
    actor.draw(display);
    TEST_ASSERT_EQUAL_INT(0, display.lineCount);

    static const Mesh empty = {"empty", nullptr, 0, nullptr, 0};
    actor.setMesh(&empty);
    actor.draw(display);
    TEST_ASSERT_EQUAL_INT(0, display.lineCount);
}

void test_zero_length_edge_lights_one_pixel(void) {
    static const MeshVertex verts[] = {{0, 0, 0}};
    static const uint8_t edges[][2] = {{0, 0}};
    static const Mesh mesh = {"point", verts, 1, edges, 1};
    MeshActor actor;
    actor.setMesh(&mesh);
    actor.setPerspective(0);
    actor.setPos(10, 12);
    DisplayProxy display;
    actor.draw(display);
    TEST_ASSERT_EQUAL_INT(1, display.lineCount);
    TEST_ASSERT_EQUAL_INT(1, countLitPixels(display));
    TEST_ASSERT_EQUAL_HEX8(0x10, display.fb[10 + 128]);
}

void test_reference_rasterizer_normalizes_endpoint_order(void) {
    // For (2,1)->(5,3), dx=3, dy=2, initial err=1. The canonical loop
    // lights (2,1), (3,2), (4,2), (5,3). Reversing endpoints must produce
    // the same pixels after its x-order normalization.
    DisplayProxy forward;
    DisplayProxy reverse;
    forward.drawLine(2, 1, 5, 3);
    reverse.drawLine(5, 3, 2, 1);
    TEST_ASSERT_EQUAL_INT(4, countLitPixels(forward));
    TEST_ASSERT_EQUAL_INT(4, countLitPixels(reverse));
    for (int i = 0; i < 1024; ++i) {
        TEST_ASSERT_EQUAL_HEX8(forward.fb[i], reverse.fb[i]);
    }
    TEST_ASSERT_EQUAL_HEX8(0x02, forward.fb[2]);
    TEST_ASSERT_EQUAL_HEX8(0x04, forward.fb[3]);
    TEST_ASSERT_EQUAL_HEX8(0x04, forward.fb[4]);
    TEST_ASSERT_EQUAL_HEX8(0x08, forward.fb[5]);
    TEST_ASSERT_EQUAL_INT16(5, reverse.lines[0].x0);
    TEST_ASSERT_EQUAL_INT16(3, reverse.lines[0].y0);
    TEST_ASSERT_EQUAL_INT16(2, reverse.lines[0].x1);
    TEST_ASSERT_EQUAL_INT16(1, reverse.lines[0].y1);

    // The steep transpose of that line lights (1,2), (2,3), (2,4), (3,5).
    DisplayProxy steep;
    steep.drawLine(1, 2, 3, 5);
    TEST_ASSERT_EQUAL_INT(4, countLitPixels(steep));
    TEST_ASSERT_EQUAL_HEX8(0x04, steep.fb[1]);
    TEST_ASSERT_EQUAL_HEX8(0x18, steep.fb[2]);
    TEST_ASSERT_EQUAL_HEX8(0x20, steep.fb[3]);
}

void test_display_reset_clears_mesh_records(void) {
    DisplayProxy display;
    display.drawLine(1, 1, 3, 1);
    TEST_ASSERT_EQUAL_INT(1, display.lineCount);
    TEST_ASSERT_GREATER_THAN_INT(0, countLitPixels(display));
    display.reset();
    TEST_ASSERT_EQUAL_INT(0, display.lineCount);
    TEST_ASSERT_EQUAL_INT(0, countLitPixels(display));
    for (int i = 0; i < DisplayProxy::kMaxRecordedLines; ++i) {
        TEST_ASSERT_EQUAL_INT16(0, display.lines[i].x0);
        TEST_ASSERT_EQUAL_INT16(0, display.lines[i].y0);
        TEST_ASSERT_EQUAL_INT16(0, display.lines[i].x1);
        TEST_ASSERT_EQUAL_INT16(0, display.lines[i].y1);
    }
}

void setUp(void)    {}
void tearDown(void) {}

int main(int /*argc*/, char** /*argv*/) {
    UNITY_BEGIN();
    RUN_TEST(test_default_state);
    RUN_TEST(test_sine_table_contract);
    RUN_TEST(test_yaw_quarter_turn_vertex_by_vertex);
    RUN_TEST(test_pitch_quarter_turn_vertex_by_vertex);
    RUN_TEST(test_roll_quarter_turn_vertex_by_vertex);
    RUN_TEST(test_yaw_half_turn_vertex_by_vertex);
    RUN_TEST(test_rotation_wraparound);
    RUN_TEST(test_scale_q8_hand_values);
    RUN_TEST(test_orthographic_anchor_and_screen_y);
    RUN_TEST(test_behind_camera_guard_and_edge_skip);
    RUN_TEST(test_trivial_reject_fully_off_one_side);
    RUN_TEST(test_crossing_edge_coordinates_are_clamped);
    RUN_TEST(test_null_and_zero_edge_mesh_draw_nothing);
    RUN_TEST(test_zero_length_edge_lights_one_pixel);
    RUN_TEST(test_reference_rasterizer_normalizes_endpoint_order);
    RUN_TEST(test_display_reset_clears_mesh_records);
    return UNITY_END();
}
