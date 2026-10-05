// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2023-2026 Dismo Industries LLC

// Emulator AudioManager: the device's audio engine (lib/AudioEngine) driven by
// the same control layer as the device (ToneControl and the shared volume
// curve), so the emulator renders the same samples as the device does before
// its speaker stage. The page pulls the sound out with wasm_audio_render();
// the render contract is in wasm/AUDIO_RENDER_CONTRACT.md.
//
// Everything here runs on one thread: the one that runs the app loop and
// calls wasm_audio_render. Commands are queued as on the device and applied
// at the start of the next render call; sound (tone lengths, sequence steps)
// advances only as samples are rendered.

#include "AudioManager.h"
#include "AudioEngine.h"
#include "SpeakerEqPresets.h"
#include "golden_script.h"
#include "tone_script.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#define AUDIO_EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define AUDIO_EXPORT
#endif

using cf_audio::Command;
using cf_audio::Engine;

namespace {

constexpr uint32_t kEngineSeed  = 1;      // as the device's engine task
constexpr int      kQueueDepth  = 64;     // commands between two render calls
constexpr int      kBufferFrames = 2048;  // most frames one wasm_audio_render call returns
constexpr uint32_t kAutoClockMaxMs = 250; // the auto clock never catches up more than this

int16_t  s_buffer[kBufferFrames];
Command  s_queue[kQueueDepth];
int      s_qHead = 0;
int      s_qCount = 0;
float    s_volume = 0.7f;          // the device's default
bool     s_volumePending = false;  // a volume change the queue could not take yet
bool     s_speakerEq = false;      // desktop speakers: EQ bypassed
bool     s_autoClock = true;       // until the page pulls samples itself
bool     s_clockStarted = false;
uint32_t s_clockLastMs = 0;
uint32_t s_clockFrac = 0;          // leftover (frames * 1000) of the auto clock

void applySpeakerEq(Engine& e) {
    if (!s_speakerEq) {
        e.commitEq(0, false, 256);
        return;
    }
    // The device's default preset, exactly as AudioManager::startEngine loads it.
    cf_audio::BiquadCoefs bands[3];
    int32_t gainQ8 = 256;
    if (!cf_audio::designSpeakerEq(kSpeakerEqPresets[kSpeakerEqDefault].eq, bands, &gainQ8)) {
        e.commitEq(0, false, 256);
        return;
    }
    for (int b = 0; b < 3; ++b) e.setEqBand(b, bands[b]);
    e.commitEq(3, true, gainQ8);
}

// Power-on state of the device's engine after AudioManager::init(): silent,
// current volume, speaker EQ as chosen.
void primeEngine(Engine& e) {
    e.reset(kEngineSeed);
    e.setMasterVolume(cf_audio::volumeToMasterQ15(s_volume));
    applySpeakerEq(e);
}

Engine& engine() {
    static Engine e(kEngineSeed);
    static bool primed = (primeEngine(e), true);
    (void)primed;
    return e;
}

// Stamped when queued, like AudioEngineTask::send, so a later stop overtakes it.
bool send(const Command& c) {
    if (s_qCount >= kQueueDepth) return false;
    Command stamped = c;
    engine().stamp(stamped);
    s_queue[(s_qHead + s_qCount) % kQueueDepth] = stamped;
    ++s_qCount;
    return true;
}

cf_audio::ToneControl s_tone(&send);

// Applies the queued commands, then renders `frames` mono samples.
void renderNow(int16_t* out, int frames) {
    Engine& e = engine();
    while (s_qCount > 0) {
        e.apply(s_queue[s_qHead]);
        s_qHead = (s_qHead + 1) % kQueueDepth;
        --s_qCount;
    }
    // A volume change the full queue refused lands now, after everything
    // queued before it (the device's AudioManager::loop() retries it the same
    // way, landing at its next block).
    if (s_volumePending) {
        e.apply(Command::master(cf_audio::volumeToMasterQ15(s_volume)));
        s_volumePending = false;
    }
    e.render(out, frames);
}

void resetAudio() {
    s_qHead = s_qCount = 0;
    s_volumePending = false;   // primeEngine applies the current volume
    primeEngine(engine());
    s_tone.forget();
}

// Without a page pulling samples, keep the engine's clock running from
// millis() and drop the samples, so tones end and sequences finish on time.
void autoClock() {
    if (!s_autoClock) return;
    const uint32_t now = (uint32_t)millis();
    if (!s_clockStarted) {
        s_clockStarted = true;
        s_clockLastMs = now;
        s_clockFrac = 0;
        return;
    }
    uint32_t elapsed = now - s_clockLastMs;
    s_clockLastMs = now;
    if (elapsed > kAutoClockMaxMs) elapsed = kAutoClockMaxMs;
    s_clockFrac += elapsed * cf_audio::kSampleRate;
    int frames = (int)(s_clockFrac / 1000);
    s_clockFrac %= 1000;
    while (frames > 0) {
        const int n = frames < kBufferFrames ? frames : kBufferFrames;
        renderNow(s_buffer, n);
        frames -= n;
    }
}

}  // namespace

