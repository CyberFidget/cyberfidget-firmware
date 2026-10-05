// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

// test/test_audio_engine_core/test_audio_engine.cpp
//
// Native tests for the platform-independent audio engine (lib/AudioEngine):
// envelope and gate lengths in samples, sample-exact sequence timing,
// voice stealing, the limiter under an 8-voice overload, the master-volume
// ramp, the DC blocker, and the golden checksum of a fixed command script.

#include <unity.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "AudioEngine.h"
#include "golden_script.h"

using namespace cf_audio;

namespace {

Engine g_engine;          // big (render buffers inside): keep off the stack
int16_t g_buf[44100];

void renderOne(Engine& e) {
    int16_t s;
    e.render(&s, 1);
}

void playSequence(Engine& e, uint32_t gen, const SeqStep* steps, int n) {
    SeqStep* buf = e.mailbox().beginWrite();
    TEST_ASSERT_NOT_NULL(buf);
    memcpy(buf, steps, sizeof(SeqStep) * (size_t)n);
    e.mailbox().commit(n, gen);
    e.apply(Command::seqPlay(gen));
}

}  // namespace

void setUp() { g_engine.reset(1); }
void tearDown() {}

// ----------------------------------------------------------------- envelope

void test_tone_envelope_and_gate_lengths_in_samples() {
    Engine& e = g_engine;
    const uint32_t gate = 1000;
    e.apply(Command::noteOn(2, kPulse, Engine::hzToInc(1000.0f), gate, kLevelUnity, 128, kToneEnvelope));

    // Attack: 132 samples, rising every sample, full after the last one.
    int32_t prev = 0;
    for (int i = 0; i < kToneEnvelope.attack; ++i) {
        renderOne(e);
        const auto info = e.voiceInfo(2);
        TEST_ASSERT_TRUE(info.active);
        TEST_ASSERT_TRUE(info.env > prev);
        prev = info.env;
    }
    TEST_ASSERT_EQUAL_INT32(kEnvOne, e.voiceInfo(2).env);
    TEST_ASSERT_EQUAL_UINT8(Engine::kSustain, e.voiceInfo(2).stage);

    // Sustain until the gate ends after `gate` samples in total.
    for (uint32_t i = kToneEnvelope.attack; i < gate - 1; ++i) renderOne(e);
    TEST_ASSERT_EQUAL_UINT8(Engine::kSustain, e.voiceInfo(2).stage);
    renderOne(e);   // sample gate-1: the gate expires after it
    TEST_ASSERT_EQUAL_UINT8(Engine::kRelease, e.voiceInfo(2).stage);
    TEST_ASSERT_EQUAL_INT32(kEnvOne, e.voiceInfo(2).env);

    // Release: 220 samples, then idle. Total sounding length = gate + release.
    for (int i = 0; i < kToneEnvelope.release - 1; ++i) renderOne(e);
    TEST_ASSERT_TRUE(e.voiceInfo(2).active);
    TEST_ASSERT_TRUE(e.voiceInfo(2).env > 0);
    renderOne(e);
    TEST_ASSERT_FALSE(e.voiceInfo(2).active);
    TEST_ASSERT_EQUAL_INT32(0, e.voiceInfo(2).env);
}

void test_tone_duration_from_milliseconds() {
    // A 40 ms tone sounds for 40 ms plus the 5 ms release.
    Engine& e = g_engine;
    e.apply(Command::noteOn(kToneVoice, kSine, Engine::hzToInc(1000.0f), Engine::msToSamples(40),
                            kToneLevel, 128, kToneEnvelope));
    const uint32_t expected = Engine::msToSamples(40) + kToneEnvelope.release;   // 1764 + 220
    uint32_t sounding = 0;
    for (int i = 0; i < 4000; ++i) {
        renderOne(e);
        if (e.voiceInfo(kToneVoice).active) ++sounding;
    }
    TEST_ASSERT_EQUAL_UINT32(expected - 1, sounding);   // idle after the last release sample
}

void test_gate_shorter_than_attack_releases_from_partial_level() {
    Engine& e = g_engine;
    e.apply(Command::noteOn(0, kSine, Engine::hzToInc(1000.0f), 50, kLevelUnity, 128, kToneEnvelope));
    for (int i = 0; i < 50; ++i) renderOne(e);
    const auto info = e.voiceInfo(0);
    TEST_ASSERT_EQUAL_UINT8(Engine::kRelease, info.stage);
    TEST_ASSERT_TRUE(info.env > 0 && info.env < kEnvOne / 2);
    for (int i = 0; i < kToneEnvelope.release; ++i) renderOne(e);
    TEST_ASSERT_FALSE(e.voiceInfo(0).active);
}

void test_decay_to_zero_sustain_frees_the_voice() {
    Engine& e = g_engine;
    const Envelope drum = {10, 100, 0, 50};
    e.apply(Command::noteOn(5, kNoise, Engine::hzToInc(8000.0f), 0, kLevelUnity, 128, drum));
    for (int i = 0; i < 109; ++i) renderOne(e);
    TEST_ASSERT_TRUE(e.voiceInfo(5).active);
    renderOne(e);
    TEST_ASSERT_FALSE(e.voiceInfo(5).active);
}

