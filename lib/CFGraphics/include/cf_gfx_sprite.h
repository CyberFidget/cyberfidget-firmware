// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

// lib/CFGraphics/include/cf_gfx_sprite.h
//
// cf::gfx core asset types: Sprite, Animation, SpriteSheet — plus the
// stateless draw wrappers and findAnimation(). Phase 1 of the sprite &
// animation framework (T-111; design N-033; invariants REQ-042).
//
// The types are POD and PROGMEM-friendly so the T-112 converter can emit
// them as header-only `inline constexpr` data next to each app
// (lib/<App>/generated/*.h). This library holds runtime types ONLY — no
// assets live here (REQ-042.2: apps own their assets).
//
// Bit format contract (pinned by test/test_cfgraphics_collision):
//   Sprite::data is 1-bit XBM: row-major, ((w + 7) / 8) bytes per row,
//   LSB-first within each byte — bit (x & 7) of byte
//   data[y * bytesPerRow + (x >> 3)] is pixel (x, y). This matches both
//   DisplayProxy::drawXbm and the browser-side .cfsprite v1 profile
//   (cyberfidget_website/assets/js/models/cfsprite.mjs), so converter
//   output is a straight byte copy.
//
// Pure C++: compiles unchanged into firmware, the WASM emulator, and the
// native host tests (pio test -e test_cfgraphics). Flash data is assumed
// memory-mapped (ESP32); pgm_read_byte is used for pixel bytes to match
// surrounding firmware style, and degrades to a plain read on host.

#ifndef CF_GFX_SPRITE_H
#define CF_GFX_SPRITE_H

#include <stdint.h>
#include <string.h>
#include <type_traits>

#ifdef HOST_TEST
#include <stdlib.h>
#include <utility>

// Native host build (pio test -e test_cfgraphics): no Arduino runtime, no
// OLED driver (lib_ldf_mode = off keeps them out on purpose).
#ifndef PROGMEM
#define PROGMEM
#endif
#ifndef pgm_read_byte
#define pgm_read_byte(addr) (*(const uint8_t*)(addr))
#endif

// Minimal recording stand-in for lib/DisplayProxy's DisplayProxy. It
// mirrors the one member cf::gfx calls — drawXbm, same signature — so the
// real drawSprite / drawSpritePivoted / Actor::draw code paths compile and
// run on host, and tests can assert on the coordinates handed to the HAL
// output contract (REQ-OUT-001). Full DisplayProxy stubbing stays punted
// to T-002's WASM SIL harness (see lib/HALMock/HALMock.h scope note).
class DisplayProxy {
public:
    struct XbmCall {
        int16_t x;
        int16_t y;
        int16_t w;
        int16_t h;
        const unsigned char* data;
    };

    static constexpr int kMaxRecordedCalls = 16;

    XbmCall calls[kMaxRecordedCalls] = {};
    int callCount = 0;

    struct LineCall {
        int16_t x0;
        int16_t y0;
        int16_t x1;
        int16_t y1;
    };

    static constexpr int kMaxRecordedLines = 64;

    uint8_t fb[1024] = {};
    LineCall lines[kMaxRecordedLines] = {};
    int lineCount = 0;

    void drawXbm(int16_t x, int16_t y, int16_t w, int16_t h,
                 const unsigned char* data) {
        if (callCount < kMaxRecordedCalls) {
            calls[callCount] = XbmCall{x, y, w, h, data};
        }
        ++callCount;
    }

    void setPixel(int16_t x, int16_t y) {
        if (x < 0 || x >= 128 || y < 0 || y >= 64) return;
        fb[x + (y / 8) * 128] |= (uint8_t)(1U << (y & 7));
    }

    // Transcription of OLEDDisplay::drawLine from the ThingPulse SSD1306
    // driver. It must be kept in step with that device rasterizer.
    void drawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1) {
        if (lineCount < kMaxRecordedLines) {
            lines[lineCount] = LineCall{x0, y0, x1, y1};
        }
        ++lineCount;

        using std::swap;
        int16_t steep = abs(y1 - y0) > abs(x1 - x0);
        if (steep) { swap(x0, y0); swap(x1, y1); }
        if (x0 > x1) { swap(x0, x1); swap(y0, y1); }
        int16_t dx = x1 - x0;
        int16_t dy = abs(y1 - y0);
        int16_t err = dx / 2;
        int16_t ystep = (y0 < y1) ? 1 : -1;
        for (; x0 <= x1; x0++) {
            if (steep) setPixel(y0, x0); else setPixel(x0, y0);
            err -= dy;
            if (err < 0) { y0 += ystep; err += dx; }
        }
    }

    const uint8_t* frameBuffer() const { return fb; }

    void reset() {
        callCount = 0;
        lineCount = 0;
        memset(fb, 0, sizeof(fb));
        memset(lines, 0, sizeof(lines));
    }
};
#else
#include <Arduino.h>       // PROGMEM / pgm_read_byte (no-op reads on ESP32)
#include "DisplayProxy.h"  // HAL output contract (REQ-OUT-001)
#endif  // HOST_TEST

