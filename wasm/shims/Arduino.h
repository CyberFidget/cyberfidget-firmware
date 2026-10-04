// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2023-2026 Dismo Industries LLC

#ifndef WASM_ARDUINO_H
#define WASM_ARDUINO_H

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <cstdio>
#include <algorithm>
#include <string>

// ---- Pin modes and digital I/O ----
#define INPUT         0x0
#define OUTPUT        0x1
#define INPUT_PULLUP  0x2
#define INPUT_PULLDOWN 0x3
#define LOW           0x0
#define HIGH          0x1

// Button state array set by JS via exported WASM functions
extern uint8_t wasm_button_states[8];
extern int wasm_analog_values[8];

inline void pinMode(int, int) {}

inline int digitalRead(int pin) {
    for (int i = 0; i < 8; i++) {
        extern const int wasm_pin_map[8];
        if (wasm_pin_map[i] == pin) {
            return wasm_button_states[i] ? LOW : HIGH;
        }
    }
    return HIGH;
}

inline void digitalWrite(int, int) {}

inline int analogRead(int) {
    return wasm_analog_values[0];
}

inline int analogReadMilliVolts(int) {
    return (wasm_analog_values[0] * 3300) / 4095;
}

// ---- Timing ----
uint32_t millis();
void delay(uint32_t ms);
inline void delayMicroseconds(uint32_t) {}
inline uint32_t micros() { return millis() * 1000; }

