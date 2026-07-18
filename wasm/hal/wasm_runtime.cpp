// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2023-2026 Dismo Industries LLC

#include "Arduino.h"
#include "Wire.h"
#include "WiFi.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

// ---- Framebuffer push to JS (single definition for the EM_JS symbol) ----
#ifdef __EMSCRIPTEN__
EM_JS(void, js_push_framebuffer, (const uint8_t* buf, int len), {
    if (Module.onFrameReady) {
        Module.onFrameReady(HEAPU8.slice(buf, buf + len));
    }
});
#else
void js_push_framebuffer(const uint8_t* buf, int len) { (void)buf; (void)len; }
#endif

// ---- Serial output to JS ----
#ifdef __EMSCRIPTEN__
EM_JS(void, js_serial_write, (const char* str, int len), {
    if (Module.onSerialOutput) {
        Module.onSerialOutput(UTF8ToString(str, len));
    }
});
#else
void js_serial_write(const char* str, int len) { (void)str; (void)len; }
#endif

// ---- Timing ----
static uint32_t s_startMs = 0;
static bool s_inited = false;

#ifdef CF_DINO_PARITY
// Parity builds are stepped synchronously by Node, so wall-clock time would
// make obstacle spawning and button debounce nondeterministic.
static uint32_t s_parityMillis = 0;

extern "C" void wasm_parity_set_millis(uint32_t value) {
    s_parityMillis = value;
}

extern "C" void wasm_parity_advance_millis(uint32_t delta) {
    s_parityMillis += delta;
}
#endif

uint32_t millis() {
#ifdef CF_DINO_PARITY
    return s_parityMillis;
#elif defined(__EMSCRIPTEN__)
    return (uint32_t)emscripten_get_now();
#else
    return 0;
#endif
}

void delay(uint32_t ms) {
#ifdef __EMSCRIPTEN__
    emscripten_sleep(ms);
#else
    (void)ms;
#endif
}

// ---- Button / analog state (set by JS) ----
uint8_t wasm_button_states[8] = {0};
int wasm_analog_values[8] = {0};

const int wasm_pin_map[8] = {36, 37, 38, 39, 34, 15, -1, -1};

// ---- Serial singleton ----
HardwareSerial Serial;

// ---- Wire singleton ----
TwoWire Wire;

// ---- WiFi singleton ----
WiFiClass WiFi;

// ColorHSV is now defined inline in shims/Adafruit_NeoPixel.h