// ---------------------------------------------------------------- sequencer

void test_sequence_step_timing_is_sample_exact() {
    Engine& e = g_engine;
    const SeqStep steps[] = {
        {Engine::hzToInc(1000.0f), 10, 5},   // tone 0..441, next boundary at 15 ms = 661
        {0, 7, 0},                            // rest; next boundary at 22 ms = 970
        {Engine::hzToInc(2000.0f), 3, 2},    // tone 970..1102 (25 ms), end at 27 ms = 1190
    };
    playSequence(e, 1, steps, 3);
    TEST_ASSERT_EQUAL_UINT32((1u << 1) | 1u, e.sequenceStatus());

    int firstOn[2] = {-1, -1};
    int idleAt[2] = {-1, -1};
    uint32_t releaseAt[2] = {0, 0};
    int note = 0;
    bool wasSounding = false, wasActive = false;
    int seqEnd = -1;
    for (int i = 0; i < 1400 && note < 2; ++i) {
        renderOne(e);
        const auto info = e.voiceInfo(kToneVoice);
        const bool sounding = info.active && info.env > 0;   // this sample carried the note
        if (sounding && !wasSounding && firstOn[note] < 0) firstOn[note] = i;
        if (info.stage == Engine::kRelease && releaseAt[note] == 0) releaseAt[note] = (uint32_t)i;
        if (!info.active && wasActive) idleAt[note++] = i;
        wasSounding = sounding;
        wasActive = info.active;
        if (seqEnd < 0 && (e.sequenceStatus() & 1u) == 0) seqEnd = i;
    }
    // Note 1: first sample 0, gate 441 samples, release begins after sample 440.
    TEST_ASSERT_EQUAL_INT(0, firstOn[0]);
    TEST_ASSERT_EQUAL_UINT32(440, releaseAt[0]);
    TEST_ASSERT_EQUAL_INT(441 + kToneEnvelope.release - 1, idleAt[0]);
    // Note 2 starts exactly at floor(22 ms * 44.1) = 970; gate to 25 ms (1102).
    TEST_ASSERT_EQUAL_INT(970, firstOn[1]);
    TEST_ASSERT_EQUAL_UINT32(1101, releaseAt[1]);
    TEST_ASSERT_EQUAL_INT(1102 + kToneEnvelope.release - 1, idleAt[1]);
    // The sequence ends at 27 ms = 1190: status flips once the clock reaches it.
    TEST_ASSERT_EQUAL_INT(1189, seqEnd);
    TEST_ASSERT_EQUAL_UINT32((1u << 1) | 0u, e.sequenceStatus());
}

void test_sequence_rest_silences_a_held_tone() {
    Engine& e = g_engine;
    e.apply(Command::noteOn(kToneVoice, kSine, Engine::hzToInc(1000.0f), 0, kToneLevel, 128, kToneEnvelope));
    for (int i = 0; i < 500; ++i) renderOne(e);
    const SeqStep steps[] = {{0, 20, 0}};
    playSequence(e, 4, steps, 1);
    renderOne(e);
    TEST_ASSERT_EQUAL_UINT8(Engine::kRelease, e.voiceInfo(kToneVoice).stage);
}

void test_sequence_latest_play_wins_and_stop_publishes() {
    Engine& e = g_engine;
    const SeqStep a[] = {{Engine::hzToInc(500.0f), 100, 0}};
    const SeqStep b[] = {{Engine::hzToInc(700.0f), 100, 0}};
    // Two plays before the render thread runs: only the newer list plays.
    SeqStep* buf = e.mailbox().beginWrite();
    memcpy(buf, a, sizeof(a));
    e.mailbox().commit(1, 7);
    buf = e.mailbox().beginWrite();
    memcpy(buf, b, sizeof(b));
    e.mailbox().commit(1, 8);
    e.apply(Command::seqPlay(7));   // stale: ignored
    TEST_ASSERT_EQUAL_UINT32(0u, e.sequenceStatus());
    e.apply(Command::seqPlay(8));
    TEST_ASSERT_EQUAL_UINT32((8u << 1) | 1u, e.sequenceStatus());
    renderOne(e);
    TEST_ASSERT_EQUAL_UINT32(Engine::hzToInc(700.0f), e.voiceInfo(kToneVoice).inc);
    e.apply(Command::seqStop(9));
    TEST_ASSERT_EQUAL_UINT32((9u << 1) | 0u, e.sequenceStatus());
}

// ------------------------------------------------------------ voice stealing

