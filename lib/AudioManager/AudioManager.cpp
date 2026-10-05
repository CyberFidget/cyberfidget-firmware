// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

// lib/AudioManager/AudioManager.cpp
#include "AudioManager.h"
#include "AudioEngineTask.h"
#include "globals.h"
#include <math.h>

using cf_audio::Command;
using cf_audio::Engine;

namespace {

// setVolume() keeps the curve the old audio-tools volume stream applied
// (its default "simulated audio pot"): the value rounded to 0.01, then
// 0..0.5 -> 0..0.1 and 0.5..1 -> 0.1..1, linear in each half. Same loudness
// at every setting as before. Returns the engine's master gain, Q15.
uint32_t masterForVolume(float v) {
    const int32_t pct = (int32_t)(v * 100.0f + 0.5f);   // v is already 0..1
    const int32_t num = (pct <= 50) ? pct : 9 * pct - 400;   // factor = num / 500
    return (uint32_t)((num * 32768 + 250) / 500);
}

// Tone voice, sequences and their status (shared engine-core logic). Stops go
// through the engine's persistent stop counters, never the bounded queue.
cf_audio::ToneControl s_tone(&AudioEngineTask::send);

constexpr uint32_t kEngineRetryMs = 2000;

}  // namespace

AudioManager::AudioManager()
    : micCopy(micMeter, i2sIn)
{
}

void AudioManager::init() {
    // --- TX: the audio engine's render task owns I2S0 (MAX98357A) ---
    engineWanted = true;
    startEngine();

    // --- Prepare (do NOT start) RX: ICS-43434 mic ---
    // Put mic on the *other* I2S peripheral to avoid any cross-talk.
    // ESP32 has I2S0/I2S1; if TX is using default (0), use 1 here.
    micCfg = i2sIn.defaultConfig(RX_MODE);
    micCfg.port_no         = 1;              // << important: separate port
    micCfg.i2s_format      = I2S_STD_FORMAT; // ICS-43434 standard I2S
    micCfg.sample_rate     = 44100;          // or your preferred rate
    micCfg.bits_per_sample = 16;             // PDM->I2S mics often packed as 24-in-32, except here
    micCfg.channels        = 1;              // mono mic
    micCfg.pin_ws          = 25;             // LRCLK
    micCfg.pin_bck         = 32;             // BCLK
    micCfg.pin_data_rx     = 33;             // DATA IN
    micCfg.pin_data        = -1;             // not used for RX
    micCfg.is_master       = true;
    micCfg.buffer_count    = 6;
    micCfg.buffer_size     = 512;

    // Make mic opt-in:
    micRunRequested = false;   // opt-in
    micRunning      = false;
    micVolumeAtomic  = 0.0f;

    
    // Create the mic pump task ONCE, pinned to core 0
    if (micTaskHandle == nullptr) {
    xTaskCreatePinnedToCore(
        &AudioManager::micTaskThunk, // task entry
        "micPump",      // name
        4096,           // stack size
        this,           // arg = this
        1,              // low priority
        &micTaskHandle, // task handle
        0               // Core 0
    );
}
}

void AudioManager::loop() {
    // The engine task renders tones and sequences; this only retries what
    // could not be done at once (an engine start, a volume change).
    if (engineWanted && !AudioEngineTask::running() && (int32_t)(millis() - engineRetryAtMs) >= 0) {
        startEngine();
    }
    if (volumePending && AudioEngineTask::running()) {
        volumePending = !AudioEngineTask::send(Command::master(masterForVolume(volume)));
    }
}

bool AudioManager::startEngine() {
    if (!AudioEngineTask::start()) {
        engineRetryAtMs = millis() + kEngineRetryMs;
        return false;
    }
    // A fresh engine starts at unity: give it the current volume.
    volumePending = !AudioEngineTask::send(Command::master(masterForVolume(volume)));
    return true;
}

void AudioManager::setVolume(float volumeLevel) {
    float vol = constrain(volumeLevel, 0.0f, 1.0f);
    if (!(vol >= 0.0f)) vol = 0.0f;   // NaN
    const bool changed = masterForVolume(vol) != masterForVolume(volume);
    volume = vol;
    // The engine ramps the change over 10 ms (no zipper noise or click). If it
    // cannot be queued now, loop() sends it.
    if (changed && AudioEngineTask::running()) {
        volumePending = !AudioEngineTask::send(Command::master(masterForVolume(volume)));
    }
}

void AudioManager::playTone(float frequency, int durationMs) {
    s_tone.playTone(AudioEngineTask::engine(), frequency, durationMs);   // durationMs <= 0: until stopTone
}

void AudioManager::stopTone() {
    s_tone.stopTone(AudioEngineTask::engine());   // 5 ms fade; never dropped
}

