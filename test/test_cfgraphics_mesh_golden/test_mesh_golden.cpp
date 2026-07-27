// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

// test/test_cfgraphics_mesh_golden/test_mesh_golden.cpp
//
// DRIFT LOCK, not a proof of correctness: this certifies that mesh behavior
// has not changed. test_mesh.cpp carries the independent hand-derived proofs.

#include <stdio.h>
#include <unity.h>

#include "cf_gfx.h"
#include "cf_gfx_mesh_golden.h"
#include "cf_gfx_mesh_ship_fixture.h"

using namespace cf::gfx;
using namespace cf::gfx::meshfixtures;
using namespace cf::gfx::meshgolden;

namespace {

void failAt(const char* kind, uint8_t pose, int index) {
    char message[96];
    snprintf(message, sizeof(message), "pose %u first differing %s index %d",
             static_cast<unsigned>(pose), kind, index);
    TEST_FAIL_MESSAGE(message);
}

void assertPose(const GoldenPose& expected) {
    MeshActor actor;
    actor.setMesh(&ship_mesh);
    actor.setPos(expected.anchorX, expected.anchorY);
    actor.setScaleQ8(expected.scaleQ8);
    actor.setRotation(expected.yaw, expected.pitch, expected.roll);
    actor.setPerspective(expected.depth);

    TEST_ASSERT_EQUAL_UINT8_MESSAGE(
        ship_mesh.vertCount, expected.pointCount, "golden point count");
    for (uint8_t i = 0; i < expected.pointCount; ++i) {
        int16_t x = 0;
        int16_t y = 0;
        const MeshVertex& vertex = ship_mesh.verts[i];
        const bool visible = actor.projectPoint(vertex.x, vertex.y, vertex.z, x, y);
        const GoldenPoint& point = expected.points[i];
        if (point.index != i || point.visible != visible ||
            (visible && (point.x != x || point.y != y))) {
            failAt("point", expected.index, i);
        }
    }

    DisplayProxy display;
    actor.draw(display);
    if (display.lineCount != expected.lineCount) {
        failAt("line-count", expected.index, display.lineCount);
    }
    for (uint8_t i = 0; i < expected.lineCount; ++i) {
        const DisplayProxy::LineCall& actual = display.lines[i];
        const GoldenLine& line = expected.lines[i];
        if (actual.x0 != line.x0 || actual.y0 != line.y0 ||
            actual.x1 != line.x1 || actual.y1 != line.y1) {
            failAt("line", expected.index, i);
        }
    }

    for (int i = 0; i < 1024; ++i) {
        if (display.fb[i] != expected.framebuffer[i]) {
            failAt("framebuffer-byte", expected.index, i);
        }
    }
}

}  // namespace

void test_all_golden_poses(void) {
    TEST_ASSERT_EQUAL_UINT8(8, pose_count);
    for (uint8_t i = 0; i < pose_count; ++i) {
        TEST_ASSERT_EQUAL_UINT8(i, poses[i].index);
        assertPose(poses[i]);
    }
}

void setUp(void)    {}
void tearDown(void) {}

int main(int /*argc*/, char** /*argv*/) {
    UNITY_BEGIN();
    RUN_TEST(test_all_golden_poses);
    return UNITY_END();
}
