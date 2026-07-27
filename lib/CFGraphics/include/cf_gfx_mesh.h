// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

// lib/CFGraphics/include/cf_gfx_mesh.h
//
/**
 * Integer 3D wireframe mesh runtime.
 *
 * The invariant implemented here is that firmware, host tests, and browser
 * previews can use the same binary-angle fixed-point operations and produce
 * identical projected lines. Edges with a culled endpoint are skipped;
 * near-plane clipping is deliberately not part of this first mesh runtime.
 */

#ifndef CF_GFX_MESH_H
#define CF_GFX_MESH_H

#include <stdint.h>
#include <type_traits>

#ifdef HOST_TEST
#include "cf_gfx_sprite.h"  // Native recording DisplayProxy stand-in.
#else
#include <Arduino.h>
#include "DisplayProxy.h"
#endif  // HOST_TEST

namespace cf { namespace gfx {

/**
 * CROSS-LANGUAGE PARITY CONTRACT
 *
 * 1. kSinQ14 has 256 signed Q14 entries. Entry i is the constant
 *    round-half-away-from-zero value of 16384 * sin(i * PI / 128).
 *    sinQ14(a) indexes a & 255; cosQ14(a) indexes (a + 64) & 255.
 * 2. Copy the model coordinates to signed 32-bit values, then rotate yaw
 *    about Y, pitch about X, and roll about Z, using exactly these operations
 *    in this order:
 *      x1=(vx*cy-vz*sy)>>14; z1=(vx*sy+vz*cy)>>14; y1=vy;
 *      y2=(y1*cp-z1*sp)>>14; z2=(y1*sp+z1*cp)>>14; x2=x1;
 *      x3=(x2*cr-y2*sr)>>14; y3=(x2*sr+y2*cr)>>14; z3=z2;
 *    Then scale:
 *      xs=(x3*scaleQ8)>>8; ys=(y3*scaleQ8)>>8;
 *      zs=(z3*scaleQ8)>>8.
 * 3. Every value and intermediate is signed 32-bit except the declared
 *    unsigned actor settings. Each >> is an arithmetic right shift with no
 *    rounding addend: it floors negative values and matches JavaScript >>.
 *    Perspective uses signed division, not a shift: division truncates toward
 *    zero and must match JavaScript Math.trunc(numerator / denominator).
 * 4. Pinned values are kFocal=110, kNearZ=8, default depth=48, default
 *    scaleQ8=256, where 256 is the 1.0 scale base. Binary uint8 angles map
 *    0..255 to one full turn. Model +x is right, +y is up, +z is toward the
 *    viewer; screen +x is right and +y is down.
 * 5. With depth 0, every vertex is visible and projects to
 *    (anchorX+xs, anchorY-ys). Otherwise zc=zs+depth; zc<kNearZ is invisible,
 *    and a visible vertex projects to
 *    (anchorX+trunc(xs*kFocal/zc), anchorY-trunc(ys*kFocal/zc)).
 *    Draw an edge only when both endpoints are visible. There is no near-plane
 *    clipping: an edge with one invisible endpoint is skipped.
 * 6. Before drawing, trivially reject an edge when both endpoints have x<0,
 *    both have x>127, both have y<0, or both have y>63.
 * 7. After that rejection, clamp every endpoint coordinate to [-1024, 1023]
 *    before issuing drawLine. Apply the rejection and clamp in that order.
 *    projectPoint applies the same clamp to its outputs, so a point-anchored
 *    caller and a line endpoint agree on where an off-screen position lands.
 * 8. Signed 32-bit overflow is the one way a port can diverge here. Every
 *    step above stays in range for int8 mesh vertices, but projectPoint
 *    accepts wider inputs: keep coordinates within about 8192. A JavaScript
 *    port using doubles will not overflow where C++ does, so an out-of-range
 *    input produces agreement failures rather than an obvious error.
 */

struct MeshVertex {
    int8_t x;
    int8_t y;
    int8_t z;
};

struct Mesh {
    const char*       name;
    const MeshVertex* verts;
    uint8_t           vertCount;
    const uint8_t   (*edges)[2];
    uint16_t          edgeCount;
};

static_assert(std::is_trivial<MeshVertex>::value &&
                  std::is_standard_layout<MeshVertex>::value,
              "MeshVertex must stay POD (PROGMEM-friendly)");
static_assert(std::is_trivial<Mesh>::value &&
                  std::is_standard_layout<Mesh>::value,
              "Mesh must stay POD (PROGMEM-friendly)");

inline constexpr int16_t kSinQ14[256] = {
    0, 402, 804, 1205, 1606, 2006, 2404, 2801, 3196, 3590, 3981, 4370, 4756, 5139, 5520, 5897,
    6270, 6639, 7005, 7366, 7723, 8076, 8423, 8765, 9102, 9434, 9760, 10080, 10394, 10702, 11003, 11297,
    11585, 11866, 12140, 12406, 12665, 12916, 13160, 13395, 13623, 13842, 14053, 14256, 14449, 14635, 14811, 14978,
    15137, 15286, 15426, 15557, 15679, 15791, 15893, 15986, 16069, 16143, 16207, 16261, 16305, 16340, 16364, 16379,
    16384, 16379, 16364, 16340, 16305, 16261, 16207, 16143, 16069, 15986, 15893, 15791, 15679, 15557, 15426, 15286,
    15137, 14978, 14811, 14635, 14449, 14256, 14053, 13842, 13623, 13395, 13160, 12916, 12665, 12406, 12140, 11866,
    11585, 11297, 11003, 10702, 10394, 10080, 9760, 9434, 9102, 8765, 8423, 8076, 7723, 7366, 7005, 6639,
    6270, 5897, 5520, 5139, 4756, 4370, 3981, 3590, 3196, 2801, 2404, 2006, 1606, 1205, 804, 402,
    0, -402, -804, -1205, -1606, -2006, -2404, -2801, -3196, -3590, -3981, -4370, -4756, -5139, -5520, -5897,
    -6270, -6639, -7005, -7366, -7723, -8076, -8423, -8765, -9102, -9434, -9760, -10080, -10394, -10702, -11003, -11297,
    -11585, -11866, -12140, -12406, -12665, -12916, -13160, -13395, -13623, -13842, -14053, -14256, -14449, -14635, -14811, -14978,
    -15137, -15286, -15426, -15557, -15679, -15791, -15893, -15986, -16069, -16143, -16207, -16261, -16305, -16340, -16364, -16379,
    -16384, -16379, -16364, -16340, -16305, -16261, -16207, -16143, -16069, -15986, -15893, -15791, -15679, -15557, -15426, -15286,
    -15137, -14978, -14811, -14635, -14449, -14256, -14053, -13842, -13623, -13395, -13160, -12916, -12665, -12406, -12140, -11866,
    -11585, -11297, -11003, -10702, -10394, -10080, -9760, -9434, -9102, -8765, -8423, -8076, -7723, -7366, -7005, -6639,
    -6270, -5897, -5520, -5139, -4756, -4370, -3981, -3590, -3196, -2801, -2404, -2006, -1606, -1205, -804, -402,
};

inline int16_t sinQ14(uint8_t a) { return kSinQ14[a & 0xFF]; }
inline int16_t cosQ14(uint8_t a) { return kSinQ14[(a + 64) & 0xFF]; }

inline constexpr int32_t kFocal = 110;
inline constexpr int32_t kNearZ = 8;
inline constexpr uint8_t kDefaultDepth = 48;
inline constexpr uint16_t kScaleBaseQ8 = 256;

class MeshActor {
public:
    MeshActor()
        : mesh_(nullptr), x_(0), y_(0), scaleQ8_(kScaleBaseQ8), yaw_(0),
          pitch_(0), roll_(0), depth_(kDefaultDepth) {}

