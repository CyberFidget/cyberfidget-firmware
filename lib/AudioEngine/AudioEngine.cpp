// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

// lib/AudioEngine/AudioEngine.cpp - see AudioEngine.h for the model.

#include "AudioEngine.h"

#include <math.h>
#include <string.h>

namespace cf_audio {

namespace {

// round(32767 * sin(2 pi i / 256)), i = 0..256 (the last entry repeats the
// first so interpolation never wraps). A literal table, not computed at run
// time, so every platform uses identical values.
const int16_t kSineTable[257] = {
         0,    804,   1608,   2410,   3212,   4011,   4808,   5602,   6393,   7179,   7962,   8739,
      9512,  10278,  11039,  11793,  12539,  13279,  14010,  14732,  15446,  16151,  16846,  17530,
     18204,  18868,  19519,  20159,  20787,  21403,  22005,  22594,  23170,  23731,  24279,  24811,
     25329,  25832,  26319,  26790,  27245,  27683,  28105,  28510,  28898,  29268,  29621,  29956,
     30273,  30571,  30852,  31113,  31356,  31580,  31785,  31971,  32137,  32285,  32412,  32521,
     32609,  32678,  32728,  32757,  32767,  32757,  32728,  32678,  32609,  32521,  32412,  32285,
     32137,  31971,  31785,  31580,  31356,  31113,  30852,  30571,  30273,  29956,  29621,  29268,
     28898,  28510,  28105,  27683,  27245,  26790,  26319,  25832,  25329,  24811,  24279,  23731,
     23170,  22594,  22005,  21403,  20787,  20159,  19519,  18868,  18204,  17530,  16846,  16151,
     15446,  14732,  14010,  13279,  12539,  11793,  11039,  10278,   9512,   8739,   7962,   7179,
      6393,   5602,   4808,   4011,   3212,   2410,   1608,    804,      0,   -804,  -1608,  -2410,
     -3212,  -4011,  -4808,  -5602,  -6393,  -7179,  -7962,  -8739,  -9512, -10278, -11039, -11793,
    -12539, -13279, -14010, -14732, -15446, -16151, -16846, -17530, -18204, -18868, -19519, -20159,
    -20787, -21403, -22005, -22594, -23170, -23731, -24279, -24811, -25329, -25832, -26319, -26790,
    -27245, -27683, -28105, -28510, -28898, -29268, -29621, -29956, -30273, -30571, -30852, -31113,
    -31356, -31580, -31785, -31971, -32137, -32285, -32412, -32521, -32609, -32678, -32728, -32757,
    -32767, -32757, -32728, -32678, -32609, -32521, -32412, -32285, -32137, -31971, -31785, -31580,
    -31356, -31113, -30852, -30571, -30273, -29956, -29621, -29268, -28898, -28510, -28105, -27683,
    -27245, -26790, -26319, -25832, -25329, -24811, -24279, -23731, -23170, -22594, -22005, -21403,
    -20787, -20159, -19519, -18868, -18204, -17530, -16846, -16151, -15446, -14732, -14010, -13279,
    -12539, -11793, -11039, -10278,  -9512,  -8739,  -7962,  -7179,  -6393,  -5602,  -4808,  -4011,
     -3212,  -2410,  -1608,   -804,      0,
};

// Soft-square weights (Q15) for 1..4 odd harmonics: (32768 / k) / P, where P
// is the peak of sum(sin(k x) / k) over the kept k = 1, 3, 5, 7, so every
// variant peaks like the sine. With one harmonic it is exactly the sine.
const int32_t kSoftSquareWeights[4][4] = {
    {32768, 0, 0, 0},
    {34756, 11585, 0, 0},
    {35109, 11703, 7022, 0},
    {35231, 11744, 7046, 5033},
};
// Phase increment of kSoftSquareTopHz: harmonic k is kept while k * inc is
// at or below it. round(10000 * 2^32 / 44100).
constexpr uint64_t kSoftSquareTopInc = 973915487ull;

#if defined(__GNUC__)
#define CF_AUDIO_INLINE inline __attribute__((always_inline))
#else
#define CF_AUDIO_INLINE inline
#endif

CF_AUDIO_INLINE int32_t sineAt(uint32_t phase) {
    const uint32_t idx = phase >> 24;
    const int32_t frac = (int32_t)((phase >> 8) & 0xFFFFu);
    const int32_t a = kSineTable[idx];
    const int32_t b = kSineTable[idx + 1];
    return a + (((b - a) * frac) >> 16);
}

// Number of odd harmonics (1, 3, 5, 7) a soft square at `inc` keeps; at least 1.
inline uint8_t softSquareHarmonics(uint32_t inc) {
    uint8_t n = 1;
    while (n < 4 && (uint64_t)(2 * n + 1) * inc <= kSoftSquareTopInc) ++n;
    return n;
}

// Oscillator peak levels (Q15 scale). Pulse, saw and noise sit below full
// scale because their harmonics make them louder than a sine at equal peak.
constexpr int32_t kPulseAmp = 24000;
constexpr int32_t kNoiseAmp = 20000;

constexpr int32_t kOneQ30 = 1 << 30;

// DC blocker pole, R = 0.995 in Q15 (corner ~35 Hz at 44.1 kHz).
constexpr int32_t kDcPoleQ15 = 32604;

// Soft-knee peak limiter: unity below the knee; above it the target peak is
// knee + (|x| - knee) / 4, capped at the ceiling. Gain follows instantly
// down and recovers with a ~23 ms time constant (1/1024 per sample).
// The knee sits above a full-volume tone (0.9 of full scale), so ordinary
// tones pass bit-exact; it only acts when voices pile up.
constexpr int32_t kLimKnee = 30000;      // -0.8 dBFS
constexpr int32_t kLimCeiling = 32700;   // -0.02 dBFS
constexpr int kLimReleaseShift = 10;

// Bus values past the voice mix saturate here (512 x full scale) before any
// narrowing, so EQ gain can never wrap the int32 path.
constexpr int64_t kBusSat = 1 << 24;

inline int32_t clampBus(int32_t v) {
    return v > (int32_t)kBusSat ? (int32_t)kBusSat : (v < -(int32_t)kBusSat ? -(int32_t)kBusSat : v);
}

// The EQ inner loop is the bus's hottest code: let GCC optimise it for speed
// even in size-optimised builds (the arithmetic is the same either way).
#if defined(__GNUC__) && !defined(__clang__)
#define CF_AUDIO_HOT __attribute__((optimize("O2")))
#else
#define CF_AUDIO_HOT
#endif

// One multiply-accumulate of the EQ (see eqRun): v is pre-scaled by 8.
inline void eqTerm(int32_t c, int32_t v, int32_t& hi, uint32_t& lo) {
    const int64_t p = (int64_t)c * v;
    hi += (int32_t)(p >> 32);
    lo += (uint32_t)p >> 3;
}


// Multiply by the DC-blocker pole, rounding toward zero so the filter state
// always decays to exactly 0.
inline int32_t mulPole(int32_t v) {
    return v >= 0 ? (int32_t)(((int64_t)v * kDcPoleQ15) >> 15)
                  : -(int32_t)(((int64_t)(-v) * kDcPoleQ15) >> 15);
}

// Saturating double -> int32 (NaN -> 0), so a silly design never narrows out of range.
int32_t satRound(double v) {
    if (!(v == v)) return 0;
    if (v >= 2147483647.0) return INT32_MAX;
    if (v <= -2147483648.0) return INT32_MIN;
    return (int32_t)lround(v);
}

BiquadCoefs quantise(double b0, double b1, double b2, double a0, double a1, double a2) {
    const double s = (double)(1 << 29) / a0;
    BiquadCoefs c;
    c.b0 = satRound(b0 * s);
    c.b1 = satRound(b1 * s);
    c.b2 = satRound(b2 * s);
    c.a1 = satRound(a1 * s);
    c.a2 = satRound(a2 * s);
    return c;
}

}  // namespace

// ------------------------------------------------------------------ Command

Command Command::noteOn(uint8_t voice, Wave wave, uint32_t inc, uint32_t gateSamples,
                        uint16_t level, uint8_t duty, const Envelope& env) {
    Command c;
    memset(&c, 0, sizeof(c));
    c.type = CmdType::NoteOn;
    c.voice = voice;
    c.wave = (uint8_t)wave;
    c.duty = duty;
    c.on.inc = inc;
    c.on.gate = gateSamples;
    c.on.level = level;
    c.on.env = env;
    return c;
}

Command Command::noteOff(uint8_t voice) {
    Command c;
    memset(&c, 0, sizeof(c));
    c.type = CmdType::NoteOff;
    c.voice = voice;
    return c;
}

Command Command::noteOnId(uint32_t noteId, Wave wave, uint32_t inc, uint32_t gateSamples,
                          uint16_t level, uint8_t duty, const Envelope& env) {
    Command c = noteOn(kAnyNoteVoice, wave, inc, gateSamples, level, duty, env);
    c.noteId = noteId;
    return c;
}

Command Command::noteOffId(uint32_t noteId) {
    Command c;
    memset(&c, 0, sizeof(c));
    c.type = CmdType::NoteOffId;
    c.noteId = noteId;
    return c;
}

Command Command::allOff() {
    Command c;
    memset(&c, 0, sizeof(c));
    c.type = CmdType::AllOff;
    return c;
}

Command Command::seqPlay(uint32_t gen) {
    Command c;
    memset(&c, 0, sizeof(c));
    c.type = CmdType::SeqPlay;
    c.gen = gen;
    return c;
}

Command Command::seqStop(uint32_t gen) {
    Command c;
    memset(&c, 0, sizeof(c));
    c.type = CmdType::SeqStop;
    c.gen = gen;
    return c;
}

Command Command::master(uint32_t q15) {
    Command c;
    memset(&c, 0, sizeof(c));
    c.type = CmdType::Master;
    c.masterQ15 = q15;
    return c;
}

Command Command::eqBand(uint8_t band, const BiquadCoefs& coefs) {
    Command c;
    memset(&c, 0, sizeof(c));
    c.type = CmdType::EqBand;
    c.voice = band;
    c.coefs = coefs;
    return c;
}

Command Command::eqCommit(uint8_t bands, bool enable, int32_t gainQ8) {
    Command c;
    memset(&c, 0, sizeof(c));
    c.type = CmdType::EqCommit;
    c.eqSet.bands = bands;
    c.eqSet.enable = enable ? 1 : 0;
    c.eqSet.gainQ8 = gainQ8;
    return c;
}

Command Command::hook(BlockHook fn, void* ctx) {
    Command c;
    memset(&c, 0, sizeof(c));
    c.type = CmdType::Hook;
    c.hookFn.fn = fn;
    c.hookFn.ctx = ctx;
    return c;
}

// ---------------------------------------------------------- SequenceMailbox

SeqStep* SequenceMailbox::beginWrite() {
    // The render thread holds kReading only while copying (microseconds), and
    // only ever from another core: on the same core it preempts us entirely.
    for (uint32_t tries = 0; tries < 100000; ++tries) {
        uint32_t s = state_.load(std::memory_order_acquire);
        if ((s == kFree || s == kReady) &&
            state_.compare_exchange_weak(s, kWriting, std::memory_order_acq_rel)) {
            return steps_;
        }
    }
    return nullptr;
}

void SequenceMailbox::commit(int count, uint32_t gen) {
    if (count < 0) count = 0;
    if (count > kMaxSeqSteps) count = kMaxSeqSteps;
    count_ = count;
    gen_ = gen;
    state_.store(kReady, std::memory_order_release);
}

int SequenceMailbox::take(uint32_t gen, SeqStep* dst) {
    uint32_t s = kReady;
    if (!state_.compare_exchange_strong(s, kReading, std::memory_order_acq_rel)) return -1;
    if (gen_ != gen) {
        // A newer list is waiting for its own SeqPlay: leave it.
        state_.store(kReady, std::memory_order_release);
        return -1;
    }
    const int n = count_;
    memcpy(dst, steps_, sizeof(SeqStep) * (size_t)n);
    state_.store(kFree, std::memory_order_release);
    return n;
}

// ------------------------------------------------------------------- Engine

Engine::Engine(uint32_t seed) { reset(seed); }

void Engine::reset(uint32_t seed) {
    memset(voices_, 0, sizeof(voices_));
    for (int i = 0; i < kVoices; ++i) {
        uint16_t l = (uint16_t)((seed + (uint32_t)i * 0x1F35u) & 0x7FFFu);
        voices_[i].lfsr = l ? l : 0x4A5Bu;
        voices_[i].noiseOut = kNoiseAmp;
    }
    noteCounter_ = 0;
    seqCount_ = 0;
    seqIdx_ = 0;
    seqPlaying_ = false;
    seqGen_ = 0;
    seqClock_ = seqNext_ = seqCumMs_ = 0;
    seqStatus_.store(0, std::memory_order_release);
    masterQ30_ = masterTargetQ30_ = kOneQ30;
    masterStep_ = 0;
    masterRampLeft_ = 0;
    dcX_ = dcY_ = 0;
    memset(eq_, 0, sizeof(eq_));
    memset(eqStaged_, 0, sizeof(eqStaged_));
    eqBands_ = 0;
    eqOn_ = false;
    eqGainQ8_ = 256;
    limGainQ30_ = kOneQ30;
    clips_.store(0, std::memory_order_relaxed);
    peakVoices_.store(0, std::memory_order_relaxed);
    hook_ = nullptr;
    hookCtx_ = nullptr;
    for (int k = 0; k < kStopKinds; ++k) {
        stopEpoch_[k].store(0, std::memory_order_relaxed);
        stopApplied_[k] = 0;
    }
    stopSeqGen_.store(0, std::memory_order_relaxed);
    for (int i = 0; i < kReleaseSlots; ++i) {
        releaseSlots_[i].store(0, std::memory_order_relaxed);
        releasedIds_[i] = 0;
    }
    releasedNext_ = 0;
}

bool Engine::postNoteRelease(uint32_t noteId) {
    if (noteId == 0) return true;
    for (int i = 0; i < kReleaseSlots; ++i) {
        uint32_t expected = 0;
        if (releaseSlots_[i].compare_exchange_strong(expected, noteId, std::memory_order_acq_rel)) return true;
    }
    return false;
}

void Engine::requestStop(StopKind kind, uint32_t seqGen) {
    if (kind == kStopSequence) stopSeqGen_.store(seqGen, std::memory_order_relaxed);
    stopEpoch_[kind].fetch_add(1, std::memory_order_acq_rel);
}

void Engine::stamp(Command& c) const {
    c.stampAll = stopEpoch_[kStopAll].load(std::memory_order_acquire);
    c.stampKind = 0;
    if (c.type == CmdType::NoteOn && c.voice == kToneVoice) {
        c.stampKind = stopEpoch_[kStopTone].load(std::memory_order_acquire);
    } else if (c.type == CmdType::NoteOn && c.voice == kAnyNoteVoice) {
        c.stampKind = stopEpoch_[kStopNotes].load(std::memory_order_acquire);
    } else if (c.type == CmdType::SeqPlay) {
        c.stampKind = stopEpoch_[kStopSequence].load(std::memory_order_acquire);
    }
}

void Engine::presetStopCounters(uint32_t tone, uint32_t sequence, uint32_t all, uint32_t notes) {
    const uint32_t v[kStopKinds] = {tone, sequence, all, notes};
    for (int k = 0; k < kStopKinds; ++k) {
        stopEpoch_[k].store(v[k], std::memory_order_relaxed);
        stopApplied_[k] = v[k];
    }
}

void Engine::applyStops() {
    for (int i = 0; i < kReleaseSlots; ++i) {
        if (releaseSlots_[i].load(std::memory_order_relaxed) == 0) continue;
        const uint32_t id = releaseSlots_[i].exchange(0, std::memory_order_acq_rel);
        if (id == 0) continue;
        noteOffId(id);
        releasedIds_[releasedNext_] = id;   // drop a start of it still queued
        releasedNext_ = (releasedNext_ + 1) % kReleaseSlots;
    }
    const uint32_t all = stopEpoch_[kStopAll].load(std::memory_order_acquire);
    if (all != stopApplied_[kStopAll]) {
        stopApplied_[kStopAll] = all;
        allOff();
    }
    const uint32_t seq = stopEpoch_[kStopSequence].load(std::memory_order_acquire);
    if (seq != stopApplied_[kStopSequence]) {
        stopApplied_[kStopSequence] = seq;
        stopSequence(stopSeqGen_.load(std::memory_order_relaxed));
    }
    const uint32_t tone = stopEpoch_[kStopTone].load(std::memory_order_acquire);
    if (tone != stopApplied_[kStopTone]) {
        stopApplied_[kStopTone] = tone;
        noteOff(kToneVoice);
    }
    const uint32_t notes = stopEpoch_[kStopNotes].load(std::memory_order_acquire);
    if (notes != stopApplied_[kStopNotes]) {
        stopApplied_[kStopNotes] = notes;
        for (uint8_t i = 0; i < kVoices; ++i)
            if (voices_[i].noteId != 0) noteOff(i);
    }
}

// True for a queued command that a later stop has overtaken: a note or
// sequence start stamped before the stop that covers it.
bool Engine::stale(const Command& c) const {
    // applyStops() ran just before, so stopApplied_ holds every stop requested
    // before this command was queued: any change since its stamp cancels it.
    const bool all = c.stampAll != stopApplied_[kStopAll];
    switch (c.type) {
        case CmdType::NoteOn:
            return all || (c.voice == kToneVoice && c.stampKind != stopApplied_[kStopTone]) ||
                   (c.voice == kAnyNoteVoice && c.stampKind != stopApplied_[kStopNotes]);
        case CmdType::SeqPlay: return all || c.stampKind != stopApplied_[kStopSequence];
        default:               return false;
    }
}

uint32_t Engine::hzToInc(float hz) {
    if (!(hz > 0.0f)) return 0;
    const double inc = (double)hz * 4294967296.0 / (double)kSampleRate;
    if (inc >= 2147483647.0) return 0x7FFFFFFFu;   // at or above Nyquist
    return (uint32_t)llround(inc);
}

uint32_t Engine::msToSamples(uint32_t ms) {
    return (uint32_t)(((uint64_t)ms * 441u) / 10u);
}

void Engine::apply(const Command& c) {
    applyStops();          // a stop requested before this command came first
    if (stale(c)) return;  // sent before a stop that cancels it
    switch (c.type) {
        case CmdType::NoteOn:
            noteOn(c.voice, (Wave)c.wave, c.on.inc, c.on.gate, c.on.level, c.duty, c.on.env, c.noteId);
            break;
        case CmdType::NoteOffId: noteOffId(c.noteId); break;
        case CmdType::NoteOff:  noteOff(c.voice); break;
        case CmdType::AllOff:   allOff(); break;
        case CmdType::SeqPlay:  startSequence(c.gen); break;
        case CmdType::SeqStop:  stopSequence(c.gen); break;
        case CmdType::Master:   setMasterVolume(c.masterQ15); break;
        case CmdType::EqBand:   setEqBand(c.voice, c.coefs); break;
        case CmdType::EqCommit: commitEq(c.eqSet.bands, c.eqSet.enable != 0, c.eqSet.gainQ8); break;
        case CmdType::Hook:     setBlockHook(c.hookFn.fn, c.hookFn.ctx); break;
    }
}

int Engine::pickVoice(bool skipToneVoice) const {
    const int first = (skipToneVoice && kToneVoice == 0) ? 1 : 0;
    for (int i = first; i < kVoices; ++i)
        if (voices_[i].stage == kIdle) return i;
    int best = -1;
    for (int i = first; i < kVoices; ++i)
        if (voices_[i].stage == kRelease && (best < 0 || voices_[i].env < voices_[best].env)) best = i;
    if (best >= 0) return best;
    best = first;
    for (int i = first + 1; i < kVoices; ++i)
        if ((int32_t)(voices_[i].startStamp - voices_[best].startStamp) < 0) best = i;
    return best;
}

void Engine::noteOn(uint8_t voice, Wave wave, uint32_t inc, uint32_t gateSamples,
                    uint16_t level, uint8_t duty, const Envelope& env, uint32_t noteId) {
    int vi = (voice == kAnyVoice) ? pickVoice(false) : (voice == kAnyNoteVoice) ? pickVoice(true) : (int)voice;
    if (vi < 0 || vi >= kVoices) return;
    if (noteId != 0) {
        for (int i = 0; i < kReleaseSlots; ++i)
            if (releasedIds_[i] == noteId) return;   // released before it could start
    }
    Voice& v = voices_[vi];
    v.noteId = noteId;   // a new note owns the voice (0 for untagged notes)
    if (wave > kSoftSquare) wave = kSine;
    if (level > kLevelUnity) level = kLevelUnity;
    v.startStamp = ++noteCounter_;
    const bool dutyChange = wave == kPulse && v.dutyThreshold != ((uint32_t)duty << 24);
    const bool harmonicsChange = wave == kSoftSquare && v.harmonics != softSquareHarmonics(inc);
    if (v.stage != kIdle && (v.wave != (uint8_t)wave || v.level != level || dutyChange || harmonicsChange)) {
        // Switching wave, level, pulse duty or soft-square harmonics mid-note
        // would jump the output: fade the old note out first; startPending()
        // starts this one from silence.
        v.pending = true;
        v.pendWave = (uint8_t)wave;
        v.pendDuty = duty;
        v.pendLevel = level;
        v.pendInc = inc;
        v.pendGate = gateSamples;
        v.pendEnv = env;
        if (!(v.stage == kRelease && v.stageLeft <= kSwitchFade)) enterRelease(v, kSwitchFade);
        if (v.stage == kIdle) startPending(v);
        return;
    }
    v.pending = false;
    startNote(v, wave, inc, gateSamples, level, duty, env);
}

void Engine::startPending(Voice& v) {
    v.pending = false;
    startNote(v, (Wave)v.pendWave, v.pendInc, v.pendGate, v.pendLevel, v.pendDuty, v.pendEnv);
}

void Engine::startNote(Voice& v, Wave wave, uint32_t inc, uint32_t gateSamples,
                       uint16_t level, uint8_t duty, const Envelope& env) {
    if (v.stage == kIdle) {
        v.phase = 0;
        v.env = 0;
    }
    v.wave = (uint8_t)wave;
    v.inc = inc;
    v.harmonics = softSquareHarmonics(inc);
    v.dutyThreshold = (uint32_t)duty << 24;
    v.level = level;
    v.sustain = (int32_t)(((int64_t)(env.sustain > kSustainFull ? kSustainFull : env.sustain) * kEnvOne) >> 15);
    v.decay = env.decay;
    v.release = env.release;
    v.gateLeft = gateSamples;
    if (env.attack == 0) {
        v.env = kEnvOne;
        enterDecay(v);
    } else {
        // Attack lasts exactly env.attack samples from wherever the level is.
        v.stage = kAttack;
        v.stageLeft = env.attack;
        v.envStep = (kEnvOne - v.env) / (int32_t)env.attack;
    }
}

void Engine::enterDecay(Voice& v) {
    if (v.decay == 0 || v.sustain >= kEnvOne) {
        v.env = v.sustain;
        v.stage = kSustain;
        v.envStep = 0;
        v.stageLeft = 0;
    } else {
        v.stage = kDecay;
        v.stageLeft = v.decay;
        v.envStep = -((kEnvOne - v.sustain) / (int32_t)v.decay);
    }
    if (v.stage == kSustain && v.env == 0) v.stage = kIdle;   // percussive note finished
}

void Engine::enterRelease(Voice& v, uint32_t samples) {
    if (v.stage == kIdle) return;
    if (samples == 0 || v.env == 0) {
        v.env = 0;
        v.stage = kIdle;
        return;
    }
    v.stage = kRelease;
    v.stageLeft = samples;
    v.envStep = -(v.env / (int32_t)samples);
    v.gateLeft = 0;
}

void Engine::noteOff(uint8_t voice) {
    if (voice >= kVoices) return;
    Voice& v = voices_[voice];
    v.pending = false;   // a note waiting on a switch fade never starts
    if (v.stage != kIdle && v.stage != kRelease) enterRelease(v, v.release);
}

void Engine::noteOffId(uint32_t noteId) {
    if (noteId == 0) return;
    for (uint8_t i = 0; i < kVoices; ++i)
        if (voices_[i].noteId == noteId) noteOff(i);   // stolen voices carry another id
}

void Engine::allOff() {
    for (auto& v : voices_) {
        v.pending = false;
        if (v.stage == kIdle) continue;
        if (v.stage == kRelease && v.stageLeft <= kFastRelease) continue;
        enterRelease(v, kFastRelease);
    }
    if (seqPlaying_) {
        seqPlaying_ = false;
        publishSequence();
    }
}

void Engine::publishSequence() {
    seqStatus_.store(((seqGen_ & 0x7FFFFFFFu) << 1) | (seqPlaying_ ? 1u : 0u), std::memory_order_release);
}

void Engine::startSequence(uint32_t gen) {
    const int n = mailbox_.take(gen, seq_);
    if (n < 0) return;   // superseded by a newer list (its own SeqPlay follows)
    seqGen_ = gen;
    seqCount_ = n;
    seqIdx_ = 0;
    seqClock_ = 0;
    seqNext_ = 0;
    seqCumMs_ = 0;
    seqPlaying_ = n > 0;
    publishSequence();
}

void Engine::stopSequence(uint32_t gen) {
    seqGen_ = gen;
    seqPlaying_ = false;
    publishSequence();
}

// One step boundary: start step seqIdx_ (or end the sequence). Step k starts
// at floor(44.1 * sum of earlier (dur + gap) ms), so rounding never drifts.
void Engine::fireSequence() {
    if (seqIdx_ >= seqCount_) {
        seqPlaying_ = false;
        publishSequence();
        return;
    }
    const SeqStep& s = seq_[seqIdx_++];
    const uint32_t start = seqNext_;
    const uint32_t toneEnd = msToSamples(seqCumMs_ + s.durMs);
    seqCumMs_ += (uint32_t)s.durMs + s.gapMs;
    seqNext_ = msToSamples(seqCumMs_);
    if (s.inc > 0 && toneEnd > start) {
        noteOn(kToneVoice, kToneWave, s.inc, toneEnd - start, kToneLevel, 128, kToneEnvelope);
    } else {
        noteOff(kToneVoice);   // rest (or a zero-length step)
    }
}

void Engine::setMasterVolume(uint32_t q15) {
    if (q15 > 32768u) q15 = 32768u;
    masterTargetQ30_ = (int32_t)(q15 << 15);
    const int32_t diff = masterTargetQ30_ - masterQ30_;
    if (diff == 0 || activeVoices() == 0) {
        // Nothing is sounding, so there is nothing to ramp: the next note
        // starts at the new volume.
        masterQ30_ = masterTargetQ30_;
        masterRampLeft_ = 0;
        return;
    }
    masterStep_ = diff / (int32_t)kMasterRampSamples;
    masterRampLeft_ = kMasterRampSamples;
}

void Engine::setEqBand(int band, const BiquadCoefs& c) {
    if (band >= 0 && band < kEqMaxBands) eqStaged_[band] = c;
}

bool Engine::eqBandValid(const BiquadCoefs& c) {
    // Stability triangle in Q29: |a2| < 1 and |a1| < 1 + a2.
    const int64_t one = (int64_t)1 << 29;
    const int64_t a1 = c.a1 < 0 ? -(int64_t)c.a1 : (int64_t)c.a1;
    const int64_t a2 = c.a2;
    return a2 < one && a2 > -one && a1 < one + a2;
}

bool Engine::eqSettingValid(const BiquadCoefs* bands, int count, int32_t gainQ8) {
    if (count < 0 || count > kEqMaxBands || gainQ8 < 0 || gainQ8 > kEqMaxGainQ8) return false;
    for (int b = 0; b < count; ++b)
        if (!eqBandValid(bands[b])) return false;
    return true;
}

bool Engine::commitEq(int bands, bool enable, int32_t gainQ8) {
    for (int b = 0; b < kEqMaxBands; ++b) {
        eq_[b].c = eqStaged_[b];
        eq_[b].x1 = eq_[b].x2 = eq_[b].y1 = eq_[b].y2 = 0;
    }
    if (!eqSettingValid(eqStaged_, bands, gainQ8)) {
        eqBands_ = 0;
        eqGainQ8_ = 256;
        eqOn_ = false;   // out of range: stay flat rather than risk overload
        return false;
    }
    eqBands_ = bands;
    eqGainQ8_ = gainQ8;
    eqOn_ = enable && bands > 0;
    return true;
}

void Engine::setBlockHook(BlockHook fn, void* ctx) {
    hook_ = fn;
    hookCtx_ = ctx;
}

int Engine::activeVoices() const {
    int n = 0;
    for (const auto& v : voices_) n += v.stage != kIdle;
    return n;
}

Engine::VoiceInfo Engine::voiceInfo(int i) const {
    VoiceInfo info = {false, 0, 0, 0, 0};
    if (i < 0 || i >= kVoices) return info;
    const Voice& v = voices_[i];
    info.active = v.stage != kIdle;
    info.stage = v.stage;
    info.env = v.env;
    info.inc = v.inc;
    info.gateLeft = v.gateLeft;
    return info;
}

void Engine::resetCounters() {
    clips_.store(0, std::memory_order_relaxed);
    peakVoices_.store(0, std::memory_order_relaxed);
}

// ------------------------------------------------------------------- render

// `run` samples of one voice with a constant envelope slope. The envelope is
// advanced before each sample; stage changes happen between runs.
template <int W>
void Engine::oscRun(Voice& v, int32_t* acc, int run, int32_t step) {
    uint32_t phase = v.phase;
    const uint32_t inc = v.inc;
    int32_t env = v.env;
    const int32_t level = v.level;
    for (int i = 0; i < run; ++i) {
        int32_t s;
        if (W == kPulse) {
            s = (phase < v.dutyThreshold) ? kPulseAmp : -kPulseAmp;
        } else if (W == kTriangle) {
            // Offset a quarter cycle so the wave starts at a rising zero crossing.
            const int32_t p = (int32_t)((phase + 0x40000000u) >> 16);
            s = ((p < 32768) ? p : 65535 - p) * 2 - 32767;
        } else if (W == kSaw) {
            // Offset half a cycle so the wave starts at zero.
            s = (int32_t)((phase + 0x80000000u) >> 16) - 32768;
            s = (s * 3) >> 2;
        } else if (W == kNoise) {
            const uint32_t before = phase;
            if (phase + inc < before) {   // clock the LFSR once per phase wrap
                const uint16_t bit = (uint16_t)((v.lfsr ^ (v.lfsr >> 1)) & 1u);
                v.lfsr = (uint16_t)((v.lfsr >> 1) | (bit << 14));
                v.noiseOut = (v.lfsr & 1u) ? kNoiseAmp : -kNoiseAmp;
            }
            s = v.noiseOut;
        } else if (W >= kSoftSquare) {
            // Soft square with H = W - kSoftSquare + 1 odd harmonics (template
            // parameter, so the sum is unrolled). Harmonic k reads the sine at
            // k x phase, which wraps with the cycle.
            constexpr int H = W - kSoftSquare + 1;
            const int32_t* w = kSoftSquareWeights[H - 1];
            int32_t sum = sineAt(phase) * w[0];
            if (H > 1) sum += sineAt(phase * 3u) * w[1];
            if (H > 2) sum += sineAt(phase * 5u) * w[2];
            if (H > 3) sum += sineAt(phase * 7u) * w[3];
            s = sum >> 15;   // |sum| < 32767 * 59054 < 2^31
        } else {
            s = sineAt(phase);
        }
        phase += inc;
        env += step;
        acc[i] += (((s * (env >> 9)) >> 15) * level) >> 8;
    }
    v.phase = phase;
    v.env = env;
}

void Engine::renderVoice(Voice& v, int32_t* acc, int n) {
    while (n > 0 && v.stage != kIdle) {
        int run = n;
        const bool timed = v.stage == kAttack || v.stage == kDecay || v.stage == kRelease;
        if (timed && v.stageLeft < (uint32_t)run) run = (int)v.stageLeft;
        const bool gated = v.gateLeft > 0 && v.stage != kRelease;
        if (gated && v.gateLeft < (uint32_t)run) run = (int)v.gateLeft;
        const int32_t step = (v.stage == kSustain) ? 0 : v.envStep;
        switch (v.wave) {
            case kPulse:    oscRun<kPulse>(v, acc, run, step); break;
            case kTriangle: oscRun<kTriangle>(v, acc, run, step); break;
            case kSaw:      oscRun<kSaw>(v, acc, run, step); break;
            case kNoise:    oscRun<kNoise>(v, acc, run, step); break;
            case kSoftSquare:
                // One instance per harmonic count (template ids kSoftSquare..+3).
                switch (v.harmonics) {
                    case 1:  oscRun<kSoftSquare>(v, acc, run, step); break;
                    case 2:  oscRun<kSoftSquare + 1>(v, acc, run, step); break;
                    case 3:  oscRun<kSoftSquare + 2>(v, acc, run, step); break;
                    default: oscRun<kSoftSquare + 3>(v, acc, run, step); break;
                }
                break;
            default:        oscRun<kSine>(v, acc, run, step); break;
        }
        acc += run;
        n -= run;
        if (timed) {
            v.stageLeft -= (uint32_t)run;
            if (v.stageLeft == 0) {
                if (v.stage == kAttack) {
                    v.env = kEnvOne;
                    enterDecay(v);
                } else if (v.stage == kDecay) {
                    v.env = v.sustain;
                    v.stage = kSustain;
                    v.envStep = 0;
                    if (v.env == 0) v.stage = kIdle;
                } else {
                    v.env = 0;
                    v.stage = kIdle;
                }
            }
        }
        if (gated) {
            v.gateLeft -= (uint32_t)run;
            if (v.gateLeft == 0 && v.stage != kIdle && v.stage != kRelease) enterRelease(v, v.release);
        }
        if (v.stage == kIdle && v.pending) startPending(v);   // switch fade done
    }
}

void Engine::renderVoices(int32_t* acc, int n) {
    for (auto& v : voices_)
        if (v.stage != kIdle) renderVoice(v, acc, n);
}

// One EQ band over a block, in place, with its coefficients and state in
// registers. Bit-exact with the plain form
//     y = clamp((b0 x + b1 x1 + b2 x2 - a1 y1 - a2 y2) >> 29)   (int64 sum)
// but without 64-bit additions: every input is pre-scaled by 8 (|x| <= 2^24,
// so 8x fits 32 bits). Then each product's high word is floor(p / 2^29) and
// its low word a multiple of 8, and the exact floor of the whole sum is
//     sum(high words) + (sum(low words / 8) >> 29),
// where the second sum stays below 2^32 (at most five terms below 2^29).
// Two exact shortcuts take one multiply-accumulate out (integer identities,
// so the result is unchanged); the designs produce them every time:
//   kEqPeak  b1 == a1 (peaking):   b1 x1 - a1 y1 = b1 (x1 - y1)
//   kEqEdge  b0 == b2 (high-pass): b0 x + b2 x2 = b0 (x + x2)
// (|x1 - y1|, |x + x2| <= 2^25, so the scaled sums still fit 32 bits.)
enum : int { kEqGeneral = 0, kEqPeak = 1, kEqEdge = 2 };

template <int Form>
CF_AUDIO_HOT void Engine::eqRun(Biquad& q, int32_t* buf, int n) {
    const int32_t b0 = q.c.b0, b1 = q.c.b1, b2 = q.c.b2;
    const int32_t na1 = -q.c.a1, na2 = -q.c.a2;   // |a1| < 2, |a2| < 1 (validated): exact
    int32_t x1 = q.x1 * 8, x2 = q.x2 * 8, y1 = q.y1 * 8, y2 = q.y2 * 8;
    for (int32_t* const end = buf + n; buf != end; ++buf) {
        const int32_t x = *buf * 8;
        int32_t hi = 0;
        uint32_t lo = 0;
        if (Form == kEqEdge) {
            eqTerm(b0, x + x2, hi, lo);
        } else {
            eqTerm(b0, x, hi, lo);
            eqTerm(b2, x2, hi, lo);
        }
        if (Form == kEqPeak) {
            eqTerm(b1, x1 - y1, hi, lo);
        } else {
            eqTerm(b1, x1, hi, lo);
            eqTerm(na1, y1, hi, lo);
        }
        eqTerm(na2, y2, hi, lo);
        const int32_t y = clampBus(hi + (int32_t)(lo >> 29));
        *buf = y;
        x2 = x1;
        x1 = x;
        y2 = y1;
        y1 = y * 8;
    }
    q.x1 = x1 / 8; q.x2 = x2 / 8; q.y1 = y1 / 8; q.y2 = y2 / 8;   // exact: multiples of 8
}

// The bus runs stage by stage over the block. Every stage depends only on its
// own state and its input sample, so this is the same arithmetic as one
// sample at a time through all stages.
void Engine::bus(int32_t* acc, int16_t* out, int n) {
    for (int i = 0; i < n; ++i) {
        if (masterRampLeft_) {
            masterQ30_ += masterStep_;
            if (--masterRampLeft_ == 0) masterQ30_ = masterTargetQ30_;
        }
        const int32_t x = (int32_t)(((int64_t)acc[i] * (masterQ30_ >> 15)) >> 15);

        // DC blocker: y[n] = x[n] - x[n-1] + R * y[n-1]
        const int32_t d = x - dcX_ + mulPole(dcY_);
        dcX_ = x;
        dcY_ = d;
        acc[i] = d;   // |d| <= 2 x 8 voices full scale, far below kBusSat
    }

    const bool eq = eqOn_;
    if (eq) {
        for (int b = 0; b < eqBands_; ++b) {
            Biquad& q = eq_[b];
            if (q.c.b1 == q.c.a1) eqRun<kEqPeak>(q, acc, n);
            else if (q.c.b0 == q.c.b2) eqRun<kEqEdge>(q, acc, n);
            else eqRun<kEqGeneral>(q, acc, n);
        }
    }

    const int32_t eqGain = eqGainQ8_;
    uint32_t clips = 0;
    for (int i = 0; i < n; ++i) {
        int32_t y = acc[i];
        // EQ make-up gain: |y| <= 2^24 and gain <= 16 x 256, so the product
        // >> 8 fits 32 bits.
        if (eq) y = clampBus((int32_t)(((int64_t)y * eqGain) >> 8));
        const int32_t ax = y < 0 ? -y : y;   // |y| <= kBusSat here
        if (limGainQ30_ != kOneQ30 || ax > kLimKnee) {
            if (limGainQ30_ != kOneQ30) {
                const int32_t gap = kOneQ30 - limGainQ30_;
                const int32_t up = gap >> kLimReleaseShift;
                limGainQ30_ += up ? up : gap;
            }
            if (ax > kLimKnee) {
                int32_t peak = kLimKnee + ((ax - kLimKnee) >> 2);
                if (peak > kLimCeiling) peak = kLimCeiling;
                const int32_t target = (int32_t)(((uint32_t)peak << 15) / (uint32_t)ax) << 15;
                if (target < limGainQ30_) limGainQ30_ = target;
            }
            y = (int32_t)(((int64_t)y * limGainQ30_) >> 30);
        }

        if (y > 32767) { y = 32767; ++clips; }
        else if (y < -32768) { y = -32768; ++clips; }
        out[i] = (int16_t)y;
    }
    if (clips) clips_.store(clips_.load(std::memory_order_relaxed) + clips, std::memory_order_relaxed);
}

void Engine::render(int16_t* out, int frames) {
    while (frames > 0) {
        const int n = frames < kMaxChunk ? frames : kMaxChunk;
        applyStops();
        if (hook_) hook_(*this, n, hookCtx_);
        memset(acc_, 0, sizeof(int32_t) * (size_t)n);
        int pos = 0;
        for (;;) {
            while (seqPlaying_ && seqClock_ == seqNext_) fireSequence();
            if (pos >= n) break;
            int seg = n - pos;
            if (seqPlaying_ && seqNext_ - seqClock_ < (uint32_t)seg) seg = (int)(seqNext_ - seqClock_);
            renderVoices(acc_ + pos, seg);
            pos += seg;
            if (seqPlaying_) seqClock_ += (uint32_t)seg;
        }
        const uint32_t active = (uint32_t)activeVoices();
        if (active > peakVoices_.load(std::memory_order_relaxed)) peakVoices_.store(active, std::memory_order_relaxed);
        bus(acc_, out, n);
        out += n;
        frames -= n;
    }
}

// -------------------------------------------------------------- ToneControl

namespace {
constexpr uint32_t kGenMask = 0x7FFFFFFFu;   // sequence tags are published in 31 bits
}

bool ToneControl::playTone(Engine* engine, float hz, int durationMs) {
    if (engine == nullptr) return false;
    if (!(hz > 0.0f)) {
        stopTone(engine);
        return true;
    }
    const uint32_t gate = durationMs > 0 ? Engine::msToSamples((uint32_t)durationMs) : 0;
    Command c = Command::noteOn(kToneVoice, kToneWave, Engine::hzToInc(hz), gate, kToneLevel, 128, kToneEnvelope);
    engine->stamp(c);
    return send_(c);
}

void ToneControl::stopTone(Engine* engine) {
    if (engine) engine->requestStop(kStopTone);
}

SeqStep* ToneControl::beginSequence(Engine* engine) {
    return engine ? engine->mailbox().beginWrite() : nullptr;
}

bool ToneControl::commitSequence(Engine* engine, int count) {
    if (engine == nullptr) return false;
    gen_ = (gen_ + 1) & kGenMask;
    engine->mailbox().commit(count, gen_);
    Command c = Command::seqPlay(gen_);
    engine->stamp(c);
    if (send_(c)) {
        wanted_ = true;
        return true;
    }
    // The newest call wins: if it cannot start, what played before stops.
    stopSequence(engine);
    return false;
}

void ToneControl::stopSequence(Engine* engine) {
    gen_ = (gen_ + 1) & kGenMask;
    wanted_ = false;
    if (engine) {
        engine->requestStop(kStopSequence, gen_);
        engine->requestStop(kStopTone);
    }
}

bool ToneControl::isSequencePlaying(const Engine* engine) const {
    if (engine == nullptr) return false;
    const uint32_t status = engine->sequenceStatus();
    // Until the render thread has acknowledged the latest play/stop, report
    // what it asked for: a stop is persistent, and a queued play always lands
    // unless a later stop (which changes the tag again) cancels it.
    if ((status >> 1) != gen_) return wanted_;
    return (status & 1u) != 0;
}

int ToneControl::playNote(Engine* engine, float hz, int durationMs) {
    if (engine == nullptr || !(hz > 0.0f)) return -1;
    if (noteSeq_ >= kLastNoteId) return -1;   // ids are never reused
    ++noteSeq_;
    const uint32_t gate = durationMs > 0 ? Engine::msToSamples((uint32_t)durationMs) : 0;
    Command c = Command::noteOnId(noteSeq_, kToneWave, Engine::hzToInc(hz), gate, kToneLevel, 128, kToneEnvelope);
    engine->stamp(c);
    return send_(c) ? (int)noteSeq_ : -1;
}

void ToneControl::stopNote(Engine* engine, int handle) {
    if (engine == nullptr || handle <= 0) return;
    Command c = Command::noteOffId((uint32_t)handle);
    engine->stamp(c);
    if (send_(c)) return;
    // Queue full: an id-targeted release that cannot be lost. Only if every
    // release slot is busy too, release all notes rather than leave one stuck.
    if (!engine->postNoteRelease((uint32_t)handle)) engine->requestStop(kStopNotes);
}

void ToneControl::stopNotes(Engine* engine) {
    if (engine) engine->requestStop(kStopNotes);
}

// ------------------------------------------------------------- EQ design

BiquadCoefs designHighpass(double hz, double q) {
    const double w = 2.0 * M_PI * hz / (double)kSampleRate;
    const double c = cos(w), al = sin(w) / (2.0 * q);
    return quantise((1 + c) / 2, -(1 + c), (1 + c) / 2, 1 + al, -2 * c, 1 - al);
}

BiquadCoefs designPeaking(double hz, double gainDb, double q) {
    const double A = pow(10.0, gainDb / 40.0), w = 2.0 * M_PI * hz / (double)kSampleRate;
    const double c = cos(w), al = sin(w) / (2.0 * q);
    return quantise(1 + al * A, -2 * c, 1 - al * A, 1 + al / A, -2 * c, 1 - al / A);
}

int32_t dbToGainQ8(double gainDb) {
    return satRound(256.0 * pow(10.0, gainDb / 20.0));   // validated at commit
}

namespace {
bool inRange(float x, float lo, float hi) { return x >= lo && x <= hi; }   // false for NaN
}

bool designSpeakerEq(const SpeakerEq& eq, BiquadCoefs bands[3], int32_t* gainQ8) {
    if (!inRange(eq.hpfHz, 20, 20000) || !inRange(eq.p1Hz, 20, 20000) || !inRange(eq.p2Hz, 20, 20000) ||
        !inRange(eq.p1Q, 0.1f, 20) || !inRange(eq.p2Q, 0.1f, 20) ||
        !inRange(eq.p1Db, -24, 24) || !inRange(eq.p2Db, -24, 24) || !inRange(eq.gainDb, -24, 24)) {
        return false;
    }
    bands[0] = designHighpass(eq.hpfHz, 0.7071);
    bands[1] = designPeaking(eq.p1Hz, eq.p1Db, eq.p1Q);
    bands[2] = designPeaking(eq.p2Hz, eq.p2Db, eq.p2Q);
    *gainQ8 = dbToGainQ8(eq.gainDb);
    return Engine::eqSettingValid(bands, 3, *gainQ8);
}

}  // namespace cf_audio
