<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright (c) 2026 Dismo Industries LLC -->
# Emulator audio render contract

Every emulator build (the full emulator, single-app builds, and the
app-free module-host core `-DCF_WASM_MODULE_HOST=ON`) runs the device's audio
engine (`lib/AudioEngine`) behind the same control layer as the device
(`cf_audio::ToneControl`, `cf_audio::volumeToMasterQ15`). `playTone`,
`stopTone`, `setVolume`, `playSequence`, `stopSequence`,
`isSequencePlaying`, `playNote`, `stopNote` and `stopNotes` render the same
samples as the device does before its speaker stage: same soft-square tone
wave, envelopes, volume curve (default volume 0.7), DC blocker and limiter.
The core makes no sound by itself: the page pulls samples and plays them.
Implementation: `hal/audio_wasm.cpp`. Local example player: `preview.html`.

## Exports (C, `Module._name`)

| Export | Meaning |
|---|---|
| `int16_t* wasm_audio_buffer(void)` | Core-owned buffer the render call writes. Read it as `new Int16Array(HEAP16.buffer, ptr, frames)` right after each render (re-create the view every time: memory can grow). |
| `int wasm_audio_buffer_frames(void)` | Buffer size in frames (2048). |
| `int wasm_audio_sample_rate(void)` | 44100. Create the `AudioContext` at this rate (`new AudioContext({sampleRate: 44100})`), or resample on the page. |
| `int wasm_audio_render(int frames)` | Applies every command the app queued since the last call, then renders `min(frames, 2048)` mono int16 frames into the buffer. Returns the frames rendered (0 for `frames <= 0`). The first call hands the clock to the caller (auto clock off). |
| `void wasm_audio_reset(void)` | Silences at once and clears all engine state and queued commands (tones, notes, sequence; the volume and speaker-EQ setting are kept). Call on app restart/unload, before the next app starts. |
| `void wasm_audio_set_speaker_eq(int on)` | 0 (default): EQ bypassed, for desktop speakers. 1: the device's default speaker EQ (`SpeakerEqPresets.h`), for a future "device speaker preview". Applied before the next rendered sample. |
| `void wasm_audio_set_autoclock(int on)` | 1: the core keeps the engine's time itself from the app loop (`millis()`) and drops the samples. Default on until the first render; turn it back on whenever the page stops pulling (muted, `AudioContext` suspended or not yet allowed). |
| `int wasm_audio_active(void)` | 1 while anything sounds, is queued, or a sequence plays. Optional (indicators, idling). |
| `uint32_t wasm_audio_selftest_golden(void)` | Test only: checksum of the engine golden script on a private engine. |
| `uint32_t wasm_audio_selftest_tone_script(void)` | Test only: checksum of the AudioManager tone script through the real emulator path. Resets the emulator's audio. |

## Timing: the consumer drives time

- Commands from the app (`playTone`, `setVolume`, ...) are queued (64 deep)
  and applied at the start of the next `wasm_audio_render` call, as the device
  applies them at the start of its next block. Stops are never dropped.
- Sound advances only as samples are rendered: tone lengths, sequence steps
  and `isSequencePlaying()` follow the rendered sample count, not wall time.
- If the tab is throttled or the context is suspended, rendering stops and
  the sound pauses (it does not glitch or skip). Tell the core with
  `wasm_audio_set_autoclock(1)` if it will stay that way for long, so tones
  still end and sequences finish; the next render takes the clock back.
- Before the first render (no sound yet, autoplay policy) the auto clock
  keeps time, so apps that wait for a sequence never hang.

## Recommended use

- Block sizes 128 to 1024 frames per call. Keep about 50 to 100 ms queued in
  the page's player (`preview.html` keeps 80 ms) and refill when the player
  reports its fill level.
- Threading: call `wasm_audio_render` from the thread that runs the app loop
  (the main thread for the full emulator, the worker for the module host).
  No concurrent calls; nothing here is thread-safe. Move blocks to the
  `AudioWorklet` with `postMessage` (transfer the buffer); no
  `SharedArrayBuffer` and no COOP/COEP headers are needed.
- Convert with `sample / 32768` to float. The output is mono; let the node
  up-mix (`outputChannelCount: [1]`).
- `AudioWorklet` needs a secure context (HTTPS or localhost) and the
  `AudioContext` must be created or resumed from a user gesture.

## Volume

`AudioManager::setVolume` sets the engine master exactly as on the device
(10 ms ramp). The page's own emulator volume is a separate `GainNode` after
the worklet; it never goes into the core.

## Parity

`node wasm/audio_parity.mjs <core.js> [...]` checks a build against the
native engine test: the engine golden script (`kGoldenChecksum`, 0xF420006F)
and the AudioManager tone script (`kToneScriptChecksum`) must match
byte-for-byte; for a module-host core it also checks a tone, its auto-stop,
reset and the auto clock. The scripts live in
`test/test_audio_engine_core/` (`golden_script.h`, `tone_script.h`).
