// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC
//
// Device-side implementation of the "cf" wasm import surface (see
// wasm/device_module/cf_hal_imports.h for the guest-side declarations and
// docs/SPIKE_WASM_IMPORT_SURFACE.md for the catalog). Also stubs the handful
// of WASI/env imports Emscripten standalone modules may reference so a guest
// that touches them fails politely instead of failing to link.

#include "WasmHostImports.h"

#include <string.h>

#include <Arduino.h>
#include <esp_random.h>

#include "AudioManager.h"
#include "ButtonManager.h"
#include "DisplayProxy.h"
#include "HAL.h"
#include "globals.h"

// ---------------------------------------------------------------------------
// Deferred exit-to-menu flag
// ---------------------------------------------------------------------------

static volatile bool s_exitRequested = false;

bool wasmHostConsumeExitRequest() {
    bool requested = s_exitRequested;
    s_exitRequested = false;
    return requested;
}

void wasmHostClearExitRequest() { s_exitRequested = false; }

// ---------------------------------------------------------------------------
// cf.* — benchmark helper
// ---------------------------------------------------------------------------

// cf.nop(i32)->i32 — returns x+1 so guest benchmark loops carry a data
// dependency and can't be optimized away.
static m3ApiRawFunction(cfNop) {
    m3ApiReturnType(int32_t)
    m3ApiGetArg(int32_t, x)
    m3ApiReturn(x + 1)
}

// ---------------------------------------------------------------------------
// cf.* — display
// ---------------------------------------------------------------------------

static m3ApiRawFunction(cfDisplayClear) {
    HAL::displayProxy().clear();
    m3ApiSuccess()
}

static m3ApiRawFunction(cfDisplayShow) {
    HAL::displayProxy().display();
    m3ApiSuccess()
}

static m3ApiRawFunction(cfDisplaySetColor) {
    m3ApiGetArg(int32_t, color)
    OLEDDISPLAY_COLOR c = WHITE;
    if (color == 0) c = BLACK;
    else if (color == 2) c = INVERSE;
    HAL::displayProxy().setColor(c);
    m3ApiSuccess()
}

static m3ApiRawFunction(cfDisplaySetPixel) {
    m3ApiGetArg(int32_t, x)
    m3ApiGetArg(int32_t, y)
    HAL::displayProxy().setPixel(x, y);
    m3ApiSuccess()
}

static m3ApiRawFunction(cfDisplayDrawLine) {
    m3ApiGetArg(int32_t, x0)
    m3ApiGetArg(int32_t, y0)
    m3ApiGetArg(int32_t, x1)
    m3ApiGetArg(int32_t, y1)
    HAL::displayProxy().drawLine(x0, y0, x1, y1);
    m3ApiSuccess()
}

static m3ApiRawFunction(cfDisplayDrawRect) {
    m3ApiGetArg(int32_t, x)
    m3ApiGetArg(int32_t, y)
    m3ApiGetArg(int32_t, w)
    m3ApiGetArg(int32_t, h)
    HAL::displayProxy().drawRect(x, y, w, h);
    m3ApiSuccess()
}

static m3ApiRawFunction(cfDisplayFillRect) {
    m3ApiGetArg(int32_t, x)
    m3ApiGetArg(int32_t, y)
    m3ApiGetArg(int32_t, w)
    m3ApiGetArg(int32_t, h)
    HAL::displayProxy().fillRect(x, y, w, h);
    m3ApiSuccess()
}

static m3ApiRawFunction(cfDisplayDrawCircle) {
    m3ApiGetArg(int32_t, x)
    m3ApiGetArg(int32_t, y)
    m3ApiGetArg(int32_t, r)
    HAL::displayProxy().drawCircle(x, y, r);
    m3ApiSuccess()
}

static m3ApiRawFunction(cfDisplayFillCircle) {
    m3ApiGetArg(int32_t, x)
    m3ApiGetArg(int32_t, y)
    m3ApiGetArg(int32_t, r)
    HAL::displayProxy().fillCircle(x, y, r);
    m3ApiSuccess()
}

