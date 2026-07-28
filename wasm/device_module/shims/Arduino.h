// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC
//
// Guest-side Arduino shim for device wasm modules (spike/wasm3-apps).
// Adapted from wasm/shims/Arduino.h, but forwards to "cf" wasm imports
// instead of the browser runtime. Fixed-buffer String keeps modules free of
// heap churn (all firmware display strings are short).

#ifndef CF_DM_ARDUINO_H
#define CF_DM_ARDUINO_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

#include "cf_hal_imports.h"

// ---- Timing ----
inline unsigned long millis() { return (unsigned long)cf_millis(); }
inline unsigned long micros() { return (unsigned long)cf_millis() * 1000ul; }
inline void delay(unsigned long) {}           // never block inside a guest tick
inline void delayMicroseconds(unsigned int) {}

// ---- Random ----
inline long random(long maxExclusive) { return (long)cf_random(0, (int32_t)maxExclusive); }
inline long random(long minInclusive, long maxExclusive) {
    return (long)cf_random((int32_t)minInclusive, (int32_t)maxExclusive);
}
inline void randomSeed(unsigned long) {}

// ---- Math helpers ----
inline long map(long x, long in_min, long in_max, long out_min, long out_max) {
    if (in_max == in_min) return out_min;
    return (x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
}

template <typename T> inline T constrain(T x, T lo, T hi) {
    return (x < lo) ? lo : ((x > hi) ? hi : x);
}
template <typename T> inline T min(T a, T b) { return (a < b) ? a : b; }
template <typename T> inline T max(T a, T b) { return (a > b) ? a : b; }
// abs() comes from the sysroot stdlib.

#define PROGMEM
#define pgm_read_byte(addr) (*(const uint8_t*)(addr))
#define pgm_read_word(addr) (*(const uint16_t*)(addr))

#ifndef PI
#define PI 3.14159265358979323846
#endif

inline float radians(float deg) { return deg * PI / 180.0f; }
inline float degrees(float rad) { return rad * 180.0f / PI; }

// ---- Minimal fixed-buffer Arduino String ----
// Truncates silently at kCap-1 chars; plenty for a 128px-wide display line.
class String {
public:
    static constexpr size_t kCap = 64;

    String() { buf[0] = '\0'; }
    String(const char* s) { set(s); }
    String(char c) { buf[0] = c; buf[1] = '\0'; }
    String(int v) { fromLong((long)v); }
    String(unsigned int v) { fromULong((unsigned long)v); }
    String(long v) { fromLong(v); }
    String(unsigned long v) { fromULong(v); }
    String(float v, int decimals = 2) { fromFloat(v, decimals); }
    String(double v, int decimals = 2) { fromFloat((float)v, decimals); }

    const char* c_str() const { return buf; }
    unsigned int length() const { return (unsigned int)strlen(buf); }

    String& operator+=(const String& o) { append(o.buf); return *this; }
    String& operator+=(const char* s) { append(s); return *this; }
    String& operator+=(char c) { char t[2] = {c, '\0'}; append(t); return *this; }
    String& operator+=(int v) { append(String(v).buf); return *this; }
    String& operator+=(unsigned int v) { append(String(v).buf); return *this; }
    String& operator+=(long v) { append(String(v).buf); return *this; }
    String& operator+=(unsigned long v) { append(String(v).buf); return *this; }
    String& operator+=(float v) { append(String(v).buf); return *this; }

    String operator+(const String& o) const { String r(*this); r += o; return r; }
    String operator+(const char* s) const { String r(*this); r += s; return r; }

private:
    void set(const char* s) {
        if (!s) { buf[0] = '\0'; return; }
        size_t n = strlen(s);
        if (n >= kCap) n = kCap - 1;
        memcpy(buf, s, n);
        buf[n] = '\0';
    }
    void append(const char* s) {
        if (!s) return;
        size_t used = strlen(buf);
        size_t n = strlen(s);
        if (used + n >= kCap) n = kCap - 1 - used;
        memcpy(buf + used, s, n);
        buf[used + n] = '\0';
    }
    void fromULong(unsigned long v) {
        char tmp[12];
        int i = 0;
        do { tmp[i++] = (char)('0' + (v % 10)); v /= 10; } while (v && i < 11);
        int o = 0;
        while (i > 0) buf[o++] = tmp[--i];
        buf[o] = '\0';
    }
    void fromLong(long v) {
        if (v < 0) { buf[0] = '-'; buf[1] = '\0'; String t; t.fromULong((unsigned long)(-v)); append(t.buf); }
        else fromULong((unsigned long)v);
    }
    void fromFloat(float v, int decimals) {
        if (decimals < 0) decimals = 0;
        if (decimals > 4) decimals = 4;
        if (isnan(v)) { set("nan"); return; }
        if (isinf(v)) { set(v < 0 ? "-inf" : "inf"); return; }
        bool neg = v < 0.0f;
        if (neg) v = -v;
        float scale = 1.0f;
        for (int i = 0; i < decimals; i++) scale *= 10.0f;
        unsigned long scaled = (unsigned long)(v * scale + 0.5f);
        unsigned long ip = scaled;
        unsigned long fp = 0;
        if (decimals > 0) {
            ip = scaled / (unsigned long)scale;
            fp = scaled % (unsigned long)scale;
        }
        buf[0] = '\0';
        if (neg) append("-");
        String t; t.fromULong(ip); append(t.buf);
        if (decimals > 0) {
            append(".");
            // zero-pad the fractional part
            unsigned long place = (unsigned long)scale / 10;
            while (place > 0 && fp < place && place > 1) { append("0"); place /= 10; }
            String f; f.fromULong(fp); append(f.buf);
        }
    }

    char buf[kCap];
};

inline String operator+(const char* a, const String& b) { String r(a); r += b; return r; }

#endif  // CF_DM_ARDUINO_H
