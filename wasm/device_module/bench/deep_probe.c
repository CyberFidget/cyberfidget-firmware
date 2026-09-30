// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2026 Dismo Industries LLC
//
// Guest-stack probe app (runs ON DEVICE inside wasm3). A bench fixture, not
// a shipped app: it deliberately recurses deeper every frame until the
// interpreter's native-stack guard stops it, so the bench can check that
//   - running out of room ends the app with a plain message (no hang, no
//     reboot), and
//   - the guard's reserve covers what can run below the deepest guest
//     frame: a host call (text drawing + a serial log line) and wasm3's
//     lazy compile of a fresh, deeply nested function.
//
// Every frame the call chain goes PROBE_STEP levels deeper. At the bottom it
// draws text, logs "probe depth=<n>" and calls one never-before-called
// 64-case switch function (lazily compiled right there, at full depth).
//
// Build (freestanding clang from emsdk; see build_device_modules.bat):
//   clang --target=wasm32 -O2 -nostdlib -Wl,--no-entry -Wl,--strip-all \
//         -Wl,--export=app_begin -Wl,--export=app_update -Wl,--export=app_end \
//         -Wl,--export=app_handle_button -o deep_probe.wasm deep_probe.c

#include <stdint.h>

#ifndef PROBE_STEP
#define PROBE_STEP 16
#endif

#define CF_IMPORT(NAME) __attribute__((import_module("cf"), import_name(NAME)))
CF_IMPORT("display_clear")       void cf_display_clear(void);
CF_IMPORT("display_show")        void cf_display_show(void);
CF_IMPORT("display_set_font")    void cf_display_set_font(int32_t fontId);
CF_IMPORT("display_draw_string") void cf_display_draw_string(int32_t x, int32_t y, const char* s, int32_t len);
CF_IMPORT("log")                 void cf_log(const char* msg, int32_t len);
CF_IMPORT("exit_to_menu")        void cf_exit_to_menu(void);

static volatile int32_t g_sink;
static int32_t g_target;
static int32_t g_frame;

static int fmt_int(char* out, int32_t v) {
    char tmp[12];
    int n = 0, len = 0;
    if (v == 0) tmp[n++] = '0';
    while (v > 0) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
    while (n > 0) out[len++] = tmp[--n];
    return len;
}

// 64 distinct case bodies -> a br_table over 64 nested wasm blocks, the
// shape that makes wasm3's compiler recurse once per nesting level.
#define C(i) case i: g_sink ^= (x << ((i) & 7)) + (i) * 131; break;
#define C8(b) C(b+0) C(b+1) C(b+2) C(b+3) C(b+4) C(b+5) C(b+6) C(b+7)
#define SWITCH_FN(n)                                                        \
    __attribute__((noinline)) static void sw##n(int32_t x) {                \
        switch ((x + (n)) & 63) {                                           \
            C8(0) C8(8) C8(16) C8(24) C8(32) C8(40) C8(48) C8(56)           \
        }                                                                   \
    }
#define SW8(b) SWITCH_FN(b##0) SWITCH_FN(b##1) SWITCH_FN(b##2) SWITCH_FN(b##3) \
               SWITCH_FN(b##4) SWITCH_FN(b##5) SWITCH_FN(b##6) SWITCH_FN(b##7)
SW8(1) SW8(2) SW8(3) SW8(4) SW8(5) SW8(6) SW8(7) SW8(8)

typedef void (*sw_fn)(int32_t);
#define R8(b) sw##b##0, sw##b##1, sw##b##2, sw##b##3, sw##b##4, sw##b##5, sw##b##6, sw##b##7
static sw_fn const kSwitches[64] = { R8(1), R8(2), R8(3), R8(4), R8(5), R8(6), R8(7), R8(8) };

static void bottom(int32_t depth) {
    char line[24] = "probe depth=";
    int len = 12 + fmt_int(line + 12, depth);
    cf_display_set_font(16);
    cf_display_draw_string(0, 24, line, len);
    cf_log(line, len);
    kSwitches[g_frame & 63](depth);
}

// Not a tail call (the sum after the call), noinline, and a little local
// state, so each level is one real wasm call frame.
__attribute__((noinline)) static int32_t dive(int32_t n, int32_t depth) {
    volatile int32_t pad[2];
    pad[0] = n;
    pad[1] = depth;
    if (n <= 0) {
        bottom(depth);
        return pad[1];
    }
    return dive(n - 1, depth) + (pad[0] & 1);
}

__attribute__((export_name("app_begin"))) void app_begin(void) {
    g_target = 0;
    g_frame = 0;
}

__attribute__((export_name("app_update"))) void app_update(void) {
    cf_display_clear();
    g_target += PROBE_STEP;
    dive(g_target, g_target);
    cf_display_show();
    g_frame++;
}

__attribute__((export_name("app_end"))) void app_end(void) {}

__attribute__((export_name("app_handle_button"))) void app_handle_button(int32_t button, int32_t event) {
    // Back (index 4) released leaves.
    if (button == 4 && event == 2) cf_exit_to_menu();
}
