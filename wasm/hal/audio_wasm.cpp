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

// Sequence stubs. The WASM emulator is currently silent for sequenced tones
// (per T-002 notes, audio PCM is on the "needs adding" list). Production-
// firmware semantics are in lib/AudioManager/AudioManager.cpp.
void AudioManager::playSequence(const ToneStep* /*steps*/, int /*count*/) {
    // No-op for now. To wire this through to the JS audio path, queue the
    // sequence here and have AudioManager::loop() advance it via playTone()
    // calls — same advance algorithm as the production AudioManager.
}

void AudioManager::stopSequence() {
    // No-op for the same reason; production stops in-flight tone + clears state.
}

void AudioManager::enableMic(bool) {}

float AudioManager::getMicVolumeDb() const {
    return -60.0f;
}
