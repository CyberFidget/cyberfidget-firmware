// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

// lib/Fonts/FontLinkage.h
//
// Force-included into every device translation unit (platformio.ini
// build_flags: -include lib/Fonts/FontLinkage.h). Do not include it directly.
//
// The ThingPulse OLED library defines its ArialMT fonts in a header
// (OLEDDisplayFonts.h) as plain namespace-scope `const` arrays. In C++ that
// means internal linkage, so every .cpp that pulls in OLEDDisplay.h and uses
// a font got its own private copy in flash (the 10 px font was linked about
// 26 times). Declaring the fonts `extern` and weak here, before that header
// is seen, gives its definitions external weak linkage: every translation
// unit still emits the same bytes, and the linker keeps exactly one copy.
//
// C and assembly sources are skipped: in C a file-scope const already has
// external linkage, and nothing C-side includes the font header.
//
// The emulator build does not use this file; it has its own font shims
// (wasm/shims/OLEDDisplayFonts.h).

#ifndef CF_FONT_LINKAGE_H
#define CF_FONT_LINKAGE_H

#ifdef __cplusplus
#include <stdint.h>

extern const uint8_t ArialMT_Plain_10[] __attribute__((weak));
extern const uint8_t ArialMT_Plain_16[] __attribute__((weak));
extern const uint8_t ArialMT_Plain_24[] __attribute__((weak));
#endif

#endif  // CF_FONT_LINKAGE_H