// ---------------------------------------------------------------- AudioManager

AudioManager::AudioManager() {}

void AudioManager::init() {}

void AudioManager::loop() {
    autoClock();
}

void AudioManager::setVolume(float volumeLevel) {
    float vol = volumeLevel;
    if (!(vol >= 0.0f)) vol = 0.0f;   // also NaN
    if (vol > 1.0f) vol = 1.0f;
    const bool changed = cf_audio::volumeToMasterQ15(vol) != cf_audio::volumeToMasterQ15(s_volume);
    s_volume = vol;
    // Ramped over 10 ms by the engine, as on the device.
    // If the queue is full, the latest volume is kept and lands at the next
    // render (as the device's volumePending retry).
    if (changed) s_volumePending = !send(Command::master(cf_audio::volumeToMasterQ15(s_volume)));
}

void AudioManager::playTone(float frequency, int durationMs) {
    s_tone.playTone(&engine(), frequency, durationMs);   // durationMs <= 0: until stopTone
}

void AudioManager::stopTone() {
    s_tone.stopTone(&engine());
}

void AudioManager::playSequence(const ToneStep* steps, int count) {
    if (steps == nullptr || count <= 0) {
        stopSequence();
        return;
    }
    if (count > cf_audio::kMaxSeqSteps) count = cf_audio::kMaxSeqSteps;
    // Copied now, as on the device.
    cf_audio::SeqStep* buf = s_tone.beginSequence(&engine());
    if (buf == nullptr) return;
    for (int i = 0; i < count; ++i) {
        buf[i].inc = Engine::hzToInc(steps[i].freq);   // 0 = rest
        buf[i].durMs = steps[i].durationMs;
        buf[i].gapMs = steps[i].gapAfterMs;
    }
    s_tone.commitSequence(&engine(), count);
}

void AudioManager::stopSequence() {
    s_tone.stopSequence(&engine());   // also releases the tone voice
}

bool AudioManager::isSequencePlaying() const {
    return s_tone.isSequencePlaying(&engine());
}

int AudioManager::playNote(float frequency, int durationMs) {
    return s_tone.playNote(&engine(), frequency, durationMs);
}

void AudioManager::stopNote(int handle) {
    s_tone.stopNote(&engine(), handle);
}

void AudioManager::stopNotes() {
    s_tone.stopNotes(&engine());
}

// The emulator has no microphone.
void AudioManager::enableMic(bool) {}

float AudioManager::getMicVolumeDb() const {
    return -60.0f;
}

