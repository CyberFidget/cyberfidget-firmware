// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2023-2026 Dismo Industries LLC

// WASM AudioManager implementation — uses Web Audio API for tone generation

#include "AudioManager.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>

EM_JS(void, js_audio_play_tone, (float frequency, float volume, int duration_ms), {
    if (!Module._audioCtx) {
        Module._audioCtx = new (window.AudioContext || window.webkitAudioContext)();
    }
    js_audio_stop_tone();
    Module._audioPlaying = true;

    var ctx = Module._audioCtx;
    var osc = ctx.createOscillator();
    var gain = ctx.createGain();
    osc.type = 'square';
    osc.frequency.setValueAtTime(frequency, ctx.currentTime);
    var appVol = Math.max(0, Math.min(1, volume));
    var master = (typeof Module._emulatorMasterVolume !== 'undefined' ? Module._emulatorMasterVolume : 1);
    var vol = appVol * master;
    gain.gain.setValueAtTime(vol, ctx.currentTime);
    osc.connect(gain);
    gain.connect(ctx.destination);
    osc.start();
    Module._audioOsc = osc;
    Module._audioGain = gain;
    if (duration_ms > 0) {
        var stopAt = ctx.currentTime + duration_ms / 1000;
        gain.gain.setValueAtTime(vol, ctx.currentTime);
        gain.gain.setValueAtTime(0, stopAt);
        osc.stop(stopAt);
        var id = setTimeout(function() {
            Module._audioStopTimeout = null;
            if (Module._audioOsc === osc) {
                Module._audioOsc = null;
                Module._audioGain = null;
                Module._audioPlaying = false;
            }
        }, duration_ms + 10);
        Module._audioStopTimeout = id;
    }
});

EM_JS(void, js_audio_stop_tone, (), {
    Module._audioPlaying = false;
    if (Module._audioStopTimeout) {
        clearTimeout(Module._audioStopTimeout);
        Module._audioStopTimeout = null;
    }
    if (Module._audioOsc) {
        try { Module._audioOsc.stop(); } catch(e) {}
        Module._audioOsc = null;
        Module._audioGain = null;
    }
});

EM_JS(void, js_audio_set_volume, (float volume), {
    if (Module._audioGain) {
        var master = (typeof Module._emulatorMasterVolume !== 'undefined' ? Module._emulatorMasterVolume : 1);
        Module._audioGain.gain.setValueAtTime(volume * master, Module._audioCtx.currentTime);
    }
});

#else
inline void js_audio_play_tone(float, float, int) {}
inline void js_audio_stop_tone() {}
inline void js_audio_set_volume(float) {}
#endif

static float s_volume = 0.3f;

AudioManager::AudioManager()
    : currentFrequency(0)
    , isPlaying(false)
    , stopAtMillis(0)
    , in(generator)
{}

void AudioManager::init() {}

void AudioManager::loop() {
    if (isPlaying && stopAtMillis > 0 && millis() >= stopAtMillis) {
        stopTone();
    }

    // Sequence advance — mirrors the production AudioManager so jingles
    // play in the browser emulator with the same timing they do on hardware.
    if (currentSequence != nullptr && millis() >= nextStepAtMs) {
        if (currentSequenceIdx >= currentSequenceLen) {
            currentSequence    = nullptr;
            currentSequenceLen = 0;
            currentSequenceIdx = 0;
        } else {
            const ToneStep& s = currentSequence[currentSequenceIdx];
            if (s.freq > 0.0f) {
                playTone(s.freq, s.durationMs);
            } else {
                stopTone(); // rest
            }
            nextStepAtMs = millis() + s.durationMs + s.gapAfterMs;
            currentSequenceIdx++;
        }
    }
}

void AudioManager::setVolume(float volume) {
    if (volume < 0.0f) volume = 0.0f;
    if (volume > 1.0f) volume = 1.0f;
    s_volume = volume;
    js_audio_set_volume(volume);
}

void AudioManager::playTone(float frequency, int durationMs) {
    currentFrequency = frequency;
    isPlaying = true;
    stopAtMillis = (durationMs > 0) ? millis() + durationMs : 0;
    js_audio_play_tone(frequency, s_volume, durationMs);
}

void AudioManager::stopTone() {
    isPlaying = false;
    currentFrequency = 0;
    stopAtMillis = 0;
    js_audio_stop_tone();
}

// Sequence playback — same semantics as the production AudioManager.
// Caller-provided ToneStep array must outlive playback (typical pattern is
// static const). loop() drives the per-step advance.
void AudioManager::playSequence(const ToneStep* steps, int count) {
    if (steps == nullptr || count <= 0) {
        stopSequence();
        return;
    }
    currentSequence    = steps;
    currentSequenceLen = count;
    currentSequenceIdx = 0;
    nextStepAtMs       = millis(); // first step fires on the next loop() tick
}

void AudioManager::stopSequence() {
    currentSequence    = nullptr;
    currentSequenceLen = 0;
    currentSequenceIdx = 0;
    stopTone();
}

void AudioManager::enableMic(bool) {}

float AudioManager::getMicVolumeDb() const {
    return -60.0f;
}