void test_voice_stealing_policy() {
    Engine& e = g_engine;
    const Envelope env = {10, 0, kSustainFull, 100};
    // Idle voices are taken lowest index first.
    for (int i = 0; i < kVoices; ++i) {
        e.apply(Command::noteOn(kAnyVoice, kPulse, Engine::hzToInc(300.0f + 100.0f * i), 0, 100, 128, env));
        TEST_ASSERT_EQUAL_UINT32(Engine::hzToInc(300.0f + 100.0f * i), e.voiceInfo(i).inc);
        renderOne(e);
    }
    // All busy, none releasing: the oldest note (voice 0) is stolen.
    e.apply(Command::noteOn(kAnyVoice, kPulse, Engine::hzToInc(5000.0f), 0, 100, 128, env));
    TEST_ASSERT_EQUAL_UINT32(Engine::hzToInc(5000.0f), e.voiceInfo(0).inc);
    renderOne(e);
    // Two voices releasing: the quieter one (released earlier) is taken.
    e.apply(Command::noteOff(4));
    for (int i = 0; i < 30; ++i) renderOne(e);
    e.apply(Command::noteOff(6));
    renderOne(e);
    TEST_ASSERT_TRUE(e.voiceInfo(4).env < e.voiceInfo(6).env);
    e.apply(Command::noteOn(kAnyVoice, kPulse, Engine::hzToInc(6000.0f), 0, 100, 128, env));
    TEST_ASSERT_EQUAL_UINT32(Engine::hzToInc(6000.0f), e.voiceInfo(4).inc);
    // Once a voice is idle it wins over releasing ones.
    for (int i = 0; i < 200; ++i) renderOne(e);   // voice 6 finishes its release
    TEST_ASSERT_FALSE(e.voiceInfo(6).active);
    e.apply(Command::noteOn(kAnyVoice, kPulse, Engine::hzToInc(7000.0f), 0, 100, 128, env));
    TEST_ASSERT_EQUAL_UINT32(Engine::hzToInc(7000.0f), e.voiceInfo(6).inc);
}

// --------------------------------------------------------------------- bus

void test_limiter_never_wraps_with_eight_full_scale_voices() {
    Engine& e = g_engine;
    const Envelope env = {0, 0, kSustainFull, 0};   // instant edges: worst case
    for (uint8_t v = 0; v < kVoices; ++v) {
        // In-phase pulses and saws, all at unity level and unity master.
        const Wave w = (v < 6) ? kPulse : kSaw;
        e.apply(Command::noteOn(v, w, Engine::hzToInc(1000.0f), 0, kLevelUnity, 128, env));
    }
    e.render(g_buf, 44100);
    int32_t peak = 0;
    for (int i = 0; i < 44100; ++i) {
        const int32_t a = g_buf[i] < 0 ? -(int32_t)g_buf[i] : g_buf[i];
        if (a > peak) peak = a;
    }
    TEST_ASSERT_EQUAL_UINT32(0, e.clipCount());
    TEST_ASSERT_TRUE(peak <= 32701);
    TEST_ASSERT_TRUE(peak > 25000);
    // The pulse's sign survives (no wrap): the first half-cycle is positive.
    TEST_ASSERT_TRUE(g_buf[5] > 0);
    TEST_ASSERT_TRUE(g_buf[30] < 0);
    TEST_ASSERT_EQUAL_UINT32(8, e.peakVoices());
}

void test_quiet_tone_passes_the_limiter_untouched() {
    // Even a full-volume tone stays below the knee: the bus applies unity gain,
    // so the sine's peak is the voice's own level.
    Engine& e = g_engine;
    e.apply(Command::noteOn(kToneVoice, kSine, Engine::hzToInc(1000.0f), 0, kToneLevel, 128, kToneEnvelope));
    e.render(g_buf, 44100);
    int32_t peak = 0;
    for (int i = 22050; i < 44100; ++i) {
        const int32_t a = g_buf[i] < 0 ? -(int32_t)g_buf[i] : g_buf[i];
        if (a > peak) peak = a;
    }
    // 32767 * 230 / 256 = 29439; the DC blocker adds ~0.2 % at 1 kHz.
    TEST_ASSERT_INT_WITHIN(150, 29439 + 50, peak);
    TEST_ASSERT_EQUAL_UINT32(0, e.clipCount());
}

void test_master_volume_ramps_without_a_step() {
    Engine& e = g_engine;
    TEST_ASSERT_EQUAL_INT32(1 << 30, e.masterGainQ30());
    // While silent a change applies at once (nothing to ramp).
    e.apply(Command::master(16384));
    TEST_ASSERT_EQUAL_INT32(16384 << 15, e.masterGainQ30());
    e.apply(Command::master(32768));
    TEST_ASSERT_EQUAL_INT32(1 << 30, e.masterGainQ30());
    // While a voice sounds it ramps over 10 ms.
    e.apply(Command::noteOn(0, kSine, Engine::hzToInc(1000.0f), 0, kToneLevel, 128, kToneEnvelope));
    e.apply(Command::master(8192));   // 1.0 -> 0.25
    const int32_t target = 8192 << 15;
    // Equal steps; the last one also absorbs the division remainder (< 441
    // parts in 2^30, inaudible).
    const int32_t maxStep = ((1 << 30) - target) / (int32_t)kMasterRampSamples + (int32_t)kMasterRampSamples;
    int32_t prev = e.masterGainQ30();
    for (uint32_t i = 0; i < kMasterRampSamples; ++i) {
        renderOne(e);
        const int32_t g = e.masterGainQ30();
        TEST_ASSERT_TRUE(g <= prev);
        TEST_ASSERT_TRUE(prev - g <= maxStep);
        prev = g;
    }
    TEST_ASSERT_EQUAL_INT32(target, e.masterGainQ30());

    // Output side: a held sine never jumps between samples by more than the
    // waveform itself allows while the volume changes.
    e.reset(1);
    e.apply(Command::noteOn(0, kSine, Engine::hzToInc(100.0f), 0, kLevelUnity, 128, kToneEnvelope));
    e.render(g_buf, 4410);
    e.apply(Command::master(0));
    e.render(g_buf, 4410);
    int32_t worst = 0;
    for (int i = 1; i < 4410; ++i) {
        const int32_t d = abs((int32_t)g_buf[i] - (int32_t)g_buf[i - 1]);
        if (d > worst) worst = d;
    }
    // 100 Hz sine at 29.7k peak moves at most ~470 per sample.
    TEST_ASSERT_TRUE(worst < 600);
    TEST_ASSERT_EQUAL_INT16(0, g_buf[4409]);
}

