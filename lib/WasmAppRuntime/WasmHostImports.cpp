// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC
// Both adapters call these behaviors; only the device adapter depends on wasm3.
#include "WasmHostFunctions.h"
#ifndef CF_WASM_MODULE_HOST
#include "WasmHostImports.h"
#include <esp_random.h>
#endif
#include <string.h>
#include <Arduino.h>
#include "AudioManager.h"
#include "DisplayProxy.h"
#include "HAL.h"
#include "globals.h"

static volatile bool s_exitRequested = false;
static bool s_appMicEnabled = false;
void wasmHostEndAudio() {
    HAL::audioManager().stopSequence();
    HAL::audioManager().stopTone();
    HAL::audioManager().stopNotes();
    if (s_appMicEnabled) HAL::audioManager().enableMic(false);
    s_appMicEnabled = false;
}
bool wasmHostConsumeExitRequest() {
    bool requested = s_exitRequested;
    s_exitRequested = false;
    return requested;
}
void wasmHostClearExitRequest() { s_exitRequested = false; }

static void copyGuestString(char* dst, size_t dstCap, const char* src, int32_t len) {
    size_t n = (len > 0) ? (size_t)len : 0;
    if (n >= dstCap) n = dstCap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}
// AudioManager retains this host copy, never a guest linear-memory pointer.
static AudioManager::ToneStep s_seqBuf[64];
static_assert(sizeof(AudioManager::ToneStep) == 8, "Guest tone-step layout must match");