namespace cf { namespace gfx {

enum LoopMode : uint8_t {
    LOOP_ONCE      = 0,  // play to last frame, then stop: finished()=true and
                         // sprite() returns nullptr (draw nothing)
    LOOP_ONCE_HOLD = 1,  // play to last frame, hold it forever (finished()=true,
                         // sprite() keeps returning the last frame)
    LOOP_LOOP      = 2,  // wrap to frame 0 after the last frame
    LOOP_PINGPONG  = 3,  // 0..N-1..1 then repeat (each end shown once per pass)
};

enum BitOrder : uint8_t {
    BO_LSB_FIRST = 0,  // XBM order — what drawXbm renders and the converter emits
    BO_MSB_FIRST = 1,  // legacy/compat only; drawXbm does NOT honor this — a
                       // MSB-first sprite draws per-byte mirrored on screen
};

// A single 1-bit image cel in PROGMEM. POD; safe in flash.
struct Sprite {
    const uint8_t* data;   // PROGMEM ptr; XBM bytes, ((w + 7) / 8) per row
    uint16_t w;
    uint16_t h;
    int8_t   pivotX;       // local anchor; default 0,0 (top-left)
    int8_t   pivotY;
    BitOrder bitOrder;     // expected: BO_LSB_FIRST for drawXbm
};

// An Animation = ordered frames + per-frame durations + loop policy.
// A duration of 0 ms means "hold this frame indefinitely" (fps 0 in the
// .cfsprite source, e.g. a one-pose "jump") — the player never advances.
struct Animation {
    const char*          name;          // PROGMEM C string, e.g. "run"
    const Sprite* const* frames;        // PROGMEM array of Sprite*
    const uint16_t*      durations_ms;  // PROGMEM per-frame ms; nullptr -> uniform
    uint16_t             uniformMs;     // used iff durations_ms == nullptr
    uint8_t              frameCount;
    LoopMode             loop;
};

// A SpriteSheet groups named Animations sharing a coordinate system.
struct SpriteSheet {
    const char*      name;
    const Animation* animations;      // PROGMEM array
    uint8_t          animationCount;
    const char*      kind;            // advisory: "sprite"|"character"|"icon"|
                                      // "screensaver"|"tile"; runtime ignores it;
                                      // tooling (Archives, gallery) filters on it
};

// POD guarantee — keeps the types PROGMEM-safe and lets the T-112 converter
// emit them as constexpr aggregates.
static_assert(std::is_trivial<Sprite>::value && std::is_standard_layout<Sprite>::value,
              "Sprite must stay POD (PROGMEM-friendly)");
static_assert(std::is_trivial<Animation>::value && std::is_standard_layout<Animation>::value,
              "Animation must stay POD (PROGMEM-friendly)");
static_assert(std::is_trivial<SpriteSheet>::value && std::is_standard_layout<SpriteSheet>::value,
              "SpriteSheet must stay POD (PROGMEM-friendly)");

// Draw a Sprite with its top-left at (x, y) — pivot ignored. Wraps drawXbm
// (the HAL output contract); never touches the display driver directly.
inline void drawSprite(DisplayProxy& d, const Sprite& s, int16_t x, int16_t y) {
    if (s.data == nullptr) return;
    d.drawXbm(x, y, (int16_t)s.w, (int16_t)s.h, s.data);
}

// Draw a Sprite anchored by its pivot — (x, y) is where the pivot lands,
// i.e. the bitmap's top-left goes to (x - pivotX, y - pivotY).
void drawSpritePivoted(DisplayProxy& d, const Sprite& s, int16_t x, int16_t y);

// O(N) name lookup over a sheet's Animations; returns nullptr on miss.
// Fine for state transitions (a few calls per second); for per-tick
// selection use Animation pointers instead (see AnimationPlayer::setIfChanged).
const Animation* findAnimation(const SpriteSheet& sheet, const char* name);

}}  // namespace cf::gfx

#endif  // CF_GFX_SPRITE_H