void test_dc_blocker_removes_offset_and_settles_to_zero() {
    Engine& e = g_engine;
    // 25 % pulse: heavy DC offset at the voice.
    e.apply(Command::noteOn(0, kPulse, Engine::hzToInc(1000.0f), 0, 128, 64, kToneEnvelope));
    e.render(g_buf, 22050);
    e.render(g_buf, 4410);   // 100 ms = 100 whole cycles
    int64_t sum = 0;
    for (int i = 0; i < 4410; ++i) sum += g_buf[i];
    TEST_ASSERT_TRUE(llabs(sum / 4410) < 100);
    e.apply(Command::allOff());
    e.render(g_buf, 22050);
    for (int i = 21050; i < 22050; ++i) TEST_ASSERT_EQUAL_INT16(0, g_buf[i]);
    TEST_ASSERT_EQUAL_INT(0, e.activeVoices());
}

void test_alloff_fades_within_five_ms() {
    Engine& e = g_engine;
    const Envelope slow = {10, 0, kSustainFull, 20000};
    for (uint8_t v = 0; v < 4; ++v)
        e.apply(Command::noteOn(v, kSaw, Engine::hzToInc(500.0f), 0, 200, 128, slow));
    e.render(g_buf, 1000);
    e.apply(Command::allOff());
    for (int i = 0; i < kFastRelease - 1; ++i) renderOne(e);
    TEST_ASSERT_EQUAL_INT(4, e.activeVoices());
    renderOne(e);
    TEST_ASSERT_EQUAL_INT(0, e.activeVoices());
}

void test_eq_default_bypassed_and_commit_is_atomic() {
    Engine& e = g_engine;
    e.apply(Command::noteOn(0, kSine, Engine::hzToInc(1000.0f), 0, kToneLevel, 128, kToneEnvelope));
    e.render(g_buf, 2000);
    static int16_t ref[2000];
    Engine* other = new Engine(1);
    other->apply(Command::noteOn(0, kSine, Engine::hzToInc(1000.0f), 0, kToneLevel, 128, kToneEnvelope));
    // Staged bands without a commit change nothing.
    other->apply(Command::eqBand(0, designHighpass(800.0, 0.7071)));
    other->render(ref, 2000);
    TEST_ASSERT_EQUAL_INT16_ARRAY(g_buf, ref, 2000);
    // Committing a flat (unity) band keeps a sine essentially unchanged.
    BiquadCoefs unity = {1 << 29, 0, 0, 0, 0};
    other->apply(Command::eqBand(0, unity));
    other->apply(Command::eqCommit(1, true, 256));
    e.render(g_buf, 2000);
    other->render(ref, 2000);
    TEST_ASSERT_EQUAL_INT16_ARRAY(g_buf, ref, 2000);
    delete other;
}

// ------------------------------------------------------ click-free switches

int32_t maxStep(const int16_t* b, int from, int to) {
    int32_t worst = 0;
    for (int i = from + 1; i < to; ++i) {
        const int32_t d = abs((int32_t)b[i] - (int32_t)b[i - 1]);
        if (d > worst) worst = d;
    }
    return worst;
}

