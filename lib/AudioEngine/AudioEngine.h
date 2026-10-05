// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

// lib/AudioEngine/AudioEngine.h
//
// Platform-independent audio engine core: 8 fixed-point synth voices, a
// sample-clocked tone sequencer and a mix bus, rendered in blocks of mono
// 16-bit samples at 44.1 kHz. Pure C++ (no Arduino, FreeRTOS or IDF), so the
// same code builds for the device, the native unit tests and the emulator.
//
// Threading model
//   One thread (the "render thread") owns the Engine: it calls apply() for
//   every queued Command and then render(). Nothing else touches engine state,
//   except three explicitly cross-thread pieces:
//     - SequenceMailbox: a control thread copies tone steps in, the render
//       thread copies them out when it applies the matching SeqPlay command;
//     - sequenceStatus(), clipCount(), peakVoices(): single 32-bit atomics the
//       render thread publishes and any thread may read without tearing.
//   The queue that carries Commands between threads belongs to the platform
//   glue (FreeRTOS on the device), not to this core.
//
// Determinism
//   The per-sample path is integer only. Floating point is used only to turn
//   a frequency into a phase increment (double, then rounded) and to design
//   EQ coefficients. Given the same seed and the same commands applied before
//   the same render() calls, every platform renders bit-identical output.
//
// Signal path (per sample)
//   voices (int32 mix) -> master gain (10 ms linear ramp on change while
//   any voice sounds; immediate when all are idle)
//   -> DC blocker (~35 Hz) -> speaker EQ (biquads, bypassed by default)
//   -> soft-knee peak limiter -> saturating clamp to int16 (counted)
//   Every stage after the voice mix saturates instead of wrapping.
//
// Voice stealing and retrigger
//   A note sent to an explicit voice index always takes that voice. A note
//   sent to kAnyVoice takes, in order: the lowest-numbered idle voice; else
//   the releasing voice with the lowest level; else the voice whose note
//   started earliest. If the voice is sounding with the same wave and level,
//   the new note continues from its current level and phase (legato, no
//   click). If the wave or level differs, the old note first fades out over
//   kSwitchFade samples and the new one then starts from silence.
//
// Stops never get lost
//   requestStop() is a persistent request (an atomic counter, not a queue
//   entry): the render thread applies it at its next command or block, and
//   then discards every queued command that was stamped before the stop and
//   would have restarted what it stopped. ToneControl uses it for the tone
//   voice and the sequencer.

#ifndef CF_AUDIO_ENGINE_H
#define CF_AUDIO_ENGINE_H

#include <stdint.h>
#include <atomic>

