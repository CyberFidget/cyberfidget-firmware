// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

// lib/AudioManager/SpeakerEqPresets.h
//
// Speaker EQ presets for the on-board speaker. AudioManager applies
// kSpeakerEqDefault every time the audio engine starts (boot, and when an app
// hands port 0 back). The engine core itself stays flat by default. A future
// settings choice can select another entry; index 0 is "off".

#ifndef SPEAKER_EQ_PRESETS_H
#define SPEAKER_EQ_PRESETS_H

#include "AudioEngine.h"

struct SpeakerEqPreset {
    const char* name;
    bool enabled;              // false: EQ bypassed (eq is ignored)
    cf_audio::SpeakerEq eq;    // hpf, peak 1 (Hz, dB, Q), peak 2 (Hz, dB, Q), make-up dB
};

constexpr SpeakerEqPreset kSpeakerEqPresets[] = {
    {"Off", false, {0, 0, 0, 0, 0, 0, 0, 0}},
    // Tuned on an enclosed unit from measurements and listening: the speaker
    // gives almost nothing below ~750 Hz, so cut it and spend the headroom on
    // presence (2.5 kHz) while taming the top.
    {"Speaker", true, {750, 2500, 4, 0.7f, 9000, -3, 1.0f, 2}},
};
constexpr int kSpeakerEqPresetCount = (int)(sizeof(kSpeakerEqPresets) / sizeof(kSpeakerEqPresets[0]));
constexpr int kSpeakerEqDefault = 1;

#endif  // SPEAKER_EQ_PRESETS_H
