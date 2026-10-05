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
    var master = (typeof Module._emulatorMasterVolume !== 'undefined' ? Module._emulatorMasterVolume : 1);
    if (Module._audioGain) {
        Module._audioGain.gain.setValueAtTime(volume * master, Module._audioCtx.currentTime);
    }
    if (Module._audioNotes) {
        for (var h in Module._audioNotes) {
            Module._audioNotes[h].gain.gain.setValueAtTime(volume * master, Module._audioCtx.currentTime);
        }
    }
});

// Notes (playNote): one oscillator per handle, alongside the tone, at most
// seven at once like the device (the oldest is replaced). Timed notes stop on
// the audio clock; every note is removed when its oscillator ends.
EM_JS(void, js_audio_note_stop, (int handle), {
    var n = Module._audioNotes && Module._audioNotes[handle];
    if (!n) return;
    delete Module._audioNotes[handle];
    try { n.osc.stop(); } catch(e) {}
});

EM_JS(void, js_audio_note_play, (int handle, float frequency, float volume, int duration_ms), {
    if (!Module._audioCtx) {
        Module._audioCtx = new (window.AudioContext || window.webkitAudioContext)();
    }
    if (!Module._audioNotes) Module._audioNotes = {};
    var held = Object.keys(Module._audioNotes).map(Number).sort(function(a, b) { return a - b; });
    while (held.length >= 7) js_audio_note_stop(held.shift());   // handles count up: lowest = oldest
    var ctx = Module._audioCtx;
    var osc = ctx.createOscillator();
    var gain = ctx.createGain();
    osc.type = 'square';
    osc.frequency.setValueAtTime(frequency, ctx.currentTime);
    var master = (typeof Module._emulatorMasterVolume !== 'undefined' ? Module._emulatorMasterVolume : 1);
    gain.gain.setValueAtTime(Math.max(0, Math.min(1, volume)) * master, ctx.currentTime);
    osc.connect(gain);
    gain.connect(ctx.destination);
    var note = { osc: osc, gain: gain };
    osc.onended = function() {
        if (Module._audioNotes && Module._audioNotes[handle] === note) delete Module._audioNotes[handle];
        try { osc.disconnect(); gain.disconnect(); } catch(e) {}
    };
    Module._audioNotes[handle] = note;
    osc.start();
    if (duration_ms > 0) osc.stop(ctx.currentTime + duration_ms / 1000);
});

EM_JS(void, js_audio_notes_stop_all, (), {
    if (!Module._audioNotes) return;
    for (var h in Module._audioNotes) js_audio_note_stop(+h);
});

#else
inline void js_audio_play_tone(float, float, int) {}
inline void js_audio_stop_tone() {}
inline void js_audio_set_volume(float) {}
inline void js_audio_note_play(int, float, float, int) {}
inline void js_audio_note_stop(int) {}
inline void js_audio_notes_stop_all() {}
#endif

static float s_volume = 0.3f;

// Tone and sequence state. The device's AudioManager keeps different private
// members (its engine task renders), so the emulator's own state lives here.
static float              currentFrequency = 0;
static bool               isPlaying = false;
static unsigned long      stopAtMillis = 0;
static const AudioManager::ToneStep* currentSequence = nullptr;
static int                currentSequenceLen = 0;
static int                currentSequenceIdx = 0;
static unsigned long      nextStepAtMs = 0;

AudioManager::AudioManager() {}

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

bool AudioManager::isSequencePlaying() const {
    return currentSequence != nullptr;
}

// Notes: one browser oscillator per handle. Handles count up from 1 and are
// never reused (after 2^31 - 1 notes playNote returns -1), so a stale handle
// never matches a newer note.
static int s_lastNoteHandle = 0;

int AudioManager::playNote(float frequency, int durationMs) {
    if (!(frequency > 0.0f)) return -1;
    if (s_lastNoteHandle >= 0x7FFFFFFF) return -1;
    ++s_lastNoteHandle;
    js_audio_note_play(s_lastNoteHandle, frequency, s_volume, durationMs);
    return s_lastNoteHandle;
}

void AudioManager::stopNote(int handle) {
    if (handle > 0) js_audio_note_stop(handle);
}

void AudioManager::stopNotes() {
    js_audio_notes_stop_all();
}

void AudioManager::enableMic(bool) {}

float AudioManager::getMicVolumeDb() const {
    return -60.0f;
}