namespace cf_host {
int32_t nop(int32_t x) {
    return x + 1;
}

void display_clear() {
    HAL::displayProxy().clear();
}

void display_show() {
    HAL::displayProxy().display();
}

void display_set_color(int32_t color) {
    OLEDDISPLAY_COLOR c = WHITE;
    if (color == 0) c = BLACK;
    else if (color == 2) c = INVERSE;
    HAL::displayProxy().setColor(c);
}

void display_set_pixel(int32_t x, int32_t y) {
    HAL::displayProxy().setPixel(x, y);
}

void display_draw_line(int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
    HAL::displayProxy().drawLine(x0, y0, x1, y1);
}

void display_draw_rect(int32_t x, int32_t y, int32_t w, int32_t h) {
    HAL::displayProxy().drawRect(x, y, w, h);
}

void display_fill_rect(int32_t x, int32_t y, int32_t w, int32_t h) {
    HAL::displayProxy().fillRect(x, y, w, h);
}

void display_draw_circle(int32_t x, int32_t y, int32_t r) {
    HAL::displayProxy().drawCircle(x, y, r);
}

void display_fill_circle(int32_t x, int32_t y, int32_t r) {
    HAL::displayProxy().fillCircle(x, y, r);
}

void display_draw_hline(int32_t x, int32_t y, int32_t len) {
    HAL::displayProxy().drawHorizontalLine(x, y, len);
}

void display_draw_vline(int32_t x, int32_t y, int32_t len) {
    HAL::displayProxy().drawVerticalLine(x, y, len);
}

void display_draw_triangle(int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t x2, int32_t y2) {
    HAL::displayProxy().drawTriangle(x0, y0, x1, y1, x2, y2);
}

void display_set_align(int32_t align) {
    OLEDDISPLAY_TEXT_ALIGNMENT a = TEXT_ALIGN_LEFT;
    if (align == 1) a = TEXT_ALIGN_RIGHT;
    else if (align == 2) a = TEXT_ALIGN_CENTER;
    else if (align == 3) a = TEXT_ALIGN_CENTER_BOTH;
    HAL::displayProxy().setTextAlignment(a);
}

void display_set_font(int32_t fontId) {
    const uint8_t* font = ArialMT_Plain_10;
    if (fontId == 16) font = ArialMT_Plain_16;
    else if (fontId == 24) font = ArialMT_Plain_24;
    HAL::displayProxy().setFont(font);
}

void display_draw_string(int32_t x, int32_t y, const char* str, int32_t len) {
    char buf[96];
    copyGuestString(buf, sizeof(buf), str, len);
    HAL::displayProxy().drawString(x, y, buf);
}

int32_t display_string_width(const char* str, int32_t len) {
    char buf[96];
    copyGuestString(buf, sizeof(buf), str, len);
    return (int32_t)HAL::displayProxy().getStringWidth(buf, strlen(buf));
}

void display_draw_xbm(int32_t x, int32_t y, int32_t w, int32_t h, const void* bits, int32_t byteLen) {
    if (xbmByteLength(w, h) && byteLen >= (int32_t)xbmByteLength(w, h)) {
        HAL::displayProxy().drawXbm(x, y, w, h, static_cast<const unsigned char*>(bits));
    }
}

void led_set(int32_t index, int32_t r, int32_t g, int32_t b, int32_t w) {
    if (index >= 0 && index <= 3) {
        HAL::setRgbLed(index, (uint8_t)r, (uint8_t)g, (uint8_t)b, (uint8_t)w);
    }
}

void led_all_off() {
    HAL::setRgbLedsOff();
}

void tone_play(float frequency, int32_t durationMs) {
    HAL::audioManager().playTone(frequency, durationMs);
}

void tone_stop() {
    HAL::audioManager().stopTone();
}

int32_t note_play(float frequency, int32_t durationMs) {
    return HAL::audioManager().playNote(frequency, durationMs);
}
void note_stop(int32_t handle) { HAL::audioManager().stopNote(handle); }
void note_all_off() { HAL::audioManager().stopNotes(); }
void mic_enable(int32_t on) {
    HAL::audioManager().enableMic(on != 0);
    s_appMicEnabled = on != 0;
}
float mic_level() { return HAL::audioManager().getMicVolumeLinear(); }
float mic_level_db() { return HAL::audioManager().getMicVolumeDb(); }

void seq_play(const void* steps, int32_t count) {
    if (count < 0) count = 0;
    if (count > (int32_t)(sizeof(s_seqBuf) / sizeof(s_seqBuf[0]))) {
        count = sizeof(s_seqBuf) / sizeof(s_seqBuf[0]);
    }

    memcpy(s_seqBuf, steps, (size_t)count * sizeof(AudioManager::ToneStep));
    HAL::audioManager().playSequence(s_seqBuf, count);
}

void seq_stop() {
    HAL::audioManager().stopSequence();
}

uint32_t millis() {
    return (uint32_t)::millis();
}

int32_t random(int32_t minInclusive, int32_t maxExclusive) {
    return (int32_t)::random(minInclusive, maxExclusive);
}

float slider_pct() {
    return sliderPosition_Percentage_Filtered;
}

float accel_x() {
    return accelX;
}

float accel_y() {
    return accelY;
}

float accel_z() {
    return accelZ;
}

void exit_to_menu() {
    s_exitRequested = true;
}

void keepalive() {
    millis_APP_LASTINTERACTION = millis_NOW;
}

void log(const char* msg, int32_t len) {
    char buf[128];
    copyGuestString(buf, sizeof(buf), msg, len);
#ifdef CF_WASM_MODULE_HOST
    // The emulator Serial shim has print(), but no printf(). Keep one callback
    // with exactly the device's formatting, without changing that shim.
    char line[sizeof(buf) + sizeof("[evt] wasm.log=\n")] = "[evt] wasm.log=";
    strcat(line, buf);
    strcat(line, "\n");
    Serial.print(line);
#else
    Serial.printf("[evt] wasm.log=%s\n", buf);
#endif
}
} // namespace cf_host