namespace cf_audio {

constexpr uint32_t kSampleRate   = 44100;
constexpr int      kVoices       = 8;
constexpr int      kDefaultBlock = 256;        // frames per render block
constexpr int      kMaxChunk     = 256;        // render() works in chunks of at most this
constexpr uint8_t  kAnyVoice     = 0xFF;       // noteOn: pick a voice (see stealing above)
constexpr int      kMaxSeqSteps  = 128;        // longer sequences are clipped by the caller
constexpr int      kEqMaxBands   = 4;

// Envelope level: Q24, 1 << 24 = full.
constexpr int32_t  kEnvOne       = 1 << 24;
// Sustain level in Envelope: Q15, 32768 = full.
constexpr uint16_t kSustainFull  = 32768;
// Voice level: Q8, 256 = unity.
constexpr uint16_t kLevelUnity   = 256;

enum Wave : uint8_t { kPulse = 0, kTriangle = 1, kSaw = 2, kNoise = 3, kSine = 4 };

// ADSR, all times in samples. attack/decay/release 0 = instant.
struct Envelope {
    uint16_t attack;
    uint16_t decay;
    uint16_t sustain;   // Q15, kSustainFull = full level
    uint16_t release;
};

// The tone voice used by AudioManager (playTone and sequences): voice 0,
// sine, 3 ms attack, 5 ms release, full sustain. kToneLevel (230/256 = 0.898)
// keeps today's tone loudness (audio-tools' sine generator at 0.9 of full
// scale).
constexpr uint8_t  kToneVoice    = 0;
constexpr uint16_t kToneLevel    = 230;
constexpr Envelope kToneEnvelope = {132, 0, kSustainFull, 220};

// AllOff fades every voice over this many samples (5 ms).
constexpr uint16_t kFastRelease  = 220;

// A note that replaces a sounding note of another wave or level waits for the
// old one to fade out over this many samples (2 ms).
constexpr uint16_t kSwitchFade   = 88;

// Speaker-EQ limits, checked when a setting is committed: make-up gain at most
// +24 dB (Q8), and every band stable (|a2| < 1, |a1| < 1 + a2).
constexpr int32_t  kEqMaxGainQ8  = 16 * 256;

// Master volume ramp length (10 ms).
constexpr uint32_t kMasterRampSamples = 441;

// One sequence step as the engine stores it. inc 0 = rest.
struct SeqStep {
    uint32_t inc;
    uint16_t durMs;
    uint16_t gapMs;
};

// Biquad coefficients, Q29, a0 normalised to 1 (y = b0 x + b1 x1 + b2 x2 - a1 y1 - a2 y2).
struct BiquadCoefs {
    int32_t b0, b1, b2, a1, a2;
};

class Engine;
// Called on the render thread at the start of every render chunk, before any
// sample of that chunk is rendered. May call Engine's direct methods.
typedef void (*BlockHook)(Engine& engine, int frames, void* ctx);

enum class CmdType : uint8_t {
    NoteOn, NoteOff, AllOff, SeqPlay, SeqStop, Master, EqBand, EqCommit, Hook
};

// Persistent stop requests (Engine::requestStop).
enum StopKind : uint8_t { kStopTone = 0, kStopSequence = 1, kStopAll = 2 };

// A message from a control thread to the render thread. Plain data, fixed
// size, so it can travel through any copying queue.
struct Command {
    CmdType type;
    uint8_t voice;      // NoteOn/NoteOff voice index (or kAnyVoice); EqBand band index
    uint8_t wave;       // NoteOn
    uint8_t duty;       // NoteOn pulse duty, /256 (128 = 50 %)
    uint32_t stamp;     // Engine::currentStamp() when queued (0 = before any stop)
    union {
        struct { uint32_t inc; uint32_t gate; uint16_t level; Envelope env; } on;   // NoteOn
        uint32_t gen;                                                             // SeqPlay, SeqStop
        uint32_t masterQ15;                                                       // Master
        BiquadCoefs coefs;                                                        // EqBand
        struct { uint8_t bands; uint8_t enable; int32_t gainQ8; } eqSet;          // EqCommit
        struct { BlockHook fn; void* ctx; } hookFn;                               // Hook
    };

    // gateSamples 0 = hold until noteOff.
    static Command noteOn(uint8_t voice, Wave wave, uint32_t inc, uint32_t gateSamples,
                          uint16_t level, uint8_t duty, const Envelope& env);
    static Command noteOff(uint8_t voice);
    static Command allOff();
    static Command seqPlay(uint32_t gen);
    static Command seqStop(uint32_t gen);
    static Command master(uint32_t q15);   // 32768 = unity, clamped to unity
    static Command eqBand(uint8_t band, const BiquadCoefs& c);
    static Command eqCommit(uint8_t bands, bool enable, int32_t gainQ8);
    static Command hook(BlockHook fn, void* ctx);
};

// Single-producer hand-off of a tone-step list from a control thread to the
// render thread. The producer may overwrite a list the render thread has not
// taken yet (the newer one wins); it never writes while the render thread is
// copying.
class SequenceMailbox {
public:
    // Control thread. Returns the buffer to fill (kMaxSeqSteps entries), or
    // nullptr if the render thread stayed busy copying (caller drops the play).
    SeqStep* beginWrite();
    // Control thread, after a successful beginWrite.
    void commit(int count, uint32_t gen);
    // Render thread. Copies the pending list tagged `gen` into dst and returns
    // its length; -1 if no list with that tag is pending.
    int take(uint32_t gen, SeqStep* dst);

private:
    enum : uint32_t { kFree = 0, kWriting = 1, kReady = 2, kReading = 3 };
    std::atomic<uint32_t> state_{kFree};
    uint32_t gen_ = 0;
    int count_ = 0;
    SeqStep steps_[kMaxSeqSteps];
};

class Engine {
public:
    explicit Engine(uint32_t seed = 1);
    void reset(uint32_t seed = 1);   // power-on state: silent, unity master, EQ bypassed

