// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

// lib/BreakoutGame/BreakoutMath.h
//
// Pure-logic free functions extracted from BreakoutGame so they can be
// exercised by `pio test` without dragging in HAL / DisplayProxy /
// AudioManager / Arduino. Anything stateless and side-effect-free that
// BreakoutGame uses lives here.
//
// Used by both the production BreakoutGame.cpp and the native test target
// in lib/BreakoutGame/test/.

#ifndef BREAKOUT_MATH_H
#define BREAKOUT_MATH_H

#include <math.h>
#include <stdint.h>

namespace BreakoutMath {

// Game-physics tunables — single source of truth so tests can reference them.
inline constexpr float MAX_ANGLE_RAD     = 1.0f;   // ~57° off vertical at paddle edges
inline constexpr int   TEXTURE_FROM_LEVEL = 5;     // L6+ (0-indexed) gets textured paddle
inline constexpr float TEXTURE_FREQ_RAD  = 10.0f;  // ~10 cycles across hitPos [-1, +1]
inline constexpr float TEXTURE_AMPLITUDE = 0.25f;  // ~14° max perturbation

inline constexpr int   GRAVITY_FROM_LEVEL = 7;     // L8+ (0-indexed) gets parabolic ball motion
inline constexpr float GRAVITY_PER_FRAME  = 0.03f; // velocity units added to vY each frame

inline constexpr float SPIKE_ONESHOT_MULT = 1.5f;  // one-shot spike speed kick on destroy
inline constexpr float SPIKE_PERSIST_MULT = 1.3f;  // persistent spike speed kick per hit

inline float clampf(float v, float lo, float hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

// Compute the normalized hit position on the paddle: -1 at left edge, 0 center, +1 right.
// Inputs: ball's leftmost x + size, paddle's leftmost x + width.
inline float computeHitPos(float ballX, int ballSize, float paddleX, int paddleWidth) {
    const float paddleCenter = paddleX + paddleWidth / 2.0f;
    const float ballCenter   = ballX + ballSize / 2.0f;
    const float hitPos       = (ballCenter - paddleCenter) / (paddleWidth / 2.0f);
    return clampf(hitPos, -1.0f, 1.0f);
}

// Classic-Breakout paddle reflection with optional textured perturbation.
// Inputs are the ball's current velocity + hit position + which level we're on.
// Outputs are written through outVX / outVY. Magnitude (speed) is preserved.
// The textured perturbation activates only when currentLevel >= TEXTURE_FROM_LEVEL,
// and is deterministic in hitPos (same input -> same output).
inline void reflectFromPaddle(float hitPos, float vxIn, float vyIn, int currentLevel,
                              float& outVX, float& outVY) {
    const float baseAngle = clampf(hitPos, -1.0f, 1.0f) * MAX_ANGLE_RAD;
    float angle = baseAngle;
    if (currentLevel >= TEXTURE_FROM_LEVEL) {
        angle += sinf(hitPos * TEXTURE_FREQ_RAD) * TEXTURE_AMPLITUDE;
    }
    float speed = sqrtf(vxIn * vxIn + vyIn * vyIn);
    if (speed < 0.1f) speed = 1.0f; // safety; shouldn't happen in normal play
    outVX = speed * sinf(angle);
    outVY = -speed * cosf(angle); // always heads upward after paddle hit
}

// Map slider percentage [0..100] to paddle left-edge X.
// Linear: slider=0 -> paddle at left wall, slider=100 -> paddle at right wall.
// Returns 0 on out-of-range input (slider hardware is supposed to clamp, but we double-check).
inline float paddleXFromSlider(float sliderPct, int screenWidth, int paddleWidth) {
    const float pct = clampf(sliderPct, 0.0f, 100.0f);
    return (pct / 100.0f) * (float)(screenWidth - paddleWidth);
}

// Apply one frame of accelerometer-driven motion to paddleX.
// `accelX` is the raw HAL global; `paddleSpeed` is BreakoutGame's tunable multiplier.
// Returns the new clamped paddleX.
inline float paddleXFromAccel(float currentPaddleX, float accelX, float paddleSpeed,
                              int screenWidth, int paddleWidth) {
    // Negative sign matches existing physical convention (tilt left -> paddle moves left).
    float next = currentPaddleX + (accelX * paddleSpeed * -0.01f);
    if (next < 0.0f) next = 0.0f;
    if (next + paddleWidth > screenWidth) next = (float)(screenWidth - paddleWidth);
    return next;
}

// Validate one level layout row. Returns true if it's exactly `cols` chars long
// and every char is one of:
//   B = brick (1 hit, breakable)
//   U = unbreakable (persists)
//   C = crumble (3 hits, breakable)
//   S = spike, one-shot (1 hit, breakable, speed kick)
//   P = spike, persistent (persists, speed kick per hit)
//   . = empty
inline bool isLayoutRowValid(const char* row, int cols) {
    if (row == nullptr) return false;
    for (int i = 0; i < cols; i++) {
        const char ch = row[i];
        if (ch == '\0') return false; // too short
        if (ch != 'B' && ch != 'U' && ch != 'C' &&
            ch != 'S' && ch != 'P' && ch != '.') return false;
    }
    return row[cols] == '\0'; // exact length
}

} // namespace BreakoutMath

#endif // BREAKOUT_MATH_H