#ifndef CF_WASM_MODULE_HOST
// The parameter list expands in three modes: unpack, bounds-check, and call.
#include "../../wasm/device_module/cf_params.h"
#define CF_UNPACK_A(type, name, bytes) m3ApiGetArg(type, name)
#define CF_UNPACK_B(type, name, bytes) CF_UNPACK_A(type, name, bytes)
#define CF_UNPACK_PA(type, name, bytes) m3ApiGetArgMem(type, name)
#define CF_UNPACK_PB(type, name, bytes) CF_UNPACK_PA(type, name, bytes)
#define CF_CHECK_A(type, name, bytes)
#define CF_CHECK_B(type, name, bytes)
#define CF_CHECK_PA(type, name, bytes) m3ApiCheckMem(name, bytes)
#define CF_CHECK_PB(type, name, bytes) CF_CHECK_PA(type, name, bytes)
#define CF_RET_void
#define CF_RET_int32_t m3ApiReturnType(int32_t)
#define CF_RET_uint32_t m3ApiReturnType(uint32_t)
#define CF_RET_float m3ApiReturnType(float)
#define CF_RETURN_void(expr) expr; m3ApiSuccess()
#define CF_RETURN_int32_t(expr) m3ApiReturn(expr)
#define CF_RETURN_uint32_t(expr) m3ApiReturn(expr)
#define CF_RETURN_float(expr) m3ApiReturn(expr)
#define CF_POLICY_PLAIN
#define CF_POLICY_STRING95
#define CF_POLICY_STRING127
#define CF_POLICY_SEQUENCE count = cf_host::sequenceCount(count);
#define CF_POLICY_XBM if (!cf_host::xbmByteLength(w, h)) { m3ApiSuccess() }
#define CF_EXTRA_PLAIN
#define CF_EXTRA_STRING95
#define CF_EXTRA_STRING127
#define CF_EXTRA_SEQUENCE
#define CF_EXTRA_XBM , cf_host::xbmByteLength(w, h)
#define CF_ROW(since, name, ret, sig, policy, args) \
    static m3ApiRawFunction(cfRaw_##name) { \
        CF_RET_##ret CF_PARAMS(UNPACK, args) CF_POLICY_##policy \
        CF_PARAMS(CHECK, args) \
        CF_RETURN_##ret(cf_host::name(CF_PARAMS(CALL, args) CF_EXTRA_##policy)) \
    }
#define CF_STUB(since, module, name, ret, sig, fn, policy, args)
#include "../../wasm/device_module/cf_imports.def"
#undef CF_ROW
#undef CF_STUB

static const void* wasiProcExit(int32_t code) {
    (void)code;
    return m3Err_trapExit;
}

static int32_t wasiFdWrite(int32_t fd, int32_t iovs, int32_t iovsLen, int32_t nwrittenPtr) {
    (void)fd; (void)iovs; (void)iovsLen; (void)nwrittenPtr;
    return 8;  // EBADF
}

static int32_t wasiFdClose(int32_t fd) {
    (void)fd;
    return 8;
}

static int32_t wasiFdSeek(int32_t fd, int64_t offset, int32_t whence, int32_t newOffsetPtr) {
    (void)fd; (void)offset; (void)whence; (void)newOffsetPtr;
    return 8;
}

static int32_t wasiEnvironSizesGet(uint32_t* countPtr, uint32_t* bufSizePtr) {
    *countPtr = 0;
    *bufSizePtr = 0;
    return 0;
}

static int32_t wasiEnvironGet(int32_t environPtr, int32_t bufPtr) {
    (void)environPtr; (void)bufPtr;
    return 0;
}

static int32_t wasiClockTimeGet(int32_t clockId, int64_t precision, uint64_t* timePtr) {
    (void)clockId; (void)precision;

    *timePtr = (uint64_t)millis() * 1000000ull;
    return 0;
}

static int32_t wasiRandomGet(uint8_t* buf, int32_t bufLen) {
    esp_fill_random(buf, (size_t)bufLen);
    return 0;
}

static void envNotifyMemoryGrowth(int32_t memIndex) {
    (void)memIndex;
}

#define CF_STUB_RETURN_PLAIN(ret, expr) CF_RETURN_##ret(expr)
#define CF_STUB_RETURN_EXIT_TRAP(ret, expr) m3ApiTrap(expr)
#define CF_ROW(since, name, ret, sig, policy, args)
#define CF_STUB(since, module, name, ret, sig, fn, policy, args) \
    static m3ApiRawFunction(cfStubRaw_##name) { \
        CF_RET_##ret CF_PARAMS(UNPACK, args) CF_PARAMS(CHECK, args) \
        CF_STUB_RETURN_##policy(ret, fn(CF_PARAMS(CALL, args))) \
    }
#include "../../wasm/device_module/cf_imports.def"
#undef CF_STUB
#undef CF_ROW

static const WasmHostImport kImports[] = {
#define CF_ROW(since, name, ret, sig, policy, args) { "cf", #name, sig, &cfRaw_##name },
#define CF_STUB(since, module, name, ret, sig, fn, policy, args) { module, #name, sig, &cfStubRaw_##name },
#include "../../wasm/device_module/cf_imports.def"
#undef CF_STUB
#undef CF_ROW
};
const WasmHostImport* wasmHostImportTable(int* outCount) {
    *outCount = (int)(sizeof(kImports) / sizeof(kImports[0]));
    return kImports;
}
#endif // !CF_WASM_MODULE_HOST