void test_wave_and_level_switch_fades_instead_of_jumping() {
    Engine& e = g_engine;
    const Envelope held = {44, 0, kSustainFull, 220};
    // 200 Hz sine at full level; switch at a waveform peak (sample 4465 =
    // 20.25 cycles) to a quiet triangle: an instant switch would jump ~32k.
    e.apply(Command::noteOn(3, kSine, Engine::hzToInc(200.0f), 0, kLevelUnity, 128, held));
    e.render(g_buf, 4465);
    const int32_t baseline = maxStep(g_buf, 2000, 4465);   // the sine's own slope
    e.apply(Command::noteOn(3, kTriangle, Engine::hzToInc(400.0f), 0, 64, 128, held));
    e.render(g_buf + 4465, 1000);
    const int32_t worst = maxStep(g_buf, 4460, 5465);
    char msg[64];
    snprintf(msg, sizeof(msg), "baseline=%d switch=%d", (int)baseline, (int)worst);
    TEST_MESSAGE(msg);
    TEST_ASSERT_TRUE(worst < baseline + 600);
    // The new note took over once the old one had faded.
    TEST_ASSERT_EQUAL_UINT32(Engine::hzToInc(400.0f), e.voiceInfo(3).inc);

    // Same wave, lower level: also faded, not stepped.
    e.reset(1);
    e.apply(Command::noteOn(0, kSine, Engine::hzToInc(200.0f), 0, kLevelUnity, 128, held));
    e.render(g_buf, 4465);
    e.apply(Command::noteOn(0, kSine, Engine::hzToInc(200.0f), 0, 64, 128, held));
    e.render(g_buf + 4465, 1000);
    TEST_ASSERT_TRUE(maxStep(g_buf, 4460, 5465) < baseline + 600);

    // A pending note is dropped by noteOff during its fade.
    e.reset(1);
    e.apply(Command::noteOn(0, kSine, Engine::hzToInc(200.0f), 0, kLevelUnity, 128, held));
    e.render(g_buf, 500);
    e.apply(Command::noteOn(0, kSaw, Engine::hzToInc(900.0f), 0, kLevelUnity, 128, held));
    e.apply(Command::noteOff(0));
    e.render(g_buf, 2000);
    TEST_ASSERT_FALSE(e.voiceInfo(0).active);
}

void test_voice_steal_fades_the_stolen_note() {
    Engine& e = g_engine;
    const Envelope held = {44, 0, kSustainFull, 220};
    for (uint8_t v = 0; v < kVoices; ++v)
        e.apply(Command::noteOn(v, kSine, Engine::hzToInc(200.0f), 0, 28, 128, held));
    e.render(g_buf, 4465);
    const int32_t baseline = maxStep(g_buf, 2000, 4465);
    // All busy: kAnyVoice steals voice 0 (oldest), which is at its peak.
    e.apply(Command::noteOn(kAnyVoice, kPulse, Engine::hzToInc(1000.0f), 0, 28, 128, held));
    e.render(g_buf + 4465, 1000);
    const int32_t worst = maxStep(g_buf, 4460, 4465 + kSwitchFade + 10);
    TEST_ASSERT_TRUE(worst < baseline + 300);
    TEST_ASSERT_EQUAL_UINT32(Engine::hzToInc(1000.0f), e.voiceInfo(0).inc);
}

// ------------------------------------------------------------- EQ limits

void test_eq_rejects_out_of_range_settings() {
    const BiquadCoefs flat = {1 << 29, 0, 0, 0, 0};
    const BiquadCoefs unstable = {1 << 29, 0, 0, 0, 1 << 29};          // |a2| = 1
    const BiquadCoefs unstable2 = {1 << 29, 0, 0, (int32_t)(3u << 28), 0};   // |a1| = 1.5
    TEST_ASSERT_TRUE(Engine::eqSettingValid(&flat, 1, 256));
    TEST_ASSERT_TRUE(Engine::eqSettingValid(&flat, 1, kEqMaxGainQ8));
    TEST_ASSERT_FALSE(Engine::eqSettingValid(&flat, 1, kEqMaxGainQ8 + 1));
    TEST_ASSERT_FALSE(Engine::eqSettingValid(&flat, 1, -1));
    TEST_ASSERT_FALSE(Engine::eqSettingValid(&unstable, 1, 256));
    TEST_ASSERT_FALSE(Engine::eqSettingValid(&unstable2, 1, 256));
    BiquadCoefs minA1 = {1 << 29, 0, 0, INT32_MIN, 0};
    TEST_ASSERT_FALSE(Engine::eqSettingValid(&minA1, 1, 256));
    TEST_ASSERT_TRUE(Engine::eqBandValid(designHighpass(800.0, 0.7071)));
    TEST_ASSERT_TRUE(Engine::eqBandValid(designPeaking(6400.0, -6.0, 2.0)));

    // A rejected commit leaves the bus flat (bit-identical to no EQ).
    Engine& e = g_engine;
    e.apply(Command::noteOn(0, kSine, Engine::hzToInc(1000.0f), 0, kToneLevel, 128, kToneEnvelope));
    static int16_t ref[3000];
    e.render(ref, 3000);
    e.reset(1);
    e.apply(Command::noteOn(0, kSine, Engine::hzToInc(1000.0f), 0, kToneLevel, 128, kToneEnvelope));
    e.apply(Command::eqBand(0, flat));
    TEST_ASSERT_FALSE(e.commitEq(1, true, 100000));   // +52 dB make-up: refused
    e.render(g_buf, 3000);
    TEST_ASSERT_EQUAL_INT16_ARRAY(ref, g_buf, 3000);
}

