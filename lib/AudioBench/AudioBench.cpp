// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

// lib/AudioBench/AudioBench.cpp - see AudioBench.h. Test builds only.

#ifdef CF_TEST_CLI

#include "AudioBench.h"

#include <Arduino.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <esp_heap_caps.h>
#include <math.h>
#include <mbedtls/base64.h>
#include <string.h>

#include <atomic>

#include "AudioEngine.h"
#include "AudioEngineTask.h"
#include "AudioManager.h"
#include "HAL.h"
#include "MicCapture.h"

namespace {

using cf_audio::Command;
using cf_audio::Engine;
using cf_audio::Envelope;
using cf_audio::Wave;

constexpr uint32_t kRate = cf_audio::kSampleRate;

uint16_t ms(uint32_t m) { return (uint16_t)Engine::msToSamples(m); }

// ------------------------------------------------- render-thread patterns
//
// The stress pattern and the sweep run inside the engine's block hook (render
// task), so they keep playing while the loop task is busy - in particular
// during the flash writes of `nvs` / `fs`. The loop task only flips the
// atomics below; a changed generation number makes the hook restart the
// pattern from its first step with its fixed seed.

std::atomic<bool> s_stressOn{false};
std::atomic<uint32_t> s_stressGen{0};
std::atomic<bool> s_sweepOn{false};
std::atomic<uint32_t> s_sweepGen{0};
std::atomic<uint32_t> s_sweepVel{120};

constexpr int kSweepSteps = 23;               // 100 Hz * 2^(k/3), k = 0..22
constexpr uint32_t kSweepOn = kRate / 4;      // 250 ms
constexpr uint32_t kSweepGap = kRate / 20;    // 50 ms

struct HookState {   // render task only
    uint32_t stressGenSeen = 0;
    uint32_t seqClock = 0, nextStep = 0, step = 0, rng = 12345, nextSfx = 0;
    uint32_t sweepGenSeen = 0;
    int sweepIdx = 0;
    uint32_t sweepClock = 0, sweepNext = 0;
    uint16_t sweepVel = 120;
} s_h;

uint32_t midiToInc(int n) { return Engine::hzToInc(440.0f * powf(2.0f, (n - 69) / 12.0f)); }

void note(Engine& e, uint8_t v, Wave w, uint32_t inc, uint32_t gate, uint16_t vel, uint8_t duty8,
          const Envelope& env) {
    e.noteOn(v, w, inc, gate, vel, duty8, env);
}

void stressStep(Engine& e) {
    // 150 BPM 16ths (4410 samples). Original pattern, not any published tune.
    static const int8_t lead[16] = {62, 65, 69, 72, 70, 69, 65, 62, 64, 67, 70, 74, 72, 70, 67, 64};
    static const int8_t bass[16] = {38, 38, 50, 38, 41, 41, 53, 41, 36, 36, 48, 36, 43, 43, 55, 43};
    static const int8_t arp[3] = {0, 3, 7};
    const Envelope lead1 = {ms(2), ms(40), 19661, ms(8)};    // 0.6 sustain
    const Envelope bassEnv = {ms(2), ms(30), 22938, ms(10)}; // 0.7 sustain
    const Envelope hat = {ms(1), ms(25), 0, ms(5)};
    const Envelope kick = {ms(1), ms(70), 0, ms(10)};
    const Envelope snare = {ms(1), ms(80), 0, ms(10)};
    const Envelope hit = {ms(1), ms(140), 0, ms(10)};
    const uint32_t step16 = kRate * 60 / 150 / 4;
    if (s_h.seqClock < s_h.nextStep) return;
    s_h.nextStep += step16;
    const uint32_t k = s_h.step++ & 15;
    const uint32_t gate = step16 * 3 / 4;
    note(e, 0, cf_audio::kPulse, midiToInc(lead[k] + 12), gate, 150, 64, lead1);
    if ((k & 1) == 0) note(e, 1, cf_audio::kPulse, midiToInc(lead[k] + 8), gate * 2, 110, 128, lead1);
    note(e, 2, cf_audio::kSaw, midiToInc(bass[k] + 12), gate, 170, 128, bassEnv);
    note(e, 3, cf_audio::kTriangle, midiToInc(lead[k & 12] + 12 + arp[s_h.step % 3]), gate, 120, 128, lead1);
    note(e, 4, cf_audio::kNoise, Engine::hzToInc(9000.0f), ms(30), 70, 128, hat);
    if ((k & 7) == 0) {
        note(e, 5, cf_audio::kPulse, Engine::hzToInc(180.0f), ms(80), 200, 128, kick);
    } else if ((k & 7) == 4) {
        note(e, 5, cf_audio::kNoise, Engine::hzToInc(4000.0f), ms(90), 160, 128, snare);
    }
    // Random sound effects on voices 6/7 (coin = thin pulse, hit = noise burst).
    if (s_h.seqClock >= s_h.nextSfx) {
        s_h.rng = s_h.rng * 1103515245u + 12345u;
        if ((s_h.rng >> 16) & 1u) {
            note(e, 6, cf_audio::kPulse, Engine::hzToInc(988.0f), ms(120), 140, 31, lead1);
        } else {
            note(e, 7, cf_audio::kNoise, Engine::hzToInc(6000.0f), ms(150), 150, 128, hit);
        }
        s_h.nextSfx = s_h.seqClock + kRate * (250 + ((s_h.rng >> 8) % 300)) / 1000;
    }
}

void sweepStep(Engine& e) {
    if (s_h.sweepClock < s_h.sweepNext) return;
    if (s_h.sweepIdx >= kSweepSteps) {
        s_sweepOn.store(false);
        return;
    }
    const float hz = 100.0f * powf(2.0f, s_h.sweepIdx / 3.0f);
    note(e, 7, cf_audio::kSine, Engine::hzToInc(hz), kSweepOn, s_h.sweepVel, 128, cf_audio::kToneEnvelope);
    s_h.sweepNext += kSweepOn + kSweepGap;
    s_h.sweepIdx++;
}

void benchHook(Engine& e, int frames, void*) {
    const uint32_t sg = s_stressGen.load();
    if (sg != s_h.stressGenSeen) {
        s_h.stressGenSeen = sg;
        s_h.seqClock = s_h.nextStep = s_h.step = s_h.nextSfx = 0;
        s_h.rng = 12345;   // same content on every `stress on`
    }
    if (s_stressOn.load()) {
        stressStep(e);
        s_h.seqClock += (uint32_t)frames;
    }
    const uint32_t wg = s_sweepGen.load();
    if (wg != s_h.sweepGenSeen) {
        s_h.sweepGenSeen = wg;
        s_h.sweepIdx = 0;
        s_h.sweepClock = s_h.sweepNext = 0;
        s_h.sweepVel = (uint16_t)s_sweepVel.load();
    }
    if (s_sweepOn.load()) {
        sweepStep(e);
        s_h.sweepClock += (uint32_t)frames;
    }
}

// ------------------------------------------------------------ control side

bool engineUp() {
    if (AudioEngineTask::running()) return true;
    Serial.println("[abench] err=not_running (I2S0 lent to another app)");
    return false;
}

void send(const Command& c) {
    if (!AudioEngineTask::send(c)) Serial.println("[abench] err=queue_full");
}

void ensureHook() {
    // The engine is rebuilt when the port is lent out and reclaimed, so the
    // hook is re-sent on every use; it is idempotent.
    send(Command::hook(benchHook, nullptr));
}

void engineNote(int voice, int wave, float hz, uint32_t msDur, int vel, int dutyPct) {
    int duty = (dutyPct * 256 + 50) / 100;
    if (duty < 1) duty = 1;
    if (duty > 255) duty = 255;
    if (vel < 0) vel = 0;
    if (vel > 256) vel = 256;
    send(Command::noteOn((uint8_t)voice, (Wave)wave, Engine::hzToInc(hz), Engine::msToSamples(msDur),
                         (uint16_t)vel, (uint8_t)duty, cf_audio::kToneEnvelope));
}

void allOff() {
    s_stressOn.store(false);
    s_sweepOn.store(false);
    Engine* e = AudioEngineTask::engine();
    if (e) e->requestStop(cf_audio::kStopAll);   // persistent: never lost to a full queue
}

void printStats() {
    AudioEngineTask::Stats st;
    AudioEngineTask::readStats(st);
    const double budget = (double)AudioEngineTask::kBlockFrames * ((double)ESP.getCpuFreqMHz() * 1e6) / kRate;
    const double avg = st.blocks ? (double)st.cycleSum / st.blocks : 0;
    Serial.printf("[abench] stats blocks=%u cpu_avg_pct=%.2f cpu_max_pct=%.2f cyc_avg=%.0f cyc_max=%u "
                  "underruns=%u gap_underruns=%u gap_max_ms=%.1f clips=%u peak_voices=%u heap_int_free=%u "
                  "heap_int_min=%u stack_hwm=%u\n",
                  (unsigned)st.blocks, 100.0 * avg / budget, 100.0 * st.cycleMax / budget, avg,
                  (unsigned)st.cycleMax, (unsigned)st.sendQueueOverflows, (unsigned)st.gapUnderruns,
                  st.gapMaxUs / 1000.0, (unsigned)st.clips, (unsigned)st.peakVoices,
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                  (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                  (unsigned)st.stackHighWater);
}

// ---- speaker EQ (bench A/B; the product bus keeps it bypassed)
// hpf p1hz p1db p1q p2hz p2db p2q gaindb
float s_eqParams[8] = {800, 2000, 4, 1.0f, 6400, -6, 2.0f, 6};

bool inRange(float x, float lo, float hi) { return x >= lo && x <= hi; }   // false for NaN

// Designs the three bands from p; false (nothing sent) when a parameter or
// the resulting setting is out of range.
bool designEq(const float* p, cf_audio::BiquadCoefs* b, int32_t* gainQ8) {
    if (!inRange(p[0], 20, 20000) || !inRange(p[1], 20, 20000) || !inRange(p[4], 20, 20000) ||
        !inRange(p[3], 0.1f, 20) || !inRange(p[6], 0.1f, 20) ||
        !inRange(p[2], -24, 24) || !inRange(p[5], -24, 24) || !inRange(p[7], -24, 24)) {
        return false;
    }
    b[0] = cf_audio::designHighpass(p[0], 0.7071);
    b[1] = cf_audio::designPeaking(p[1], p[2], p[3]);
    b[2] = cf_audio::designPeaking(p[4], p[5], p[6]);
    *gainQ8 = cf_audio::dbToGainQ8(p[7]);
    return Engine::eqSettingValid(b, 3, *gainQ8);
}

bool sendEq(bool enable) {
    cf_audio::BiquadCoefs b[3];
    int32_t gainQ8 = 256;
    if (!designEq(s_eqParams, b, &gainQ8)) return false;
    for (uint8_t i = 0; i < 3; ++i) send(Command::eqBand(i, b[i]));
    send(Command::eqCommit(3, enable, gainQ8));   // applied atomically
    return true;
}

// --------------------------------------------------------------- recorder
enum ActKind : uint8_t { A_NONE, A_TONE, A_ENGINE, A_SWEEP };
struct Recorder {
    int16_t* buf = nullptr;
    uint32_t capSamples = 0;
    uint32_t len = 0;
    bool active = false;
    uint32_t startMs = 0;
    ActKind act = A_NONE;
    uint32_t actAtMs = 300;
    bool actDone = false;
    float hz = 1000;
    uint32_t durMs = 100;
    int vel = 160;
} s_rec;

constexpr uint32_t kMicRate = 48000;

bool startRecording(uint32_t msLen, ActKind act) {
    if (s_rec.active) { Serial.println("[abench] err=rec_busy"); return false; }
    if (msLen > 12000) msLen = 12000;
    if (s_rec.buf) { heap_caps_free(s_rec.buf); s_rec.buf = nullptr; }
    s_rec.capSamples = kMicRate * msLen / 1000;
    s_rec.buf = (int16_t*)heap_caps_malloc(s_rec.capSamples * 2, MALLOC_CAP_SPIRAM);
    if (!s_rec.buf) { Serial.println("[abench] err=psram"); return false; }
    const char* err = nullptr;
    if (!MicCapture::instance().acquire("abench", kMicRate, &err, 8)) {
        Serial.printf("[abench] err=mic %s\n", err ? err : "?");
        return false;
    }
    MicCapture::instance().startStreaming(0);
    s_rec.len = 0;
    s_rec.act = act;
    s_rec.actDone = (act == A_NONE);
    s_rec.startMs = millis();
    s_rec.active = true;
    Serial.printf("[abench] rec.start ms=%u rate=%u act=%d\n", (unsigned)msLen, (unsigned)kMicRate, (int)act);
    return true;
}

void pollRecorder() {
    if (!s_rec.active) return;
    auto& mic = MicCapture::instance();
    const uint32_t room = (s_rec.capSamples - s_rec.len) * 2;
    if (room) s_rec.len += mic.ring().pop((uint8_t*)(s_rec.buf + s_rec.len), room) / 2;
    const uint32_t elapsed = millis() - s_rec.startMs;
    if (!s_rec.actDone && elapsed >= s_rec.actAtMs) {
        s_rec.actDone = true;
        // Mic sample index when the action fired. Mic DMA latency shifts both
        // edges of a tone equally, so durations are latency-free; onsets are not.
        Serial.printf("[abench] act.fire at_sample=%u\n", (unsigned)s_rec.len);
        if (s_rec.act == A_TONE) {
            HAL::audioManager().playTone(s_rec.hz, (int)s_rec.durMs);   // the product path
        } else if (s_rec.act == A_ENGINE) {
            engineNote(7, cf_audio::kSine, s_rec.hz, s_rec.durMs, s_rec.vel, 50);
        } else if (s_rec.act == A_SWEEP) {
            ensureHook();
            s_sweepVel.store((uint32_t)s_rec.vel);
            s_sweepGen.fetch_add(1);
            s_sweepOn.store(true);
        }
    }
    if (s_rec.len >= s_rec.capSamples) {
        mic.stopStreaming();
        mic.release();
        s_rec.active = false;
        Serial.printf("[abench] rec.done samples=%u\n", (unsigned)s_rec.len);
    }
}

void dumpRecording() {
    if (!s_rec.buf || s_rec.active) { Serial.println("[abench] err=nothing_to_dump"); return; }
    const uint8_t* p = (const uint8_t*)s_rec.buf;
    const uint32_t total = s_rec.len * 2;
    unsigned char line[1100];
    Serial.printf("[abench] dump.begin bytes=%u rate=%u\n", (unsigned)total, (unsigned)kMicRate);
    for (uint32_t off = 0; off < total; off += 768) {
        const uint32_t n = (total - off) < 768 ? (total - off) : 768;
        size_t olen = 0;
        mbedtls_base64_encode(line, sizeof(line) - 1, &olen, p + off, n);
        line[olen] = 0;
        Serial.print("[abench] d=");
        Serial.println((const char*)line);
    }
    Serial.println("[abench] dump.end");
}

void nvsStall(int n) {
    Preferences prefs;
    uint8_t blob[512];
    for (int i = 0; i < 512; ++i) blob[i] = (uint8_t)(i * 7 + n);
    const uint32_t t0 = millis();
    prefs.begin("abench", false);
    for (int i = 0; i < n; ++i) { blob[0] = (uint8_t)i; prefs.putBytes("blob", blob, sizeof(blob)); }
    prefs.clear();
    prefs.end();
    Serial.printf("[abench] nvs.writes=%d ms=%u\n", n, (unsigned)(millis() - t0));
}

void fsStall(int kb) {
    uint8_t blob[1024];
    memset(blob, 0x5A, sizeof(blob));
    const uint32_t t0 = millis();
    File f = LittleFS.open("/abench.bin", "w");
    if (!f) { Serial.println("[abench] err=fs_open (LittleFS not mounted?)"); return; }
    for (int i = 0; i < kb; ++i) f.write(blob, sizeof(blob));
    f.close();
    LittleFS.remove("/abench.bin");
    Serial.printf("[abench] fs.kb=%d ms=%u\n", kb, (unsigned)(millis() - t0));
}

void help() {
    Serial.println("[abench] verbs: start [desc frames prio core] (engine always runs; reports its config) |"
                   " stop | stats [reset] | stress on|off |"
                   " note v wave hz ms vel [duty] (wave 0 pulse 1 tri 2 saw 3 noise 4 sine) | off |"
                   " master q8 | measure tone|engine hz ms [vel] (tone = AudioManager::playTone;"
                   " legacy -> err=legacy_unavailable) |"
                   " sweep [vel] | rec ms | dump | eq on|off|set hpf p1hz p1db p1q p2hz p2db p2q gaindb |"
                   " nvs n | fs kb");
}

}  // namespace

namespace AudioBench {

void poll() { pollRecorder(); }

void command(const char* arg) {
    char sub[16] = {0};
    int consumed = 0;
    if (sscanf(arg, "%15s %n", sub, &consumed) < 1) { help(); return; }
    const char* rest = arg + consumed;

    if (!strcmp(sub, "help")) { help(); return; }
    if (!strcmp(sub, "start")) {
        // The engine runs for the whole power cycle; any arguments are ignored.
        if (!engineUp()) return;
        Serial.printf("[abench] started rate=%u block=%d dma=%dx%d cushion_ms=%.1f prio=%d core=%d\n",
                      (unsigned)kRate, AudioEngineTask::kBlockFrames, AudioEngineTask::kDmaDescNum,
                      AudioEngineTask::kBlockFrames,
                      1000.0f * AudioEngineTask::kDmaDescNum * AudioEngineTask::kBlockFrames / kRate,
                      AudioEngineTask::kTaskPriority, AudioEngineTask::kTaskCore);
        return;
    }
    if (!strcmp(sub, "stop")) {
        if (!engineUp()) return;
        allOff();
        Serial.println("[abench] stopped");
        return;
    }
    if (!strcmp(sub, "stats")) {
        if (!strncmp(rest, "reset", 5)) {
            AudioEngineTask::requestStatsReset();
            Serial.println("[abench] stats.reset");
        } else {
            printStats();
        }
        return;
    }
    if (!strcmp(sub, "stress")) {
        if (!engineUp()) return;
        const bool on = strncmp(rest, "off", 3) != 0;
        if (on) {
            ensureHook();
            s_stressGen.fetch_add(1);   // restart from step 0 with the fixed seed
            s_stressOn.store(true);
        } else {
            allOff();
        }
        Serial.printf("[abench] stress=%d\n", on ? 1 : 0);
        return;
    }
    if (!strcmp(sub, "note")) {
        int v = 0, w = 0, msDur = 300, vel = 160, duty = 50;
        float hz = 440;
        if (sscanf(rest, "%d %d %f %d %d %d", &v, &w, &hz, &msDur, &vel, &duty) < 4) { help(); return; }
        if (v < 0 || v >= cf_audio::kVoices || w < 0 || w > cf_audio::kSine || msDur < 0) {
            Serial.println("[abench] err=bad_args");
            return;
        }
        if (!engineUp()) return;
        engineNote(v, w, hz, (uint32_t)msDur, vel, duty);
        Serial.println("[abench] note.ok");
        return;
    }
    if (!strcmp(sub, "off")) {
        allOff();
        Serial.println("[abench] off");
        return;
    }
    if (!strcmp(sub, "eq")) {
        if (!engineUp()) return;
        if (!strncmp(rest, "set", 3)) {
            float p[8];
            memcpy(p, s_eqParams, sizeof(p));
            sscanf(rest + 3, "%f %f %f %f %f %f %f %f", &p[0], &p[1], &p[2], &p[3], &p[4], &p[5], &p[6], &p[7]);
            cf_audio::BiquadCoefs b[3];
            int32_t gainQ8 = 256;
            if (!designEq(p, b, &gainQ8)) {
                Serial.println("[abench] err=eq_range (hz 20..20000, q 0.1..20, db -24..24, stable bands)");
                return;
            }
            memcpy(s_eqParams, p, sizeof(p));
            sendEq(false);   // designed, left off until `eq on`
            const float* v = s_eqParams;
            Serial.printf("[abench] eq.set hpf=%.0f p1=%.0f/%.1f/%.2f p2=%.0f/%.1f/%.2f gain=%.1f\n",
                          v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7]);
            return;
        }
        const bool on = !strncmp(rest, "on", 2);
        if (!sendEq(on)) {
            Serial.println("[abench] err=eq_range");
            return;
        }
        Serial.printf("[abench] eq=%d\n", on ? 1 : 0);
        return;
    }
    if (!strcmp(sub, "master")) {
        if (!engineUp()) return;
        int q8 = atoi(rest);
        if (q8 < 0) q8 = 0;
        if (q8 > 256) q8 = 256;
        send(Command::master((uint32_t)q8 * 128u));
        Serial.printf("[abench] master=%d\n", q8);
        return;
    }
    if (!strcmp(sub, "measure")) {
        char kind[10] = {0};
        float hz = 1000;
        int msDur = 100, vel = 160;
        if (sscanf(rest, "%9s %f %d %d", kind, &hz, &msDur, &vel) < 3) { help(); return; }
        ActKind act;
        if (!strcmp(kind, "tone")) act = A_TONE;          // product path: AudioManager::playTone
        else if (!strcmp(kind, "engine")) act = A_ENGINE; // raw engine sine note at `vel`
        else if (!strcmp(kind, "legacy")) {
            // The old audio-tools tone chain is gone; nothing here plays it.
            Serial.println("[abench] err=legacy_unavailable");
            return;
        } else { help(); return; }
        if (!engineUp()) return;
        s_rec.hz = hz;
        s_rec.durMs = (uint32_t)(msDur > 0 ? msDur : 0);
        s_rec.vel = vel;
        s_rec.actAtMs = 300;
        startRecording(300 + s_rec.durMs + 700, act);
        return;
    }
    if (!strcmp(sub, "sweep")) {
        int vel = 120;
        sscanf(rest, "%d", &vel);
        if (!engineUp()) return;
        s_rec.vel = vel;
        s_rec.actAtMs = 300;
        startRecording(300 + kSweepSteps * 300 + 500, A_SWEEP);
        return;
    }
    if (!strcmp(sub, "rec")) { startRecording((uint32_t)atoi(rest), A_NONE); return; }
    if (!strcmp(sub, "dump")) { dumpRecording(); return; }
    if (!strcmp(sub, "nvs")) { nvsStall(atoi(rest)); return; }
    if (!strcmp(sub, "fs")) { fsStall(atoi(rest)); return; }
    help();
}

}  // namespace AudioBench

#endif  // CF_TEST_CLI
