// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

// test/test_audio_engine_core/tone_script.h
//
// A fixed script of AudioManager calls (setVolume, playTone, stopTone,
// playSequence, stopSequence, playNote, stopNote, stopNotes) rendered in
// blocks of 256 frames, with every queued command applied at the start of
// the next block - what the device's engine task does. Two builds run it:
//
//   - the native test, through a model of the device glue
//     (lib/AudioManager/AudioManager.cpp: ToneControl, the shared volume
//     curve, EQ off = the pre-speaker signal);
//   - the emulator, through its real AudioManager and render path
//     (wasm/hal/audio_wasm.cpp, export wasm_audio_selftest_tone_script),
//     checked by wasm/audio_parity.mjs.
//
// Both must produce kToneScriptChecksum: FNV-1a 32 over every rendered
// sample (int16 little-endian), then one 16-bit word per block holding
// isSequencePlaying() as seen after that block.
//
// `Am` needs the AudioManager calls above and a ToneStep type with the
// AudioManager::ToneStep layout. `render(int16_t* out, int frames)` applies
// the queued commands and renders. Both start from a fresh engine at the
// default volume (0.7) with the speaker EQ off.

#ifndef CF_AUDIO_TONE_SCRIPT_H
#define CF_AUDIO_TONE_SCRIPT_H

#include <stdint.h>

#include "golden_script.h"   // fnv1a

namespace cf_audio_tone_script {

constexpr int kBlocks = 140;
constexpr int kBlockFrames = 256;

// Expected checksum (see above). Read by wasm/audio_parity.mjs too.
constexpr uint32_t kToneScriptChecksum = 0x1F7C78FBu;

template <class Am, class RenderFn>
uint32_t runToneScript(Am& am, RenderFn render) {
    typedef typename Am::ToneStep Step;
    static const Step jingle[] = {
        {1047.0f, 60, 10}, {0.0f, 20, 0}, {1318.5f, 40, 0}, {1568.0f, 80, 15},
    };
    static const Step blip[] = {{1500.0f, 30, 0}, {1700.0f, 30, 0}};

    int16_t buf[kBlockFrames];
    uint32_t h = 2166136261u;
    int n1 = -1, n2 = -1;
    for (int b = 0; b < kBlocks; ++b) {
        switch (b) {
            case 0:  am.playTone(880.0f, 120); break;
            case 10: am.playSequence(jingle, 4); break;
            case 40:
                n1 = am.playNote(523.25f, 0);
                n2 = am.playNote(659.25f, 0);
                am.playNote(783.99f, 200);
                break;
            case 50: am.setVolume(0.35f); break;
            case 60: am.stopNote(n1); break;
            case 70: am.playTone(440.0f, 0); break;
            case 80: am.stopNotes(); break;
            case 85: am.stopNote(n2); break;   // already stopped: nothing
            case 90: am.stopTone(); break;
            case 100:
                am.setVolume(1.0f);
                am.playSequence(blip, 2);
                break;
            case 104: am.stopSequence(); break;
            case 110: am.playTone(2000.0f, 30); break;
            case 112: am.playTone(0.0f, 0); break;   // 0 Hz = stop
            case 120: am.playSequence(blip, 2); break;
            default: break;
        }
        render(buf, kBlockFrames);
        h = cf_audio_golden::fnv1a(h, buf, kBlockFrames);
        const int16_t playing = am.isSequencePlaying() ? 1 : 0;
        h = cf_audio_golden::fnv1a(h, &playing, 1);
    }
    return h;
}

}  // namespace cf_audio_tone_script

#endif  // CF_AUDIO_TONE_SCRIPT_H