    // ---- render thread ----
    void apply(const Command& c);
    void render(int16_t* out, int frames);   // mono

    void noteOn(uint8_t voice, Wave wave, uint32_t inc, uint32_t gateSamples,
                uint16_t level, uint8_t duty, const Envelope& env);
    void noteOff(uint8_t voice);
    void allOff();                    // fast fade of every voice + stop the sequencer
    void startSequence(uint32_t gen); // takes the mailbox list tagged gen
    void stopSequence(uint32_t gen);
    void setMasterVolume(uint32_t q15);
    void setEqBand(int band, const BiquadCoefs& c);   // staged until commitEq
    // Applies the staged bands. An out-of-range setting (see kEqMaxGainQ8)
    // turns the EQ off instead and returns false.
    bool commitEq(int bands, bool enable, int32_t gainQ8);
    void setBlockHook(BlockHook fn, void* ctx);
    int  activeVoices() const;

    // ---- any thread ----
    // Persistent stop: kStopTone releases the tone voice, kStopSequence stops
    // the sequencer and publishes `seqGen` as its status tag, kStopAll fades
    // every voice. Applied before the next command or block.
    void requestStop(StopKind kind, uint32_t seqGen = 0);
    // The tag to put in Command::stamp when queueing (control thread).
    uint32_t currentStamp() const;
    SequenceMailbox& mailbox() { return mailbox_; }
    // (gen << 1) | playing, for the last SeqPlay/SeqStop applied.
    uint32_t sequenceStatus() const { return seqStatus_.load(std::memory_order_acquire); }
    uint32_t clipCount() const { return clips_.load(std::memory_order_relaxed); }
    uint32_t peakVoices() const { return peakVoices_.load(std::memory_order_relaxed); }

    // ---- test / bench introspection (render thread) ----
    struct VoiceInfo { bool active; uint8_t stage; int32_t env; uint32_t inc; uint32_t gateLeft; };
    VoiceInfo voiceInfo(int v) const;
    int32_t masterGainQ30() const { return masterQ30_; }
    uint32_t noteCount() const { return noteCounter_; }
    void resetCounters();             // clips, peak voices

    static bool eqBandValid(const BiquadCoefs& c);
    static bool eqSettingValid(const BiquadCoefs* bands, int count, int32_t gainQ8);

    // ---- conversions (deterministic) ----
    static uint32_t hzToInc(float hz);             // 0 for hz <= 0; capped below Nyquist
    static uint32_t msToSamples(uint32_t ms);      // floor(ms * 44.1)

    enum Stage : uint8_t { kIdle = 0, kAttack = 1, kDecay = 2, kSustain = 3, kRelease = 4 };

private:
    struct Voice {
        uint32_t phase;
        uint32_t inc;
        uint32_t dutyThreshold;
        int32_t  env;         // Q24
        int32_t  envStep;     // per-sample delta in the current stage
        uint32_t stageLeft;   // samples left in attack/decay/release
        uint32_t gateLeft;    // samples until release; 0 = held
        int32_t  sustain;     // Q24
        uint32_t startStamp;
        uint16_t decay;
        uint16_t release;
        uint16_t level;
        uint16_t lfsr;
        int16_t  noiseOut;
        uint8_t  wave;
        uint8_t  stage;
        // A note waiting for this voice's switch fade to finish.
        bool     pending;
        uint8_t  pendWave;
        uint8_t  pendDuty;
        uint16_t pendLevel;
        uint32_t pendInc;
        uint32_t pendGate;
        Envelope pendEnv;
    };

