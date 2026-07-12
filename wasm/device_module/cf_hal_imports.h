// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC
//
// The "cf" wasm import surface — every host call an app module can make.
// Device side: lib/WasmAppRuntime/WasmHostImports.cpp implements these and
// forwards to the real HAL. Guest side: the shims in
// wasm/device_module/shims/ forward the firmware HAL API onto these.
//
// Keep docs/SPIKE_WASM_IMPORT_SURFACE.md in sync when adding entries.

#ifndef CF_HAL_IMPORTS_H
#define CF_HAL_IMPORTS_H

#include <stdint.h>

#define CF_IMPORT(NAME) \
    __attribute__((import_module("cf"), import_name(NAME)))

#ifdef __cplusplus
extern "C" {
#endif

// --- Display (SSD1306 128x64 via DisplayProxy) ---
CF_IMPORT("display_clear")        void    cf_display_clear(void);
CF_IMPORT("display_show")         void    cf_display_show(void);
CF_IMPORT("display_set_color")    void    cf_display_set_color(int32_t color);      // 0=BLACK 1=WHITE 2=INVERSE
CF_IMPORT("display_set_pixel")    void    cf_display_set_pixel(int32_t x, int32_t y);
CF_IMPORT("display_draw_line")    void    cf_display_draw_line(int32_t x0, int32_t y0, int32_t x1, int32_t y1);
CF_IMPORT("display_draw_rect")    void    cf_display_draw_rect(int32_t x, int32_t y, int32_t w, int32_t h);
CF_IMPORT("display_fill_rect")    void    cf_display_fill_rect(int32_t x, int32_t y, int32_t w, int32_t h);
CF_IMPORT("display_draw_circle")  void    cf_display_draw_circle(int32_t x, int32_t y, int32_t r);
CF_IMPORT("display_fill_circle")  void    cf_display_fill_circle(int32_t x, int32_t y, int32_t r);
CF_IMPORT("display_draw_hline")   void    cf_display_draw_hline(int32_t x, int32_t y, int32_t len);
CF_IMPORT("display_draw_vline")   void    cf_display_draw_vline(int32_t x, int32_t y, int32_t len);
CF_IMPORT("display_draw_triangle") void   cf_display_draw_triangle(int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t x2, int32_t y2);
CF_IMPORT("display_set_align")    void    cf_display_set_align(int32_t align);      // 0=L 1=R 2=C 3=CB
CF_IMPORT("display_set_font")     void    cf_display_set_font(int32_t fontId);      // 10/16/24 = ArialMT sizes
CF_IMPORT("display_draw_string")  void    cf_display_draw_string(int32_t x, int32_t y, const char* str, int32_t len);
CF_IMPORT("display_string_width") int32_t cf_display_string_width(const char* str, int32_t len);
// XBM bitmap blit; bits length = ((w+7)/8)*h bytes. The host blits into its
// framebuffer during the call (no pointer retained), so guest memory is read
// in place — safe even with memory.grow.
CF_IMPORT("display_draw_xbm")     void    cf_display_draw_xbm(int32_t x, int32_t y, int32_t w, int32_t h, const void* bits);

// --- RGBW LEDs (pixel index map: 0=Back 1=FrontTop 2=FrontMiddle 3=FrontBottom) ---
CF_IMPORT("led_set")              void    cf_led_set(int32_t index, int32_t r, int32_t g, int32_t b, int32_t w);
CF_IMPORT("led_all_off")          void    cf_led_all_off(void);

// --- Audio ---
CF_IMPORT("tone_play")            void    cf_tone_play(float frequency, int32_t durationMs);  // 0 = indefinite
CF_IMPORT("tone_stop")            void    cf_tone_stop(void);
// steps: packed {f32 freq; u16 durationMs; u16 gapAfterMs;} — host copies out
// of guest memory before use (max 64 steps).
CF_IMPORT("seq_play")             void    cf_seq_play(const void* steps, int32_t count);
CF_IMPORT("seq_stop")             void    cf_seq_stop(void);

// --- Sensors / time ---
CF_IMPORT("millis")               uint32_t cf_millis(void);
CF_IMPORT("random")               int32_t  cf_random(int32_t minInclusive, int32_t maxExclusive);
CF_IMPORT("slider_pct")           float    cf_slider_pct(void);   // filtered 0..100
CF_IMPORT("accel_x")              float    cf_accel_x(void);
CF_IMPORT("accel_y")              float    cf_accel_y(void);
CF_IMPORT("accel_z")              float    cf_accel_z(void);

// --- System ---
// Deferred on the host: the shell exits to menu after the current guest call
// returns (unloading mid-call would free the interpreter under our feet).
CF_IMPORT("exit_to_menu")         void    cf_exit_to_menu(void);
CF_IMPORT("log")                  void    cf_log(const char* msg, int32_t len);
// Resets the host's last-interaction timer (deep-sleep inhibit). The glue
// calls this when the app bumps millis_APP_LASTINTERACTION.
CF_IMPORT("keepalive")            void    cf_keepalive(void);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // CF_HAL_IMPORTS_H
