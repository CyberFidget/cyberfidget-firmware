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

inline int32_t satBus(int64_t v) {
    return (int32_t)(v > kBusSat ? kBusSat : (v < -kBusSat ? -kBusSat : v));
}

// Command stamps pack the three stop counters, 10 bits each.
inline uint32_t stampField(uint32_t stamp, int kind) { return (stamp >> (10 * kind)) & 0x3FFu; }

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
    for (int k = 0; k < 3; ++k) {
        stopEpoch_[k].store(0, std::memory_order_relaxed);
        stopApplied_[k] = 0;
    }
    stopSeqGen_.store(0, std::memory_order_relaxed);
}

void Engine::requestStop(StopKind kind, uint32_t seqGen) {
    if (kind == kStopSequence) stopSeqGen_.store(seqGen, std::memory_order_relaxed);
    stopEpoch_[kind].fetch_add(1, std::memory_order_release);
}

uint32_t Engine::currentStamp() const {
    uint32_t stamp = 0;
    for (int k = 0; k < 3; ++k) stamp |= (stopEpoch_[k].load(std::memory_order_acquire) & 0x3FFu) << (10 * k);
    return stamp;
}

void Engine::applyStops() {
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
}

// True for a queued command that a later stop has overtaken: a note or
// sequence start stamped before the stop that covers it.
bool Engine::stale(const Command& c) const {
    auto moved = [&](int kind) { return stampField(c.stamp, kind) != (stopApplied_[kind] & 0x3FFu); };
    switch (c.type) {
        case CmdType::NoteOn:  return moved(kStopAll) || (c.voice == kToneVoice && moved(kStopTone));
        case CmdType::SeqPlay: return moved(kStopAll) || moved(kStopSequence);
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
            noteOn(c.voice, (Wave)c.wave, c.on.inc, c.on.gate, c.on.level, c.duty, c.on.env);
            break;
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

int Engine::pickVoice() const {
    for (int i = 0; i < kVoices; ++i)
        if (voices_[i].stage == kIdle) return i;
    int best = -1;
    for (int i = 0; i < kVoices; ++i)
        if (voices_[i].stage == kRelease && (best < 0 || voices_[i].env < voices_[best].env)) best = i;
    if (best >= 0) return best;
    best = 0;
    for (int i = 1; i < kVoices; ++i)
        if ((int32_t)(voices_[i].startStamp - voices_[best].startStamp) < 0) best = i;
    return best;
}

void Engine::noteOn(uint8_t voice, Wave wave, uint32_t inc, uint32_t gateSamples,
                    uint16_t level, uint8_t duty, const Envelope& env) {
    int vi = (voice == kAnyVoice) ? pickVoice() : (int)voice;
    if (vi < 0 || vi >= kVoices) return;
    Voice& v = voices_[vi];
    if (wave > kSine) wave = kSine;
    if (level > kLevelUnity) level = kLevelUnity;
    v.startStamp = ++noteCounter_;
    if (v.stage != kIdle && (v.wave != (uint8_t)wave || v.level != level)) {
        // Switching wave or level mid-note would jump the output: fade the
        // old note out first; startPending() starts this one from silence.
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
        noteOn(kToneVoice, kSine, s.inc, toneEnd - start, kToneLevel, 128, kToneEnvelope);
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
        } else {
            const uint32_t idx = phase >> 24;
            const int32_t frac = (int32_t)((phase >> 8) & 0xFFFFu);
            const int32_t a = kSineTable[idx];
            const int32_t b = kSineTable[idx + 1];
            s = a + (((b - a) * frac) >> 16);
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

void Engine::bus(const int32_t* acc, int16_t* out, int n) {
    uint32_t clips = 0;
    for (int i = 0; i < n; ++i) {
        if (masterRampLeft_) {
            masterQ30_ += masterStep_;
            if (--masterRampLeft_ == 0) masterQ30_ = masterTargetQ30_;
        }
        int32_t y = (int32_t)(((int64_t)acc[i] * (masterQ30_ >> 15)) >> 15);

        // DC blocker: y[n] = x[n] - x[n-1] + R * y[n-1]
        const int32_t d = y - dcX_ + mulPole(dcY_);
        dcX_ = y;
        dcY_ = d;
        y = d;

        if (eqOn_) {
            for (int b = 0; b < eqBands_; ++b) {
                Biquad& q = eq_[b];
                const int64_t a = (int64_t)q.c.b0 * y + (int64_t)q.c.b1 * q.x1 + (int64_t)q.c.b2 * q.x2
                                - (int64_t)q.c.a1 * q.y1 - (int64_t)q.c.a2 * q.y2;
                const int32_t r = satBus(a >> 29);
                q.x2 = q.x1; q.x1 = y; q.y2 = q.y1; q.y1 = r;
                y = r;
            }
            y = satBus(((int64_t)y * eqGainQ8_) >> 8);
        }

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
    Command c = Command::noteOn(kToneVoice, kSine, Engine::hzToInc(hz), gate, kToneLevel, 128, kToneEnvelope);
    c.stamp = engine->currentStamp();
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
    c.stamp = engine->currentStamp();
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

}  // namespace cf_audio