static m3ApiRawFunction(cfDisplayDrawHLine) {
    m3ApiGetArg(int32_t, x)
    m3ApiGetArg(int32_t, y)
    m3ApiGetArg(int32_t, len)
    HAL::displayProxy().drawHorizontalLine(x, y, len);
    m3ApiSuccess()
}

static m3ApiRawFunction(cfDisplayDrawVLine) {
    m3ApiGetArg(int32_t, x)
    m3ApiGetArg(int32_t, y)
    m3ApiGetArg(int32_t, len)
    HAL::displayProxy().drawVerticalLine(x, y, len);
    m3ApiSuccess()
}

static m3ApiRawFunction(cfDisplayDrawTriangle) {
    m3ApiGetArg(int32_t, x0)
    m3ApiGetArg(int32_t, y0)
    m3ApiGetArg(int32_t, x1)
    m3ApiGetArg(int32_t, y1)
    m3ApiGetArg(int32_t, x2)
    m3ApiGetArg(int32_t, y2)
    HAL::displayProxy().drawTriangle(x0, y0, x1, y1, x2, y2);
    m3ApiSuccess()
}

static m3ApiRawFunction(cfDisplaySetAlign) {
    m3ApiGetArg(int32_t, align)
    OLEDDISPLAY_TEXT_ALIGNMENT a = TEXT_ALIGN_LEFT;
    if (align == 1) a = TEXT_ALIGN_RIGHT;
    else if (align == 2) a = TEXT_ALIGN_CENTER;
    else if (align == 3) a = TEXT_ALIGN_CENTER_BOTH;
    HAL::displayProxy().setTextAlignment(a);
    m3ApiSuccess()
}

static m3ApiRawFunction(cfDisplaySetFont) {
    m3ApiGetArg(int32_t, fontId)
    const uint8_t* font = ArialMT_Plain_10;
    if (fontId == 16) font = ArialMT_Plain_16;
    else if (fontId == 24) font = ArialMT_Plain_24;
    HAL::displayProxy().setFont(font);
    m3ApiSuccess()
}

