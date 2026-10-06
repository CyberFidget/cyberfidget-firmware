// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC
//
// Guest-side AudioManager shim forwarding to "cf" imports. ToneStep layout
// (f32 + u16 + u16 = 8 bytes, little-endian) must match
// lib/AudioManager/AudioManager.h — the host memcpys steps straight out of
// guest linear memory.

#ifndef AUDIO_MANAGER_H  // same guard as the real header — must shadow it
#define AUDIO_MANAGER_H

#include <stdint.h>

#include "cf_hal_imports.h"

class AudioManager {
public:
    struct ToneStep {
        float    freq;        // 0 = rest
        uint16_t durationMs;
        uint16_t gapAfterMs;
    };
    static_assert(sizeof(ToneStep) == 8, "ToneStep layout must match firmware");

    void setVolume(float) {}  // volume stays host-controlled
    void playTone(float frequency, int durationMs = 0) { cf_tone_play(frequency, durationMs); }
    void stopTone() { cf_tone_stop(); }
    int playNote(float frequency, int durationMs = 0) { return cf_note_play(frequency, durationMs); }
    void stopNote(int handle) { cf_note_stop(handle); }
    void stopNotes() { cf_note_all_off(); }
    void enableMic(bool on) { cf_mic_enable(on); }
    float getMicVolumeLinear() const { return cf_mic_level(); }
    float getMicVolumeDb() const { return cf_mic_level_db(); }
    void playSequence(const ToneStep* steps, int count) { cf_seq_play(steps, count); }
    void stopSequence() { cf_seq_stop(); }
    bool isSequencePlaying() const { return false; }  // not tracked guest-side
};

#endif  // AUDIO_MANAGER_H
