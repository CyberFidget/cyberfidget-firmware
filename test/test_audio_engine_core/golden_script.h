// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

// test/test_audio_engine_core/golden_script.h
//
// The fixed command script behind the audio engine's golden checksum. Any
// other build of the engine (the emulator's, for one) renders this same
// script and must produce the same checksum:
//
//   cf_audio::Engine e(kGoldenSeed);
//   for block b in 0 .. kGoldenBlocks-1:
//       apply every script entry whose block == b (in listed order)
//       e.render(buf, kGoldenBlockFrames)
//   checksum = FNV-1a 32 over every rendered sample as 2 little-endian bytes
//
// It touches every waveform (the tone sequences play the tone voice's soft
// square with 2, 3 and 4 harmonics), envelopes with decay and zero sustain, a
// tone sequence with rests and gaps, voice stealing, a master-volume ramp, an
// 8-voice overload (limiter) and the AllOff fade.

#ifndef CF_AUDIO_GOLDEN_SCRIPT_H
#define CF_AUDIO_GOLDEN_SCRIPT_H

#include <stdint.h>
#include <string.h>

#include "AudioEngine.h"

namespace cf_audio_golden {

using namespace cf_audio;

constexpr uint32_t kGoldenSeed = 0x1234;
constexpr int kGoldenBlocks = 200;
constexpr int kGoldenBlockFrames = 256;

// Expected FNV-1a 32 checksum of the rendered script (see test_audio_engine.cpp).
constexpr uint32_t kGoldenChecksum = 0xF420006Fu;

inline uint32_t fnv1a(uint32_t h, const int16_t* s, int n) {
    for (int i = 0; i < n; ++i) {
        const uint16_t u = (uint16_t)s[i];
        h = (h ^ (u & 0xFFu)) * 16777619u;
        h = (h ^ (u >> 8)) * 16777619u;
    }
    return h;
}

// Applies the script entries for block `b`. Sequences go through the engine's
// mailbox exactly as AudioManager sends them.
inline void applyGoldenBlock(Engine& e, int b) {
    const Envelope pluck = {88, 1764, 19661, 441};    // 2 ms A, 40 ms D, 0.6 S, 10 ms R
    const Envelope pad   = {44, 0, kSustainFull, 882};
    const Envelope drum  = {44, 1000, 0, 220};
    const Envelope full  = {22, 0, kSustainFull, 220};

    auto playSeq = [&](uint32_t gen, const float* hz, const uint16_t* dur, const uint16_t* gap, int n) {
        SeqStep* buf = e.mailbox().beginWrite();
        for (int i = 0; i < n; ++i) {
            buf[i].inc = Engine::hzToInc(hz[i]);
            buf[i].durMs = dur[i];
            buf[i].gapMs = gap[i];
        }
        e.mailbox().commit(n, gen);
        e.apply(Command::seqPlay(gen));
    };

    switch (b) {
        case 0: {
            e.apply(Command::master(32768));
            const float hz[] = {1047.0f, 0.0f, 1318.5f, 1568.0f, 2093.0f};
            const uint16_t dur[] = {60, 20, 40, 80, 25};
            const uint16_t gap[] = {10, 0, 0, 15, 5};
            playSeq(1, hz, dur, gap, 5);
            e.apply(Command::noteOn(1, kPulse, Engine::hzToInc(523.25f), Engine::msToSamples(200), 160, 64, pluck));
            break;
        }
        case 10:
            e.apply(Command::noteOn(kAnyVoice, kSaw, Engine::hzToInc(220.0f), Engine::msToSamples(300), 180, 128, pad));
            break;
        case 20:
            e.apply(Command::noteOn(kAnyVoice, kTriangle, Engine::hzToInc(880.0f), 0, 200, 128, pad));
            break;
        case 30:
            e.apply(Command::noteOn(kAnyVoice, kNoise, Engine::hzToInc(8000.0f), Engine::msToSamples(30), 120, 128, drum));
            break;
        case 40:
            e.apply(Command::master(16384));
            break;
        case 60:
            e.apply(Command::noteOff(3));
            break;
        case 80:
            for (uint8_t v = 0; v < kVoices; ++v) {
                const Wave w = (v & 1) ? kSaw : kPulse;
                e.apply(Command::noteOn(v, w, Engine::hzToInc(440.0f * (float)(v + 1)), Engine::msToSamples(150),
                                        kLevelUnity, 128, full));
            }
            e.apply(Command::noteOn(kAnyVoice, kSine, Engine::hzToInc(3000.0f), Engine::msToSamples(50),
                                    kLevelUnity, 128, full));   // steals the oldest
            break;
        case 120:
            e.apply(Command::master(32768));
            break;
        case 130:
            e.apply(Command::allOff());
            break;
        case 140:
            e.apply(Command::noteOn(kToneVoice, kSine, Engine::hzToInc(2000.0f), Engine::msToSamples(100),
                                    kToneLevel, 128, kToneEnvelope));
            break;
        case 150: {
            const float hz[] = {1500.0f, 1700.0f};
            const uint16_t dur[] = {30, 30};
            const uint16_t gap[] = {0, 0};
            playSeq(2, hz, dur, gap, 2);
            break;
        }
        case 152:
            e.apply(Command::seqStop(3));
            e.apply(Command::noteOff(kToneVoice));
            break;
        default:
            break;
    }
}

inline uint32_t renderGolden(Engine& e) {
    int16_t buf[kGoldenBlockFrames];
    uint32_t h = 2166136261u;
    for (int b = 0; b < kGoldenBlocks; ++b) {
        applyGoldenBlock(e, b);
        e.render(buf, kGoldenBlockFrames);
        h = fnv1a(h, buf, kGoldenBlockFrames);
    }
    return h;
}

}  // namespace cf_audio_golden

#endif  // CF_AUDIO_GOLDEN_SCRIPT_H