void test_eq_extreme_gain_never_wraps() {
    // The largest accepted setting: b0 ~ 4.0 and +24 dB make-up = x64 on a
    // full-scale sine. The bus saturates and the limiter holds the peak; the
    // output keeps the input's sign (a wrap would flip it).
    Engine& e = g_engine;
    const BiquadCoefs big = {INT32_MAX, 0, 0, 0, 0};
    e.apply(Command::noteOn(0, kSine, Engine::hzToInc(500.0f), 0, kLevelUnity, 128, kToneEnvelope));
    static int16_t ref[8820];
    e.render(ref, 8820);
    e.reset(1);
    e.apply(Command::noteOn(0, kSine, Engine::hzToInc(500.0f), 0, kLevelUnity, 128, kToneEnvelope));
    e.apply(Command::eqBand(0, big));
    TEST_ASSERT_TRUE(e.commitEq(1, true, kEqMaxGainQ8));
    e.render(g_buf, 8820);
    int32_t peak = 0;
    for (int i = 0; i < 8820; ++i) {
        const int32_t a = abs((int32_t)g_buf[i]);
        if (a > peak) peak = a;
        if (abs((int32_t)ref[i]) > 2000) TEST_ASSERT_TRUE(((int32_t)ref[i] > 0) == ((int32_t)g_buf[i] > 0));
    }
    TEST_ASSERT_TRUE(peak <= 32701);
    TEST_ASSERT_EQUAL_UINT32(0, e.clipCount());
}

// ------------------------------------------- stops, queue and sequence status

// A tiny bounded queue standing in for the device's command queue.
Command g_q[4];
int g_qn = 0;
bool fakeSend(const Command& c) {
    if (g_qn >= 4) return false;
    g_q[g_qn++] = c;
    return true;
}
void drainAndRender(Engine& e, int frames) {
    for (int i = 0; i < g_qn; ++i) e.apply(g_q[i]);
    g_qn = 0;
    e.render(g_buf, frames);
}

void test_stop_overtakes_queued_play() {
    Engine& e = g_engine;
    g_qn = 0;
    ToneControl tc(fakeSend);
    TEST_ASSERT_TRUE(tc.playTone(&e, 1000.0f, 0));
    tc.stopTone(&e);                  // before the render thread ran
    drainAndRender(e, 256);
    TEST_ASSERT_FALSE(e.voiceInfo(kToneVoice).active);
    // A play after the stop still plays.
    tc.stopTone(&e);
    TEST_ASSERT_TRUE(tc.playTone(&e, 1000.0f, 0));
    drainAndRender(e, 256);
    TEST_ASSERT_TRUE(e.voiceInfo(kToneVoice).active);
}

void test_stop_survives_a_full_queue() {
    Engine& e = g_engine;
    g_qn = 0;
    ToneControl tc(fakeSend);
    TEST_ASSERT_TRUE(tc.playTone(&e, 1000.0f, 0));
    drainAndRender(e, 256);
    TEST_ASSERT_TRUE(e.voiceInfo(kToneVoice).active);
    for (int i = 0; i < 4; ++i) TEST_ASSERT_TRUE(tc.playTone(&e, 1200.0f + i, 0));
    TEST_ASSERT_FALSE(tc.playTone(&e, 2000.0f, 0));   // queue full: dropped
    tc.stopTone(&e);                                   // still lands
    drainAndRender(e, 512);
    TEST_ASSERT_FALSE(e.voiceInfo(kToneVoice).active);
    TEST_ASSERT_EQUAL_UINT32(Engine::hzToInc(1000.0f), e.voiceInfo(kToneVoice).inc);   // no queued play ran
}

void test_sequence_status_matches_the_engine() {
    Engine& e = g_engine;
    g_qn = 0;
    ToneControl tc(fakeSend);
    TEST_ASSERT_FALSE(tc.isSequencePlaying(&e));
    TEST_ASSERT_FALSE(tc.isSequencePlaying(nullptr));

    SeqStep* buf = tc.beginSequence(&e);
    TEST_ASSERT_NOT_NULL(buf);
    buf[0] = {Engine::hzToInc(1000.0f), 200, 0};
    TEST_ASSERT_TRUE(tc.commitSequence(&e, 1));
    TEST_ASSERT_TRUE(tc.isSequencePlaying(&e));    // requested, not yet applied
    drainAndRender(e, 256);
    TEST_ASSERT_TRUE(tc.isSequencePlaying(&e));    // acknowledged
    TEST_ASSERT_EQUAL_UINT32(1u, e.sequenceStatus() & 1u);

    tc.stopSequence(&e);
    TEST_ASSERT_FALSE(tc.isSequencePlaying(&e));
    drainAndRender(e, 256);
    TEST_ASSERT_EQUAL_UINT32(0u, e.sequenceStatus() & 1u);
    TEST_ASSERT_FALSE(tc.isSequencePlaying(&e));
    TEST_ASSERT_FALSE(e.voiceInfo(kToneVoice).active);   // 5 ms release, inside the block

    // A play that cannot be queued stops the previous sequence, so the status
    // (false) is what the engine does.
    buf = tc.beginSequence(&e);
    buf[0] = {Engine::hzToInc(1000.0f), 500, 0};
    TEST_ASSERT_TRUE(tc.commitSequence(&e, 1));
    drainAndRender(e, 256);
    TEST_ASSERT_TRUE(tc.isSequencePlaying(&e));
    for (int i = 0; i < 4; ++i) TEST_ASSERT_TRUE(fakeSend(Command::master(32768)));
    buf = tc.beginSequence(&e);
    buf[0] = {Engine::hzToInc(700.0f), 500, 0};
    TEST_ASSERT_FALSE(tc.commitSequence(&e, 1));
    TEST_ASSERT_FALSE(tc.isSequencePlaying(&e));
    drainAndRender(e, 256);
    TEST_ASSERT_EQUAL_UINT32(0u, e.sequenceStatus() & 1u);
    TEST_ASSERT_FALSE(tc.isSequencePlaying(&e));
    TEST_ASSERT_EQUAL_UINT32(Engine::hzToInc(1000.0f), e.voiceInfo(kToneVoice).inc);

    // A play queued before a stop never starts.
    buf = tc.beginSequence(&e);
    buf[0] = {Engine::hzToInc(1500.0f), 500, 0};
    TEST_ASSERT_TRUE(tc.commitSequence(&e, 1));
    tc.stopSequence(&e);
    drainAndRender(e, 256);
    TEST_ASSERT_EQUAL_UINT32(0u, e.sequenceStatus() & 1u);
    TEST_ASSERT_FALSE(tc.isSequencePlaying(&e));
    TEST_ASSERT_EQUAL_UINT32(Engine::hzToInc(1000.0f), e.voiceInfo(kToneVoice).inc);
}