// Bounded copy of a guest string into a host stack buffer.
static void copyGuestString(char* dst, size_t dstCap, const char* src, int32_t len) {
    size_t n = (len > 0) ? (size_t)len : 0;
    if (n >= dstCap) n = dstCap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static m3ApiRawFunction(cfDisplayDrawString) {
    m3ApiGetArg(int32_t, x)
    m3ApiGetArg(int32_t, y)
    m3ApiGetArgMem(const char*, str)
    m3ApiGetArg(int32_t, len)
    m3ApiCheckMem(str, (uint32_t)len)
    char buf[96];
    copyGuestString(buf, sizeof(buf), str, len);
    HAL::displayProxy().drawString(x, y, buf);
    m3ApiSuccess()
}

// XBM blit: the host DisplayProxy copies the bits into its framebuffer during
// the call (ThingPulse drawXbm reads data synchronously, keeps no pointer), so
// the guest memory is safe to use in place. Bits length = ((w+7)/8)*h.
static m3ApiRawFunction(cfDisplayDrawXbm) {
    m3ApiGetArg(int32_t, x)
    m3ApiGetArg(int32_t, y)
    m3ApiGetArg(int32_t, w)
    m3ApiGetArg(int32_t, h)
    m3ApiGetArgMem(const unsigned char*, bits)
    if (w > 0 && h > 0 && w <= 128 && h <= 64) {
        uint32_t byteLen = (uint32_t)(((w + 7) / 8) * h);
        m3ApiCheckMem(bits, byteLen)
        HAL::displayProxy().drawXbm(x, y, w, h, bits);
    }
    m3ApiSuccess()
}

static m3ApiRawFunction(cfDisplayStringWidth) {
    m3ApiReturnType(int32_t)
    m3ApiGetArgMem(const char*, str)
    m3ApiGetArg(int32_t, len)
    m3ApiCheckMem(str, (uint32_t)len)
    char buf[96];
    copyGuestString(buf, sizeof(buf), str, len);
    m3ApiReturn((int32_t)HAL::displayProxy().getStringWidth(buf, strlen(buf)))
}

// ---------------------------------------------------------------------------
// cf.* — LEDs (pixel map 0=Back 1=FrontTop 2=FrontMiddle 3=FrontBottom, which
// matches the pixel_* constants in lib/HAL/HAL.cpp)
// ---------------------------------------------------------------------------

static m3ApiRawFunction(cfLedSet) {
    m3ApiGetArg(int32_t, index)
    m3ApiGetArg(int32_t, r)
    m3ApiGetArg(int32_t, g)
    m3ApiGetArg(int32_t, b)
    m3ApiGetArg(int32_t, w)
    if (index >= 0 && index <= 3) {
        HAL::setRgbLed(index, (uint8_t)r, (uint8_t)g, (uint8_t)b, (uint8_t)w);
    }
    m3ApiSuccess()
}

static m3ApiRawFunction(cfLedAllOff) {
    HAL::setRgbLedsOff();
    m3ApiSuccess()
}

// ---------------------------------------------------------------------------
// cf.* — audio
// ---------------------------------------------------------------------------

static m3ApiRawFunction(cfTonePlay) {
    m3ApiGetArg(float, freq)
    m3ApiGetArg(int32_t, durationMs)
    HAL::audioManager().playTone(freq, durationMs);
    m3ApiSuccess()
}

static m3ApiRawFunction(cfToneStop) {
    HAL::audioManager().stopTone();
    m3ApiSuccess()
}

// Guest ToneStep layout mirrors AudioManager::ToneStep (f32 + u16 + u16).
// The host AudioManager keeps the pointer for the duration of playback and
// guest linear memory can move on memory.grow, so copy into a host buffer.
static AudioManager::ToneStep s_seqBuf[64];

static m3ApiRawFunction(cfSeqPlay) {
    m3ApiGetArgMem(const uint8_t*, steps)
    m3ApiGetArg(int32_t, count)
    if (count < 0) count = 0;
    if (count > (int32_t)(sizeof(s_seqBuf) / sizeof(s_seqBuf[0]))) {
        count = sizeof(s_seqBuf) / sizeof(s_seqBuf[0]);
    }
    m3ApiCheckMem(steps, (uint32_t)count * sizeof(AudioManager::ToneStep))
    memcpy(s_seqBuf, steps, (size_t)count * sizeof(AudioManager::ToneStep));
    HAL::audioManager().playSequence(s_seqBuf, count);
    m3ApiSuccess()
}

static m3ApiRawFunction(cfSeqStop) {
    HAL::audioManager().stopSequence();
    m3ApiSuccess()
}

// ---------------------------------------------------------------------------
// cf.* — sensors / time / system
// ---------------------------------------------------------------------------

static m3ApiRawFunction(cfMillis) {
    m3ApiReturnType(uint32_t)
    m3ApiReturn((uint32_t)millis())
}

static m3ApiRawFunction(cfRandom) {
    m3ApiReturnType(int32_t)
    m3ApiGetArg(int32_t, minInclusive)
    m3ApiGetArg(int32_t, maxExclusive)
    m3ApiReturn((int32_t)random(minInclusive, maxExclusive))
}

static m3ApiRawFunction(cfSliderPct) {
    m3ApiReturnType(float)
    m3ApiReturn(sliderPosition_Percentage_Filtered)
}

static m3ApiRawFunction(cfAccelX) {
    m3ApiReturnType(float)
    m3ApiReturn(accelX)
}

static m3ApiRawFunction(cfAccelY) {
    m3ApiReturnType(float)
    m3ApiReturn(accelY)
}

static m3ApiRawFunction(cfAccelZ) {
    m3ApiReturnType(float)
    m3ApiReturn(accelZ)
}

static m3ApiRawFunction(cfExitToMenu) {
    s_exitRequested = true;
    m3ApiSuccess()
}

static m3ApiRawFunction(cfKeepalive) {
    millis_APP_LASTINTERACTION = millis_NOW;
    m3ApiSuccess()
}

static m3ApiRawFunction(cfLog) {
    m3ApiGetArgMem(const char*, msg)
    m3ApiGetArg(int32_t, len)
    m3ApiCheckMem(msg, (uint32_t)len)
    char buf[128];
    copyGuestString(buf, sizeof(buf), msg, len);
    Serial.printf("[evt] wasm.log=%s\n", buf);
    m3ApiSuccess()
}

// ---------------------------------------------------------------------------
// WASI / Emscripten stubs — Emscripten standalone modules may import these.
// Filesystem calls fail with WASI errno 8 (EBADF); the rest do the minimum.
// ---------------------------------------------------------------------------

static m3ApiRawFunction(wasiProcExit) {
    m3ApiGetArg(int32_t, code)
    (void)code;
    m3ApiTrap(m3Err_trapExit)
}

static m3ApiRawFunction(wasiFdWrite) {
    m3ApiReturnType(int32_t)
    m3ApiGetArg(int32_t, fd)
    m3ApiGetArg(int32_t, iovs)
    m3ApiGetArg(int32_t, iovsLen)
    m3ApiGetArg(int32_t, nwrittenPtr)
    (void)fd; (void)iovs; (void)iovsLen; (void)nwrittenPtr;
    m3ApiReturn(8)  // EBADF
}

static m3ApiRawFunction(wasiFdClose) {
    m3ApiReturnType(int32_t)
    m3ApiGetArg(int32_t, fd)
    (void)fd;
    m3ApiReturn(8)
}

static m3ApiRawFunction(wasiFdSeek) {
    m3ApiReturnType(int32_t)
    m3ApiGetArg(int32_t, fd)
    m3ApiGetArg(int64_t, offset)
    m3ApiGetArg(int32_t, whence)
    m3ApiGetArg(int32_t, newOffsetPtr)
    (void)fd; (void)offset; (void)whence; (void)newOffsetPtr;
    m3ApiReturn(8)
}

static m3ApiRawFunction(wasiEnvironSizesGet) {
    m3ApiReturnType(int32_t)
    m3ApiGetArgMem(uint32_t*, countPtr)
    m3ApiGetArgMem(uint32_t*, bufSizePtr)
    m3ApiCheckMem(countPtr, sizeof(uint32_t))
    m3ApiCheckMem(bufSizePtr, sizeof(uint32_t))
    *countPtr = 0;
    *bufSizePtr = 0;
    m3ApiReturn(0)
}

static m3ApiRawFunction(wasiEnvironGet) {
    m3ApiReturnType(int32_t)
    m3ApiGetArg(int32_t, environPtr)
    m3ApiGetArg(int32_t, bufPtr)
    (void)environPtr; (void)bufPtr;
    m3ApiReturn(0)
}

static m3ApiRawFunction(wasiClockTimeGet) {
    m3ApiReturnType(int32_t)
    m3ApiGetArg(int32_t, clockId)
    m3ApiGetArg(int64_t, precision)
    m3ApiGetArgMem(uint64_t*, timePtr)
    (void)clockId; (void)precision;
    m3ApiCheckMem(timePtr, sizeof(uint64_t))
    *timePtr = (uint64_t)millis() * 1000000ull;
    m3ApiReturn(0)
}

static m3ApiRawFunction(wasiRandomGet) {
    m3ApiReturnType(int32_t)
    m3ApiGetArgMem(uint8_t*, buf)
    m3ApiGetArg(int32_t, bufLen)
    m3ApiCheckMem(buf, (uint32_t)bufLen)
    esp_fill_random(buf, (size_t)bufLen);
    m3ApiReturn(0)
}

static m3ApiRawFunction(envNotifyMemoryGrowth) {
    m3ApiGetArg(int32_t, memIndex)
    (void)memIndex;
    m3ApiSuccess()
}

// ---------------------------------------------------------------------------
// The table
// ---------------------------------------------------------------------------

static const WasmHostImport kImports[] = {
    // Adding a row here keeps CF_HAL_ABI; removing/renumbering a row is a MAJOR ABI bump (REQ-063).
    // benchmark
    { "cf", "nop",                  "i(i)",     &cfNop },
    // display
    { "cf", "display_clear",        "v()",      &cfDisplayClear },
    { "cf", "display_show",         "v()",      &cfDisplayShow },
    { "cf", "display_set_color",    "v(i)",     &cfDisplaySetColor },
    { "cf", "display_set_pixel",    "v(ii)",    &cfDisplaySetPixel },
    { "cf", "display_draw_line",    "v(iiii)",  &cfDisplayDrawLine },
    { "cf", "display_draw_rect",    "v(iiii)",  &cfDisplayDrawRect },
    { "cf", "display_fill_rect",    "v(iiii)",  &cfDisplayFillRect },
    { "cf", "display_draw_circle",  "v(iii)",   &cfDisplayDrawCircle },
    { "cf", "display_fill_circle",  "v(iii)",   &cfDisplayFillCircle },
    { "cf", "display_draw_hline",   "v(iii)",   &cfDisplayDrawHLine },
    { "cf", "display_draw_vline",   "v(iii)",   &cfDisplayDrawVLine },
    { "cf", "display_draw_triangle","v(iiiiii)",&cfDisplayDrawTriangle },
    { "cf", "display_set_align",    "v(i)",     &cfDisplaySetAlign },
    { "cf", "display_set_font",     "v(i)",     &cfDisplaySetFont },
    { "cf", "display_draw_string",  "v(iiii)",  &cfDisplayDrawString },
    { "cf", "display_string_width", "i(ii)",    &cfDisplayStringWidth },
    { "cf", "display_draw_xbm",     "v(iiiii)", &cfDisplayDrawXbm },
    // LEDs
    { "cf", "led_set",              "v(iiiii)", &cfLedSet },
    { "cf", "led_all_off",          "v()",      &cfLedAllOff },
    // audio
    { "cf", "tone_play",            "v(fi)",    &cfTonePlay },
    { "cf", "tone_stop",            "v()",      &cfToneStop },
    { "cf", "seq_play",             "v(ii)",    &cfSeqPlay },
    { "cf", "seq_stop",             "v()",      &cfSeqStop },
    // sensors / time / system
    { "cf", "millis",               "i()",      &cfMillis },
    { "cf", "random",               "i(ii)",    &cfRandom },
    { "cf", "slider_pct",           "f()",      &cfSliderPct },
    { "cf", "accel_x",              "f()",      &cfAccelX },
    { "cf", "accel_y",              "f()",      &cfAccelY },
    { "cf", "accel_z",              "f()",      &cfAccelZ },
    { "cf", "exit_to_menu",         "v()",      &cfExitToMenu },
    { "cf", "keepalive",            "v()",      &cfKeepalive },
    { "cf", "log",                  "v(ii)",    &cfLog },
    // WASI / Emscripten stubs
    { "wasi_snapshot_preview1", "proc_exit",         "v(i)",    &wasiProcExit },
    { "wasi_snapshot_preview1", "fd_write",          "i(iiii)", &wasiFdWrite },
    { "wasi_snapshot_preview1", "fd_close",          "i(i)",    &wasiFdClose },
    { "wasi_snapshot_preview1", "fd_seek",           "i(iIii)", &wasiFdSeek },
    { "wasi_snapshot_preview1", "environ_sizes_get", "i(ii)",   &wasiEnvironSizesGet },
    { "wasi_snapshot_preview1", "environ_get",       "i(ii)",   &wasiEnvironGet },
    { "wasi_snapshot_preview1", "clock_time_get",    "i(iIi)",  &wasiClockTimeGet },
    { "wasi_snapshot_preview1", "random_get",        "i(ii)",   &wasiRandomGet },
    { "env", "emscripten_notify_memory_growth",      "v(i)",    &envNotifyMemoryGrowth },
};

const WasmHostImport* wasmHostImportTable(int* outCount) {
    *outCount = (int)(sizeof(kImports) / sizeof(kImports[0]));
    return kImports;
}