// ---- Math helpers ----
inline long map(long x, long in_min, long in_max, long out_min, long out_max) {
    if (in_max == in_min) return out_min;
    return (x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
}

inline long constrain(long x, long a, long b) {
    return (x < a) ? a : ((x > b) ? b : x);
}

using std::min;
using std::max;
using std::abs;

// ---- Random ----
inline long random(long max_val) { return std::rand() % max_val; }
inline long random(long min_val, long max_val) {
    if (max_val <= min_val) return min_val;
    return min_val + (std::rand() % (max_val - min_val));
}
inline void randomSeed(unsigned long seed) { std::srand(seed); }

// ---- Memory / PROGMEM ----
#define PROGMEM
#define pgm_read_byte(addr) (*(const uint8_t*)(addr))
#define pgm_read_word(addr) (*(const uint16_t*)(addr))
#define pgm_read_dword(addr) (*(const uint32_t*)(addr))
#define F(str) (str)

// ---- String class (Arduino-compatible subset) ----
class String {
public:
    String() : _str() {}
    String(const char* s) : _str(s ? s : "") {}
    String(const String& s) : _str(s._str) {}
    explicit String(char c) : _str(c ? std::string(1, c) : std::string()) {}
    explicit String(unsigned char val, unsigned char base = 10)
        : _str(formatInteger(val, base, false)) {}
    explicit String(int val, unsigned char base = 10)
        : _str(formatInteger(val < 0 ? static_cast<unsigned int>(0) - static_cast<unsigned int>(val) : static_cast<unsigned int>(val), base, val < 0)) {}
    explicit String(unsigned int val, unsigned char base = 10)
        : _str(formatInteger(val, base, false)) {}
    explicit String(long val, unsigned char base = 10)
        : _str(formatInteger(val < 0 ? static_cast<unsigned long>(0) - static_cast<unsigned long>(val) : static_cast<unsigned long>(val), base, val < 0)) {}
    explicit String(unsigned long val, unsigned char base = 10)
        : _str(formatInteger(val, base, false)) {}
    explicit String(long long val, unsigned char base = 10)
        : _str(formatInteger(val < 0 ? static_cast<unsigned long long>(0) - static_cast<unsigned long long>(val) : static_cast<unsigned long long>(val), base, val < 0)) {}
    explicit String(unsigned long long val, unsigned char base = 10)
        : _str(formatInteger(val, base, false)) {}
    explicit String(float val, unsigned int dec = 2) : _str(formatFloat(val, dec)) {}
    explicit String(double val, unsigned int dec = 2) : _str(formatFloat(val, dec)) {}

    const char* c_str() const { return _str.c_str(); }
    unsigned int length() const { return _str.length(); }
    bool isEmpty() const { return _str.empty(); }
    char charAt(unsigned int i) const { return (i < _str.size()) ? _str[i] : 0; }
    void toCharArray(char* buf, unsigned int len) const {
        strncpy(buf, _str.c_str(), len);
        if (len > 0) buf[len - 1] = '\0';
    }
    int toInt() const { return atoi(_str.c_str()); }
    float toFloat() const { return atof(_str.c_str()); }

    String& operator=(const String& rhs) { _str = rhs._str; return *this; }
    String& operator=(const char* rhs) { _str = rhs ? rhs : ""; return *this; }

    String operator+(const String& rhs) const { return String((_str + rhs._str).c_str()); }
    String operator+(const char* rhs) const { return String((_str + (rhs ? rhs : "")).c_str()); }
    String operator+(char c) const { String result(*this); result += c; return result; }
    String operator+(unsigned char val) const { return *this + String(val); }
    friend String operator+(unsigned char lhs, const String& rhs) { return String(lhs) + rhs; }
    String operator+(int val) const { return *this + String(val); }
    friend String operator+(int lhs, const String& rhs) { return String(lhs) + rhs; }
    String operator+(unsigned int val) const { return *this + String(val); }
    friend String operator+(unsigned int lhs, const String& rhs) { return String(lhs) + rhs; }
    String operator+(long val) const { return *this + String(val); }
    friend String operator+(long lhs, const String& rhs) { return String(lhs) + rhs; }
    String operator+(unsigned long val) const { return *this + String(val); }
    friend String operator+(unsigned long lhs, const String& rhs) { return String(lhs) + rhs; }
    String operator+(long long val) const { return *this + String(val); }
    friend String operator+(long long lhs, const String& rhs) { return String(lhs) + rhs; }
    String operator+(unsigned long long val) const { return *this + String(val); }
    friend String operator+(unsigned long long lhs, const String& rhs) { return String(lhs) + rhs; }
    String operator+(float val) const { return *this + String(val); }
    friend String operator+(float lhs, const String& rhs) { return String(lhs) + rhs; }
    String operator+(double val) const { return *this + String(val); }
    friend String operator+(double lhs, const String& rhs) { return String(lhs) + rhs; }
    friend String operator+(char lhs, const String& rhs) { return String(lhs) + rhs; }

    friend String operator+(const char* lhs, const String& rhs) {
        return String((std::string(lhs ? lhs : "") + rhs._str).c_str());
    }

    bool concat(const String& rhs) { _str += rhs._str; return true; }
    bool concat(const char* rhs) { if (!rhs) return false; _str += rhs; return true; }
    bool concat(char c) { _str += c; return true; }
    bool concat(unsigned char val) { return concat(String(val)); }
    bool concat(int val) { return concat(String(val)); }
    bool concat(unsigned int val) { return concat(String(val)); }
    bool concat(long val) { return concat(String(val)); }
    bool concat(unsigned long val) { return concat(String(val)); }
    bool concat(long long val) { return concat(String(val)); }
    bool concat(unsigned long long val) { return concat(String(val)); }
    bool concat(float val) { return concat(String(val)); }
    bool concat(double val) { return concat(String(val)); }

    String& operator+=(const String& rhs) { concat(rhs); return *this; }
    String& operator+=(const char* rhs) { concat(rhs); return *this; }
    String& operator+=(char c) { concat(c); return *this; }
    String& operator+=(unsigned char val) { concat(val); return *this; }
    String& operator+=(int val) { concat(val); return *this; }
    String& operator+=(unsigned int val) { concat(val); return *this; }
    String& operator+=(long val) { concat(val); return *this; }
    String& operator+=(unsigned long val) { concat(val); return *this; }
    String& operator+=(long long val) { concat(val); return *this; }
    String& operator+=(unsigned long long val) { concat(val); return *this; }
    String& operator+=(float val) { concat(val); return *this; }
    String& operator+=(double val) { concat(val); return *this; }

    bool operator==(const String& rhs) const { return _str == rhs._str; }
    bool operator==(const char* rhs) const { return _str == (rhs ? rhs : ""); }
    bool operator!=(const String& rhs) const { return _str != rhs._str; }
    char operator[](unsigned int i) const { return charAt(i); }

    int indexOf(char c) const { auto p = _str.find(c); return p == std::string::npos ? -1 : (int)p; }
    String substring(unsigned int from) const { return String(_str.substr(from).c_str()); }
    String substring(unsigned int from, unsigned int to) const {
        return String(_str.substr(from, to - from).c_str());
    }
    void replace(const String& f, const String& r) {
        size_t pos = 0;
        while ((pos = _str.find(f._str, pos)) != std::string::npos) {
            _str.replace(pos, f._str.length(), r._str);
            pos += r._str.length();
        }
    }
    void trim() {
        auto s = _str.find_first_not_of(" \t\r\n");
        auto e = _str.find_last_not_of(" \t\r\n");
        _str = (s == std::string::npos) ? "" : _str.substr(s, e - s + 1);
    }

private:
    static std::string formatInteger(unsigned long long magnitude, unsigned char base, bool negative) {
        if (base < 2 || base > 16) return "";
        std::string result;
        do {
            result += "0123456789abcdef"[magnitude % base];
            magnitude /= base;
        } while (magnitude);
        if (negative) result += '-';
        std::reverse(result.begin(), result.end());
        return result;
    }

    static std::string formatFloat(double val, unsigned int dec) {
        if (std::isnan(val)) return "nan";
        if (std::isinf(val)) return "inf";
        const bool negative = val < 0;
        if (negative) val = -val;
        // Match Arduino-ESP32 dtostrf: round half up, then extract digits.
        double divisor = 2.0;
        for (unsigned int i = 0; i < dec; ++i) divisor *= 10.0;
        val += 1.0 / divisor;
        double place = 1.0;
        unsigned int digits = 1;
        while (val >= place * 10.0) { place *= 10.0; ++digits; }
        val /= place;
        std::string result;
        // Constructors use a minimum width of decimal places + 2.
        if (dec == 0 && digits == 1 && !negative) result += ' ';
        if (negative) result += '-';
        for (unsigned int i = 0; i < digits + dec; ++i) {
            if (i == digits && dec != 0) result += '.';
            const int digit = std::min(static_cast<int>(val), 9);
            result += static_cast<char>('0' + digit);
            val = (val - digit) * 10.0;
        }
        return result;
    }

    std::string _str;
};

// ---- Serial ----
extern "C" void js_serial_write(const char* str, int len);

class HardwareSerial {
public:
    void begin(unsigned long) {}
    void end() {}

    void print(const char* s)          { _write(s); }
    void print(const String& s)        { _write(s.c_str()); }
    void print(int v)                  { char b[16]; snprintf(b,sizeof(b),"%d",v);   _write(b); }
    void print(unsigned int v)         { char b[16]; snprintf(b,sizeof(b),"%u",v);   _write(b); }
    void print(long v)                 { char b[16]; snprintf(b,sizeof(b),"%ld",v);  _write(b); }
    void print(unsigned long v)        { char b[16]; snprintf(b,sizeof(b),"%lu",v);  _write(b); }
    void print(float v)                { char b[24]; snprintf(b,sizeof(b),"%f",v);   _write(b); }
    void print(double v)               { char b[24]; snprintf(b,sizeof(b),"%f",v);   _write(b); }
    void println()                     { _write("\n"); }
    void println(const char* s)        { _write(s); _write("\n"); }
    void println(const String& s)      { _write(s.c_str()); _write("\n"); }
    void println(int v)                { print(v);  _write("\n"); }
    void println(unsigned int v)       { print(v);  _write("\n"); }
    void println(long v)               { print(v);  _write("\n"); }
    void println(unsigned long v)      { print(v);  _write("\n"); }
    void println(float v)              { print(v);  _write("\n"); }
    void println(double v)             { print(v);  _write("\n"); }

    int available() { return 0; }
    int read() { return -1; }
    void flush() {}
    operator bool() { return true; }

private:
    void _write(const char* s) {
        int len = (int)strlen(s);
        js_serial_write(s, len);
    }
};

extern HardwareSerial Serial;

// ---- Misc Arduino defines ----
#define SDA 21
#define SCL 22
#define LED_BUILTIN 13
#define A0 0

#define bitRead(value, bit) (((value) >> (bit)) & 0x01)
#define bitSet(value, bit) ((value) |= (1 << (bit)))
#define bitClear(value, bit) ((value) &= ~(1 << (bit)))
#define bit(b) (1 << (b))

#ifndef PI
#define PI 3.14159265358979323846
#endif

inline float radians(float deg) { return deg * PI / 180.0f; }
inline float degrees(float rad) { return rad * 180.0f / PI; }

// ---- time.h compatibility ----
#include <ctime>

#endif // WASM_ARDUINO_H