    void setMesh(const Mesh* m) { mesh_ = m; }
    const Mesh* mesh() const { return mesh_; }

    void setPos(int16_t sx, int16_t sy) { x_ = sx; y_ = sy; }
    int16_t x() const { return x_; }
    int16_t y() const { return y_; }

    void setScaleQ8(uint16_t s) { scaleQ8_ = s; }
    uint16_t scaleQ8() const { return scaleQ8_; }

    void setRotation(uint8_t yaw, uint8_t pitch, uint8_t roll) {
        yaw_ = yaw;
        pitch_ = pitch;
        roll_ = roll;
    }

    void spin(int8_t dYaw, int8_t dPitch, int8_t dRoll) {
        yaw_ = (uint8_t)(yaw_ + dYaw);
        pitch_ = (uint8_t)(pitch_ + dPitch);
        roll_ = (uint8_t)(roll_ + dRoll);
    }

    uint8_t yaw() const { return yaw_; }
    uint8_t pitch() const { return pitch_; }
    uint8_t roll() const { return roll_; }

    void setPerspective(uint8_t depth) { depth_ = depth; }
    uint8_t perspective() const { return depth_; }

    void draw(DisplayProxy& d) const {
        if (mesh_ == nullptr || mesh_->edgeCount == 0) return;

        for (uint16_t i = 0; i < mesh_->edgeCount; ++i) {
            const uint8_t a = mesh_->edges[i][0];
            const uint8_t b = mesh_->edges[i][1];
            int32_t x0, y0, x1, y1;
            if (!projectRaw(mesh_->verts[a].x, mesh_->verts[a].y,
                            mesh_->verts[a].z, x0, y0) ||
                !projectRaw(mesh_->verts[b].x, mesh_->verts[b].y,
                            mesh_->verts[b].z, x1, y1)) {
                continue;
            }

            if ((x0 < 0 && x1 < 0) || (x0 > 127 && x1 > 127) ||
                (y0 < 0 && y1 < 0) || (y0 > 63 && y1 > 63)) {
                continue;
            }

            x0 = clampDrawCoordinate(x0);
            y0 = clampDrawCoordinate(y0);
            x1 = clampDrawCoordinate(x1);
            y1 = clampDrawCoordinate(y1);
            d.drawLine((int16_t)x0, (int16_t)y0, (int16_t)x1, (int16_t)y1);
        }
    }