    void renderVoices(int32_t* acc, int n);
    void renderVoice(Voice& v, int32_t* acc, int n);
    template <int W> void oscRun(Voice& v, int32_t* acc, int run, int32_t step);
    void startNote(Voice& v, Wave wave, uint32_t inc, uint32_t gateSamples,
                   uint16_t level, uint8_t duty, const Envelope& env);
    void startPending(Voice& v);
    bool stale(const Command& c) const;
    void applyStops();
    void enterDecay(Voice& v);
    void enterRelease(Voice& v, uint32_t samples);
    void fireSequence();
    void publishSequence();
    void bus(const int32_t* acc, int16_t* out, int n);
    int  pickVoice() const;

    Voice voices_[kVoices];
    uint32_t noteCounter_ = 0;

    // sequencer
    SequenceMailbox mailbox_;
    SeqStep  seq_[kMaxSeqSteps];
    int      seqCount_ = 0;
    int      seqIdx_ = 0;
    bool     seqPlaying_ = false;
    uint32_t seqGen_ = 0;
    uint32_t seqClock_ = 0;   // samples since the sequence started
    uint32_t seqNext_ = 0;    // clock of the next step boundary
    uint32_t seqCumMs_ = 0;   // ms from the start to that boundary
    std::atomic<uint32_t> seqStatus_{0};

    // bus
    int32_t  masterQ30_ = 0;
    int32_t  masterTargetQ30_ = 0;
    int32_t  masterStep_ = 0;
    uint32_t masterRampLeft_ = 0;
    int32_t  dcX_ = 0, dcY_ = 0;
    struct Biquad { BiquadCoefs c; int32_t x1, x2, y1, y2; };
    Biquad   eq_[kEqMaxBands];
    BiquadCoefs eqStaged_[kEqMaxBands];
    int      eqBands_ = 0;
    bool     eqOn_ = false;
    int32_t  eqGainQ8_ = 256;
    int32_t  limGainQ30_ = 1 << 30;
    std::atomic<uint32_t> clips_{0};
    std::atomic<uint32_t> peakVoices_{0};

    BlockHook hook_ = nullptr;
    void*     hookCtx_ = nullptr;

    // persistent stops: written by control threads, applied by the render thread
    std::atomic<uint32_t> stopEpoch_[3];
    std::atomic<uint32_t> stopSeqGen_{0};
    uint32_t stopApplied_[3] = {0, 0, 0};

    int32_t acc_[kMaxChunk];
};

// Control-side tone logic shared by every AudioManager build: the tone voice,
// tone sequences, and a sequence status that agrees with what the render
// thread has acknowledged. One control thread at a time. `engine` is the
// running engine or nullptr (then calls are dropped); `send` queues a command
// for the render thread and returns false when it could not.
class ToneControl {
public:
    typedef bool (*SendFn)(const Command& c);
    explicit ToneControl(SendFn send) : send_(send) {}

    bool playTone(Engine* engine, float hz, int durationMs);   // durationMs <= 0: until stopTone
    void stopTone(Engine* engine);                              // never dropped
    // Fill the returned buffer (kMaxSeqSteps entries), then commitSequence.
    // nullptr: no engine or the hand-off is busy; the call is dropped.
    SeqStep* beginSequence(Engine* engine);
    // If the play cannot be queued, the previous sequence is stopped instead,
    // so the status never claims a sequence the engine is not playing.
    bool commitSequence(Engine* engine, int count);
    void stopSequence(Engine* engine);                          // never dropped
    bool isSequencePlaying(const Engine* engine) const;
    void forget() { wanted_ = false; }   // the engine went away

private:
    SendFn send_;
    uint32_t gen_ = 0;       // tag of the last sequence play/stop issued
    bool wanted_ = false;    // whether that play/stop asked to play
};

// RBJ-cookbook biquad designs, quantised to Q29. Uses libm, so coefficients
// may differ by an LSB between platforms; the default (bypassed) bus never
// uses them.
BiquadCoefs designHighpass(double hz, double q);
BiquadCoefs designPeaking(double hz, double gainDb, double q);
int32_t     dbToGainQ8(double gainDb);

}  // namespace cf_audio

#endif  // CF_AUDIO_ENGINE_H