void AudioManager::playSequence(const ToneStep* steps, int count) {
    if (steps == nullptr || count <= 0) {
        stopSequence();
        return;
    }
    Engine* engine = AudioEngineTask::engine();
    if (engine == nullptr) return;   // port lent out (or no engine): nothing plays
    if (count > cf_audio::kMaxSeqSteps) {
        Serial.printf("[audio] sequence cut to %d of %d steps\n", cf_audio::kMaxSeqSteps, count);
        count = cf_audio::kMaxSeqSteps;
    }
    // Copy the steps now, so the caller's array may go away during playback.
    cf_audio::SeqStep* buf = s_tone.beginSequence(engine);
    if (buf == nullptr) {
        Serial.println("[audio] err=sequence_busy");
        return;
    }
    for (int i = 0; i < count; ++i) {
        buf[i].inc = Engine::hzToInc(steps[i].freq);   // 0 = rest
        buf[i].durMs = steps[i].durationMs;
        buf[i].gapMs = steps[i].gapAfterMs;
    }
    // If the play cannot be queued, the previous sequence stops instead.
    if (!s_tone.commitSequence(engine, count)) Serial.println("[audio] err=sequence_queue_full");
}

void AudioManager::stopSequence() {
    s_tone.stopSequence(AudioEngineTask::engine());   // also releases the tone voice
}

bool AudioManager::isSequencePlaying() const {
    return s_tone.isSequencePlaying(AudioEngineTask::engine());
}

bool AudioManager::releaseI2S(BorrowerStop stopBorrower) {
    // stop() fades every voice, stops the sequencer and lets the DMA cushion
    // play out silence; it reports success only once the render task has
    // acknowledged its exit and the channel is deleted.
    if (!AudioEngineTask::stop()) {
        Serial.println("[audio] err=release_failed (I2S0 still held)");
        return false;
    }
    s_tone.forget();
    engineWanted = false;   // loop() leaves the port alone until reclaimI2S()
    borrowerStop = stopBorrower;
    return true;
}

bool AudioManager::reclaimI2S() {
    borrowerStop = nullptr;
    if (sleepHold) return true;   // going to sleep: the port stays released
    engineWanted = true;   // loop() keeps retrying if this start fails
    if (AudioEngineTask::running()) return true;
    return startEngine();
}

bool AudioManager::stopForSleep(bool hardShutdown) {
    // From here on nothing restarts the engine: not loop(), not a borrower's
    // reclaimI2S(). A sleep that has to wait retries this call instead.
    sleepHold = true;
    engineWanted = false;
    if (borrowerStop != nullptr) {
        // The app streaming on port 0 ends its stream through its own stop
        // path (bounded output waits; none at all on hardShutdown), which
        // hands the port back with reclaimI2S().
        BorrowerStop stopFn = borrowerStop;
        borrowerStop = nullptr;
        const uint32_t t0 = millis();
        stopFn(hardShutdown);
        Serial.printf("[audio] sleep: port-0 app stopped in %u ms\n", (unsigned)(millis() - t0));
    }
    return releaseI2S();
}

void AudioManager::enableMic(bool on) {
    micRunRequested = on; // task will do the rest
}

float AudioManager::getMicVolumeDb() const {
    float lin = getMicVolumeLinear();      // 0..1
    if (lin < 1e-6f) lin = 1e-6f;          // avoid log(0)
    return 20.0f * log10f(lin);            // dBFS (negative up to 0)
}

void AudioManager::micTaskThunk(void *arg) {
    reinterpret_cast<AudioManager*>(arg)->micTaskLoop();
}

void AudioManager::micTaskLoop() {
    static uint8_t buf[512];
    const TickType_t idleDelay = pdMS_TO_TICKS(5);
    uint32_t lastLevelMs = millis();

    for (;;) {
        // State transitions
        if (micRunRequested && !micRunning) {
            i2sIn.begin(micCfg);
            i2sIn.setTimeout(0);

            micMeter.begin(AudioInfo(micCfg.sample_rate, 1, 16));
            micMeter.setTimeout(0);

            micRunning = true;
        } else if (!micRunRequested && micRunning) {
            micMeter.end();
            i2sIn.end();
            micRunning = false;
            micVolumeAtomic = 0.0f;
        }

        if (!micRunning) {
            vTaskDelay(idleDelay);
            continue;
        }

        // Non-blocking pump
        int avail = i2sIn.available();
        if (avail > 0) {
            size_t toRead = (size_t)avail;
            if (toRead > sizeof(buf)) toRead = sizeof(buf);
            int n = i2sIn.readBytes(buf, toRead); // timeout(0) => non-blocking
            if (n > 0) {
                micMeter.write(buf, (size_t)n);
            }
        } else {
            vTaskDelay(1);
        }

        // Publish level ~every 20ms
        uint32_t now = millis();
        if (now - lastLevelMs >= 20) {
            float raw = micMeter.volume();        // ~0..32767
            micVolumeAtomic = (raw <= 0.0f) ? 0.0f : (raw / 32768.0f); // 0..1
            // optional: micMeter.clear();
            lastLevelMs = now;
        }
    }
}