void test_stop_stamps_do_not_alias_after_many_stops() {
    // A held tone is queued, then thousands of stops arrive before the render
    // thread drains it (a burst while it waits on the output): the old play
    // must stay cancelled, however many stops came in between.
    Engine& e = g_engine;
    g_qn = 0;
    ToneControl tc(fakeSend);
    TEST_ASSERT_TRUE(tc.playTone(&e, 1000.0f, 0));
    for (int i = 0; i < 4096 + 7; ++i) tc.stopTone(&e);
    drainAndRender(e, 256);
    TEST_ASSERT_FALSE(e.voiceInfo(kToneVoice).active);
    // Exactly 1024 and 2048 more (the old 10-bit field width) as well.
    const int bursts[] = {1024, 2048};
    for (int n : bursts) {
        TEST_ASSERT_TRUE(tc.playTone(&e, 1000.0f, 0));
        for (int i = 0; i < n; ++i) tc.stopTone(&e);
        drainAndRender(e, 256);
        TEST_ASSERT_FALSE(e.voiceInfo(kToneVoice).active);
    }
    // Sequence plays too.
    SeqStep* buf = tc.beginSequence(&e);
    buf[0] = {Engine::hzToInc(1000.0f), 500, 0};
    TEST_ASSERT_TRUE(tc.commitSequence(&e, 1));
    for (int i = 0; i < 1024; ++i) e.requestStop(kStopSequence, 0);
    drainAndRender(e, 256);
    TEST_ASSERT_EQUAL_UINT32(0u, e.sequenceStatus() & 1u);
    TEST_ASSERT_FALSE(e.voiceInfo(kToneVoice).active);
}

void test_stop_counters_compare_by_equality_at_half_range() {
    // The tone counter far past half range while the stop-all and sequence
    // counters are still 0: fresh commands must not look stale, and a first
    // stop of a dormant kind must still apply and cancel what it covers.
    Engine& e = g_engine;
    g_qn = 0;
    ToneControl tc(fakeSend);
    e.presetStopCounters(0x80000000u, 0, 0);
    tc.stopTone(&e);                               // tone counter -> 0x80000001
    TEST_ASSERT_TRUE(tc.playTone(&e, 1000.0f, 0)); // stamped after that stop
    drainAndRender(e, 256);
    TEST_ASSERT_TRUE(e.voiceInfo(kToneVoice).active);
    SeqStep* buf = tc.beginSequence(&e);
    buf[0] = {Engine::hzToInc(1200.0f), 500, 0};
    TEST_ASSERT_TRUE(tc.commitSequence(&e, 1));
    drainAndRender(e, 256);
    TEST_ASSERT_TRUE(tc.isSequencePlaying(&e));
    // First ever stop-all: applies, and cancels a play queued before it.
    Command c = Command::noteOn(5, kSaw, Engine::hzToInc(300.0f), 0, 200, 128, kToneEnvelope);
    e.stamp(c);
    fakeSend(c);
    e.requestStop(kStopAll);
    drainAndRender(e, 512);
    TEST_ASSERT_FALSE(e.voiceInfo(5).active);
    TEST_ASSERT_FALSE(e.voiceInfo(kToneVoice).active);
    TEST_ASSERT_EQUAL_UINT32(0u, e.sequenceStatus() & 1u);

    // Wrap of a counter (0xFFFFFFFF -> 0) behaves the same.
    e.reset(1);
    g_qn = 0;
    ToneControl tc2(fakeSend);
    e.presetStopCounters(0xFFFFFFFFu, 0x7FFFFFFFu, 0x80000001u);
    TEST_ASSERT_TRUE(tc2.playTone(&e, 1000.0f, 0));
    tc2.stopTone(&e);                              // wraps to 0, cancels the queued play
    drainAndRender(e, 256);
    TEST_ASSERT_FALSE(e.voiceInfo(kToneVoice).active);
    TEST_ASSERT_TRUE(tc2.playTone(&e, 1000.0f, 0));
    drainAndRender(e, 256);
    TEST_ASSERT_TRUE(e.voiceInfo(kToneVoice).active);
}