    // Projects one model-space point through the current transform, so that
    // anything anchored to a point on the model (a sprite riding a node, for
    // instance) lands where the wireframe lands.
    //
    // Outputs are clamped to the same [-1024, 1023] range draw() clamps line
    // endpoints to, so a caller drawing at this point and an edge ending at
    // it agree. Without the clamp a far-off point would wrap on the cast to
    // int16_t and land somewhere arbitrary on screen.
    //
    // PARITY LIMIT: keep |mx|, |my|, |mz| within about 8192. The rotation
    // multiplies a coordinate by up to 16384 and sums two such terms, so
    // larger inputs overflow signed 32-bit arithmetic here. JavaScript, whose
    // numbers are doubles, would NOT overflow at the same input and would
    // silently disagree with the device. Callers working in world units
    // should scale down before projecting.
    bool projectPoint(int32_t mx, int32_t my, int32_t mz,
                      int16_t& outX, int16_t& outY) const {
        int32_t sx, sy;
        if (!projectRaw(mx, my, mz, sx, sy)) return false;
        outX = (int16_t)clampDrawCoordinate(sx);
        outY = (int16_t)clampDrawCoordinate(sy);
        return true;
    }

private:
    static int32_t clampDrawCoordinate(int32_t value) {
        if (value < -1024) return -1024;
        if (value > 1023) return 1023;
        return value;
    }

    bool projectRaw(int32_t vx, int32_t vy, int32_t vz,
                    int32_t& sx, int32_t& sy) const {
        const int32_t cy = cosQ14(yaw_);
        const int32_t syaw = sinQ14(yaw_);
        const int32_t x1 = (vx * cy - vz * syaw) >> 14;
        const int32_t z1 = (vx * syaw + vz * cy) >> 14;
        const int32_t y1 = vy;

        const int32_t cp = cosQ14(pitch_);
        const int32_t sp = sinQ14(pitch_);
        const int32_t y2 = (y1 * cp - z1 * sp) >> 14;
        const int32_t z2 = (y1 * sp + z1 * cp) >> 14;
        const int32_t x2 = x1;

        const int32_t cr = cosQ14(roll_);
        const int32_t sr = sinQ14(roll_);
        const int32_t x3 = (x2 * cr - y2 * sr) >> 14;
        const int32_t y3 = (x2 * sr + y2 * cr) >> 14;
        const int32_t z3 = z2;

        const int32_t xs = (x3 * (int32_t)scaleQ8_) >> 8;
        const int32_t ys = (y3 * (int32_t)scaleQ8_) >> 8;
        const int32_t zs = (z3 * (int32_t)scaleQ8_) >> 8;

        if (depth_ == 0) {
            sx = (int32_t)x_ + xs;
            sy = (int32_t)y_ - ys;
            return true;
        }

        const int32_t zc = zs + (int32_t)depth_;
        if (zc < kNearZ) return false;
        sx = (int32_t)x_ + (xs * kFocal) / zc;
        sy = (int32_t)y_ - (ys * kFocal) / zc;
        return true;
    }

    const Mesh* mesh_;
    int16_t x_;
    int16_t y_;
    uint16_t scaleQ8_;
    uint8_t yaw_;
    uint8_t pitch_;
    uint8_t roll_;
    uint8_t depth_;
};

}}  // namespace cf::gfx

#endif  // CF_GFX_MESH_H
