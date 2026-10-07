// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

// lib/AudioManager/SpeakerEqPresets.h
//
// Speaker EQ presets for the on-board speaker. The user picks one in
// Settings > Sound > Speaker EQ; AudioManager applies the saved one every time
// the audio engine starts (boot, and when an app hands port 0 back). The
// engine core itself stays flat by default.
//
// The array index is the id stored in settings: only ever APPEND new presets,
// never reorder or remove one (an unknown or missing id falls back to
// kSpeakerEqDefault).

#ifndef SPEAKER_EQ_PRESETS_H
#define SPEAKER_EQ_PRESETS_H

#include <stdint.h>

#include "AudioEngine.h"

struct SpeakerEqPreset {
    const char* name;          // shown on the Settings screen: plain words
    const char* hint;          // one short line under the name
    bool enabled;              // false: EQ bypassed (eq is ignored)
    cf_audio::SpeakerEq eq;    // hpf, peak 1 (Hz, dB, Q), peak 2 (Hz, dB, Q), make-up dB
};

constexpr SpeakerEqPreset kSpeakerEqPresets[] = {
    // Tuned on an enclosed unit from measurements and listening: the speaker
    // gives almost nothing below ~700 Hz, so cut it (gently, at 600 Hz, so
    // low tones keep some body) and spend the headroom on presence (2.5 kHz)
    // while taming the top.
    {"Balanced", "Clear and even", true, {600, 2500, 4, 0.7f, 9000, -3, 1.0f, 2}},
    // More presence and 1 dB more make-up gain: louder, a little sharper.
    {"Loud", "Louder and punchier", true, {600, 2500, 6, 0.7f, 9000, -3, 1.0f, 3}},
    // No boost or cut, but the 600 Hz high-pass stays: the speaker cannot
    // reproduce those low frequencies, and they only cost headroom.
    {"Off", "Plain, no shaping", true, {600, 2500, 0, 0.7f, 9000, 0, 1.0f, 0}},
};
constexpr int kSpeakerEqPresetCount = (int)(sizeof(kSpeakerEqPresets) / sizeof(kSpeakerEqPresets[0]));
constexpr int kSpeakerEqDefault = 0;   // Balanced

// The preset to use for a stored setting: the stored id when it names a
// preset, else the default (nothing stored yet, or an id this image does not
// know, e.g. after a rollback).
constexpr int speakerEqPresetFromStored(bool hasValue, uint8_t value) {
    return (hasValue && value < kSpeakerEqPresetCount) ? (int)value : kSpeakerEqDefault;
}

#endif  // SPEAKER_EQ_PRESETS_H