void test_pulse_duty_switch_fades_instead_of_jumping() {
    // 100 Hz pulse, 50 % duty, switched at phase 0.3 of a cycle (between the
    // 25 % and 50 % thresholds) to 25 % duty: an instant switch flips
    // +24000 to -24000.
    Engine& e = g_engine;
    const Envelope held = {44, 0, kSustainFull, 220};
    e.apply(Command::noteOn(2, kPulse, Engine::hzToInc(100.0f), 0, kLevelUnity, 128, held));
    const int at = 4410 + 132;   // 10.3 cycles
    e.render(g_buf, at);
    TEST_ASSERT_TRUE(g_buf[at - 1] > 15000);
    e.apply(Command::noteOn(2, kPulse, Engine::hzToInc(100.0f), 0, kLevelUnity, 64, held));
    e.render(g_buf + at, 400);
    // Up to the new pulse's first own edge (25 % of 441 samples after the
    // fade) no step may come near the 48000 an instant switch makes.
    const int32_t worst = maxStep(g_buf, at - 5, at + kSwitchFade + 100);
    char msg[48];
    snprintf(msg, sizeof(msg), "duty switch max step=%d", (int)worst);
    TEST_MESSAGE(msg);
    TEST_ASSERT_TRUE(worst < 2000);
}

void test_stop_all_cancels_queued_notes() {
    Engine& e = g_engine;
    g_qn = 0;
    Command c = Command::noteOn(5, kSaw, Engine::hzToInc(300.0f), 0, 200, 128, kToneEnvelope);
    e.stamp(c);
    fakeSend(c);
    e.requestStop(kStopAll);
    drainAndRender(e, 256);
    TEST_ASSERT_FALSE(e.voiceInfo(5).active);
}

// ------------------------------------------------------------------ golden

void test_golden_checksum() {
    Engine* e = new Engine(cf_audio_golden::kGoldenSeed);
    const uint32_t h = cf_audio_golden::renderGolden(*e);
    char msg[80];
    snprintf(msg, sizeof(msg), "golden checksum 0x%08X clips=%u", (unsigned)h, (unsigned)e->clipCount());
    TEST_MESSAGE(msg);
    printf("%s\n", msg);
    // Deterministic: a second engine renders the same bytes.
    Engine* f = new Engine(cf_audio_golden::kGoldenSeed);
    TEST_ASSERT_EQUAL_HEX32(h, cf_audio_golden::renderGolden(*f));
    TEST_ASSERT_EQUAL_HEX32(cf_audio_golden::kGoldenChecksum, h);
    TEST_ASSERT_EQUAL_UINT32(0, e->clipCount());
    delete e;
    delete f;
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_tone_envelope_and_gate_lengths_in_samples);
    RUN_TEST(test_tone_duration_from_milliseconds);
    RUN_TEST(test_gate_shorter_than_attack_releases_from_partial_level);
    RUN_TEST(test_decay_to_zero_sustain_frees_the_voice);
    RUN_TEST(test_sequence_step_timing_is_sample_exact);
    RUN_TEST(test_sequence_rest_silences_a_held_tone);
    RUN_TEST(test_sequence_latest_play_wins_and_stop_publishes);
    RUN_TEST(test_voice_stealing_policy);
    RUN_TEST(test_limiter_never_wraps_with_eight_full_scale_voices);
    RUN_TEST(test_quiet_tone_passes_the_limiter_untouched);
    RUN_TEST(test_master_volume_ramps_without_a_step);
    RUN_TEST(test_dc_blocker_removes_offset_and_settles_to_zero);
    RUN_TEST(test_alloff_fades_within_five_ms);
    RUN_TEST(test_eq_default_bypassed_and_commit_is_atomic);
    RUN_TEST(test_wave_and_level_switch_fades_instead_of_jumping);
    RUN_TEST(test_voice_steal_fades_the_stolen_note);
    RUN_TEST(test_eq_rejects_out_of_range_settings);
    RUN_TEST(test_eq_extreme_gain_never_wraps);
    RUN_TEST(test_stop_overtakes_queued_play);
    RUN_TEST(test_stop_survives_a_full_queue);
    RUN_TEST(test_sequence_status_matches_the_engine);
    RUN_TEST(test_stop_all_cancels_queued_notes);
    RUN_TEST(test_stop_stamps_do_not_alias_after_many_stops);
    RUN_TEST(test_pulse_duty_switch_fades_instead_of_jumping);
    RUN_TEST(test_stop_counters_compare_by_equality_at_half_range);
    RUN_TEST(test_golden_checksum);
    return UNITY_END();
}