// The app ended (module host: before the next app begins). Commands it left
// queued are dropped, so none can leak into the next app or fill the queue
// against it, and every voice and the sequencer fade out over 5 ms at the
// next render. A fade, not wasm_audio_reset(): the page already holds
// rendered blocks of whatever was sounding, and a hard cut after them clicks.
void wasmAudioEndApp() {
    s_qHead = s_qCount = 0;
    // The current volume goes first in the emptied queue, so a change the
    // ended app could not queue (a mute, say) lands before the next app's
    // first sound rather than after it.
    s_volumePending = !send(Command::master(cf_audio::volumeToMasterQ15(s_volume)));
    engine().requestStop(cf_audio::kStopAll);
    s_tone.stopSequence(&engine());   // status: no sequence wanted
}

// ------------------------------------------------------------- render exports

namespace {

// Test only: the emulator's AudioManager as runToneScript sees it.
struct EmulatorAm {
    typedef AudioManager::ToneStep ToneStep;
    AudioManager& am;   // all of its state is this file's
    void setVolume(float v) { am.setVolume(v); }
    void playTone(float hz, int ms) { am.playTone(hz, ms); }
    void stopTone() { am.stopTone(); }
    void playSequence(const ToneStep* s, int n) { am.playSequence(s, n); }
    void stopSequence() { am.stopSequence(); }
    bool isSequencePlaying() const { return am.isSequencePlaying(); }
    int playNote(float hz, int ms) { return am.playNote(hz, ms); }
    void stopNote(int h) { am.stopNote(h); }
    void stopNotes() { am.stopNotes(); }
};

}  // namespace

extern "C" {

AUDIO_EXPORT int16_t* wasm_audio_buffer(void) { return s_buffer; }

AUDIO_EXPORT int wasm_audio_buffer_frames(void) { return kBufferFrames; }

AUDIO_EXPORT int wasm_audio_sample_rate(void) { return (int)cf_audio::kSampleRate; }

AUDIO_EXPORT int wasm_audio_render(int frames) {
    s_autoClock = false;   // the caller is the clock from now on
    if (frames <= 0) return 0;
    if (frames > kBufferFrames) frames = kBufferFrames;
    renderNow(s_buffer, frames);
    return frames;
}

AUDIO_EXPORT void wasm_audio_set_autoclock(int on) {
    s_autoClock = on != 0;
    s_clockStarted = false;   // no catch-up burst for the time it was off
}

AUDIO_EXPORT void wasm_audio_reset(void) { resetAudio(); }

AUDIO_EXPORT void wasm_audio_set_speaker_eq(int on) {
    s_speakerEq = on != 0;
    applySpeakerEq(engine());   // same thread as render: applied before the next sample
}

AUDIO_EXPORT int wasm_audio_active(void) {
    Engine& e = engine();
    return (s_qCount > 0 || e.activeVoices() > 0 || (e.sequenceStatus() & 1u)) ? 1 : 0;
}

// Test only (wasm/audio_parity.mjs): the engine golden script on a private
// engine. Returns its checksum; leaves the emulator's audio untouched.
AUDIO_EXPORT uint32_t wasm_audio_selftest_golden(void) {
    static Engine e(cf_audio_golden::kGoldenSeed);
    e.reset(cf_audio_golden::kGoldenSeed);
    return cf_audio_golden::renderGolden(e);
}

// Test only (wasm/audio_parity.mjs): the AudioManager tone script through the
// emulator's real AudioManager and render path, from a reset engine at the
// default volume with the speaker EQ off. Resets the emulator's audio before
// and after; the volume and EQ setting are restored.
AUDIO_EXPORT uint32_t wasm_audio_selftest_tone_script(void) {
    const float volume = s_volume;
    const bool eq = s_speakerEq;
    const bool autoClockOn = s_autoClock;
    s_volume = 0.7f;
    s_speakerEq = false;
    resetAudio();
    static AudioManager manager;
    EmulatorAm am{manager};
    const uint32_t h = cf_audio_tone_script::runToneScript(
        am, [](int16_t* out, int frames) { renderNow(out, frames); });
    s_volume = volume;
    s_speakerEq = eq;
    resetAudio();
    s_autoClock = autoClockOn;
    s_clockStarted = false;
    return h;
}

}  // extern "C"
