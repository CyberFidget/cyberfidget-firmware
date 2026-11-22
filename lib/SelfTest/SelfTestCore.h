// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#ifndef SELF_TEST_CORE_H
#define SELF_TEST_CORE_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <limits.h>

namespace SelfTestCore {
constexpr unsigned kSteps = 8;
constexpr unsigned kDetailSize = 192;
constexpr unsigned kReportBytes = 5120; // accommodates every detail at maximum escaping
constexpr uint32_t kButtonTimeoutMs = 15000;
constexpr uint32_t kReadyReleaseSettleMs = 150;
constexpr uint32_t kMotionTimeoutMs = 20000;
constexpr uint32_t kAbortHoldMs = 3000;
constexpr int kAccelSpanMg = 350;
constexpr int kAccelSignMg = 500;
constexpr uint32_t kToneHz = 1000;
constexpr uint32_t kMicRate = 16000;
constexpr uint32_t kAudioRatioMilli = 4000; // power ratio, enclosure calibration required
constexpr uint32_t kAudioMinPower = 4;     // rejects zero / near-zero captures
constexpr unsigned kAudioWindows = 16;
enum Step : uint8_t { Charger, Battery, Buttons, Slider, Accel, Audio, Display, Leds };
// Execution order is independent of the report's stable step indexes.
inline unsigned runStep(unsigned index) {
    static constexpr Step order[] = {Charger, Battery, Audio, Buttons, Slider, Accel, Display, Leds};
    return index < kSteps ? unsigned(order[index]) : kSteps;
}
inline unsigned runIndex(unsigned step) {
    for (unsigned i=0; i<kSteps; ++i) if (runStep(i) == step) return i;
    return kSteps;
}
enum class Phase { Running, Result, PatternPrompt, Answer, Ready, ReadyRelease, ReadySettle, Done };
inline bool waiting(Phase phase) {
    return phase == Phase::Ready || phase == Phase::ReadyRelease || phase == Phase::ReadySettle;
}
inline bool holdsIdleSleep(bool running, Phase phase, bool serialStarted) {
    return running && (serialStarted || !waiting(phase));
}
inline unsigned firstUnfinished(unsigned step, Phase phase) {
    return runIndex(step) + (phase == Phase::Result || waiting(phase) ? 1 : 0);
}
constexpr char kReadyPrompt[] = "Press any button to start";
enum class Result : uint8_t { Pass, Fail, Skip };
enum class Overall : uint8_t { Pass, Fail, Aborted };
inline const char* id(unsigned s) {
    static const char* const ids[] = {"charger", "battery", "buttons", "slider", "accel", "audio", "display", "leds"};
    return s < kSteps ? ids[s] : "idle";
}
inline const char* name(Result r) { return r == Result::Pass ? "pass" : r == Result::Skip ? "skip" : "fail"; }
inline const char* name(Overall r) { return r == Overall::Pass ? "pass" : r == Overall::Aborted ? "aborted" : "fail"; }
// A start stamped after the caller read `now` (a step that began earlier in
// the same tick) is not expired: compare as signed so the difference cannot
// wrap to a huge elapsed time. Still correct across the 49-day millis() wrap.
inline bool expired(uint32_t now, uint32_t start, uint32_t duration) { return int32_t(now - start) >= int32_t(duration); }
inline Result charger(bool answered, uint32_t mv) { return answered && mv > 0 ? Result::Pass : Result::Fail; }
struct StepResult { Result result = Result::Fail; char detail[kDetailSize] = "not run"; };
struct Report {
    uint32_t run = 0;
    char firmware[64] = {};
    Overall overall = Overall::Fail;
    StepResult steps[kSteps];
    void judge(bool aborted) {
        overall = aborted ? Overall::Aborted : Overall::Pass;
        if (!aborted) for (const auto& s : steps) if (s.result == Result::Fail) overall = Overall::Fail;
    }
};

struct ButtonsState {
    unsigned target = 0;
    uint32_t since = 0;
    uint16_t wrong[6] = {};
    static int index(unsigned target) { static const int order[] = {0, 1, 2, 3, 5, 4}; return target < 6 ? order[target] : -1; }
    static const char* label(unsigned target) {
        static const char* const labels[] = {"Up (top left)", "Down (top right)", "Left (middle left)", "Right (middle right)", "Enter (bottom right)", "Back (bottom left)"};
        return target < 6 ? labels[target] : "done";
    }
    bool press(int button, uint32_t now) {
        if (target >= 6 || expired(now, since, kButtonTimeoutMs)) return false;
        if (button == index(target)) { ++target; since = now; return true; }
        if (button >= 0 && button < 6 && wrong[button] != UINT16_MAX) ++wrong[button];
        return false;
    }
    bool timedOut(uint32_t now) const { return target < 6 && expired(now, since, kButtonTimeoutMs); }
    void detail(char* out, size_t n) const {
        snprintf(out, n, "%s%s;wrong Up:%u Down:%u Left:%u Right:%u Enter:%u Back:%u",
                 target == 6 ? "all pressed" : "missing ", target == 6 ? "" : label(target),
                 wrong[0], wrong[1], wrong[2], wrong[3], wrong[5], wrong[4]);
    }
};
// Only a physical press may arm the gate. Swallow input through release
// settling, bounded from that press so a stuck button reaches its check.
inline bool updateReady(Phase& phase, uint32_t& phaseAt, int physicalPress, bool allReleased,
                        uint32_t now, ButtonsState& buttons) {
    if (phase == Phase::Ready && physicalPress >= 0 && physicalPress < 6) {
        phase = Phase::ReadyRelease; phaseAt = now;
    } else if (phase == Phase::ReadyRelease || phase == Phase::ReadySettle) {
        if (expired(now, phaseAt, kButtonTimeoutMs) ||
            (phase == Phase::ReadySettle && allReleased && expired(now, buttons.since, kReadyReleaseSettleMs))) {
            buttons = ButtonsState{}; buttons.since = now;
            phase = Phase::Running;
            return true;
        }
        if (!allReleased) phase = Phase::ReadyRelease;
        else if (phase == Phase::ReadyRelease) {
            // Reuse the inactive Buttons timer for the continuous release interval.
            phase = Phase::ReadySettle; buttons.since = now;
        }
    }
    return false;
}
// Each end counts once the filtered reading has stayed within 3% of it for
// kSliderHoldMs: the filter is still easing toward the end when it first
// crosses, so lo/hi then show where it settled, not the first crossing.
constexpr uint32_t kSliderHoldMs = 1000;
struct SliderState {
    int lo = 100, hi = 0;
    bool seen = false;
    bool lowHeld = false, highHeld = false;
    bool inLow = false, inHigh = false;
    uint32_t lowSince = 0, highSince = 0;
    void sample(int pct, uint32_t now) {
        if (pct < 0 || pct > 100) return;
        seen = true; if (pct < lo) lo = pct; if (pct > hi) hi = pct;
        hold(pct <= 3, now, inLow, lowSince, lowHeld);
        hold(pct >= 97, now, inHigh, highSince, highHeld);
    }
    static void hold(bool in, uint32_t now, bool& was, uint32_t& since, bool& held) {
        if (!in) { was = false; return; }
        if (!was) { was = true; since = now; }
        if (now - since >= kSliderHoldMs) held = true;
    }
    bool passed() const { return lowHeld && highHeld; }
    void detail(char* out, size_t n) const { if (seen) snprintf(out, n, "range %d-%d%%", lo, hi); else snprintf(out, n, "no slider readings"); }
};
struct AccelState {
    int lo[3] = {}, hi[3] = {};
    bool seen = false;
    int sign = 0;
    unsigned changes = 0;
    void sample(int x, int y, int z) {
        const int v[] = {x, y, z};
        for (unsigned i = 0; i < 3; ++i) { if (!seen || v[i] < lo[i]) lo[i] = v[i]; if (!seen || v[i] > hi[i]) hi[i] = v[i]; }
        seen = true;
        int next = z >= kAccelSignMg ? 1 : z <= -kAccelSignMg ? -1 : 0;
        if (next) { if (sign && next != sign && changes < 2) ++changes; sign = next; }
    }
    bool passed() const {
        if (!seen || changes < 2) return false;
        for (unsigned i = 0; i < 3; ++i) if (hi[i] - lo[i] < kAccelSpanMg) return false;
        return true;
    }
    void detail(char* out, size_t n) const {
        if (!seen) { snprintf(out,n,"no accel readings"); return; }
        snprintf(out, n, "mg x:%d/%d y:%d/%d z:%d/%d flips:%u", lo[0], hi[0], lo[1], hi[1], lo[2], hi[2], changes);
    }
};
// LIS2DH12 register output -> mg, matching the existing driver's sensitivity
// tables. Read mode/range without changing any sensor configuration.
inline int accelMg(int16_t raw, uint8_t ctrl1, uint8_t ctrl4) {
    static const int scales[] = {1,2,4,12};
    int factor = scales[(ctrl4 >> 4) & 3];
    if (ctrl1 & 0x08) return (raw / 256) * factor * 16;
    if (ctrl4 & 0x08) return (raw / 16) * factor;
    return (raw / 64) * factor * 4;
}
struct ConfirmState {
    unsigned count = 0;
    char answers[4][8] = {};
    void record(bool pass, bool timeout) {
        if (count < 4) snprintf(answers[count++],8,"%s",timeout ? "timeout" : pass ? "ok" : "fail");
    }
    bool passed(unsigned total) const {
        if (count != total || total > 4) return false;
        for (unsigned i=0;i<total;++i) if (strcmp(answers[i],"ok")) return false;
        return true;
    }
    const char* value(unsigned i) const { return answers[i][0] ? answers[i] : "not_checked"; }
    void detail(bool leds, char* out, size_t cap) const {
        if (leds) snprintf(out,cap,"cycle=%s off=%s",value(0),value(1));
        else snprintf(out,cap,"white=%s checker=%s off=%s",value(0),value(1),value(2));
    }
};

// 256 samples = 16 exact cycles at 1 kHz / 16 kHz. Divide input by 64
// for bounded Q14 recurrence even with clipped microphone samples.
struct ToneMeter {
    int32_t a = 0, b = 0;
    unsigned samples = 0, windows = 0;
    uint64_t powerSum = 0;
    void sample(int16_t raw) {
        if (windows >= kAudioWindows) return;
        int32_t next = raw / 64 + int32_t((int64_t(30274) * a) / 16384) - b;
        b = a; a = next;
        if (++samples == 256) {
            int64_t power = int64_t(a)*a + int64_t(b)*b - (int64_t(30274)*a*b)/16384;
            if (power > 0) powerSum += uint64_t(power) / (256 * 256);
            ++windows; samples = 0; a = b = 0;
        }
    }
    bool ready() const { return windows == kAudioWindows; }
    uint32_t level() const { return windows ? uint32_t(powerSum / windows) : 0; }
};
inline uint32_t ratioMilli(uint32_t silence, uint32_t tone) {
    uint64_t ratio = uint64_t(tone) * 1000 / (silence ? silence : 1);
    return ratio > UINT32_MAX ? UINT32_MAX : uint32_t(ratio);
}
inline bool loopbackPass(uint32_t silence, uint32_t tone) {
    return tone >= kAudioMinPower && ratioMilli(silence, tone) >= kAudioRatioMilli;
}

// Serial values use percent escapes (including spaces, '%' and '=') to keep
// every value a single token. The stored report uses this same bounded codec.
inline bool token(const char* src, char* dst, size_t cap) {
    static const char hex[] = "0123456789ABCDEF";
    size_t n = 0;
    for (; *src; ++src) {
        unsigned char c = static_cast<unsigned char>(*src);
        bool safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == ':' || c == '/';
        unsigned need = safe ? 1 : 3;
        if (n + need >= cap) return false;
        if (safe) dst[n++] = c;
        else { dst[n++] = '%'; dst[n++] = hex[c >> 4]; dst[n++] = hex[c & 15]; }
    }
    if (n >= cap) return false;
    dst[n] = 0; return true;
}
inline int unhex(char c) { return c >= '0' && c <= '9' ? c-'0' : c >= 'A' && c <= 'F' ? c-'A'+10 : -1; }
inline bool untoken(const char* src, char* dst, size_t cap) {
    size_t n = 0;
    while (*src) {
        unsigned char c = *src++;
        if (c == '%') {
            if (!src[0] || !src[1] || unhex(src[0]) < 0 || unhex(src[1]) < 0) return false;
            c = (unhex(src[0]) << 4) | unhex(src[1]); src += 2;
        }
        if (!c || c < 32 || c > 126 || n + 1 >= cap) return false;
        dst[n++] = c;
    }
    if (n >= cap) return false;
    dst[n] = 0; return true;
}
inline bool promptLine(unsigned step, Phase phase, const char* prompt, char* out, size_t cap) {
    char encoded[128 * 3];
    if (!token(prompt, encoded, sizeof(encoded))) return false;
    int n = snprintf(out, cap, "[cmd] health.step=%s prompt=%s", waiting(phase) ? "ready" : id(step), encoded);
    return n >= 0 && size_t(n) < cap;
}
inline bool statusLine(unsigned step, Phase phase, bool running, char* out, size_t cap) {
    const char* state = waiting(phase) ? "ready" : phase == Phase::Result ? "result" :
                        phase == Phase::Answer ? "answer" : phase == Phase::PatternPrompt ? "prompt" : "running";
    int n = running ? snprintf(out, cap, "[cmd] health.status=%s state=%s", waiting(phase) ? "ready" : id(step), state) :
                      snprintf(out, cap, "[cmd] health.status=idle");
    return n >= 0 && size_t(n) < cap;
}
inline bool stepLine(unsigned step, const StepResult& value, char* out, size_t cap) {
    char encoded[kDetailSize * 3];
    if (step >= kSteps || !token(value.detail, encoded, sizeof(encoded))) return false;
    int n = snprintf(out, cap, "[cmd] health.%s=%s detail=%s", id(step), name(value.result), encoded);
    return n >= 0 && size_t(n) < cap;
}
inline bool summaryLine(const Report& report, const char* unit, char* out, size_t cap) {
    char fw[192];
    if (!token(report.firmware, fw, sizeof(fw))) return false;
    int n = snprintf(out, cap, "[cmd] health.summary id=%s fw=%s run=%lu charger=%s battery=%s buttons=%s slider=%s accel=%s audio=%s display=%s leds=%s",
        unit, fw, (unsigned long)report.run, name(report.steps[0].result), name(report.steps[1].result), name(report.steps[2].result), name(report.steps[3].result), name(report.steps[4].result), name(report.steps[5].result), name(report.steps[6].result), name(report.steps[7].result));
    return n >= 0 && size_t(n) < cap;
}

// Versioned text blob; NVS replaces this single key atomically. Parsing is
// transactional: a bad/truncated blob never replaces the caller's report.
inline size_t encode(const Report& report, char* out, size_t cap) {
    char fw[192], detail[kDetailSize * 3];
    if (!token(report.firmware, fw, sizeof(fw))) return 0;
    int n = snprintf(out, cap, "CFHEALTH1|%lu|%s|%s\n", (unsigned long)report.run, name(report.overall), fw);
    if (n < 0 || size_t(n) >= cap) return 0;
    size_t used = n;
    for (const auto& s : report.steps) {
        if (!token(s.detail, detail, sizeof(detail))) return 0;
        n = snprintf(out + used, cap - used, "%s|%s\n", name(s.result), detail);
        if (n < 0 || size_t(n) >= cap-used) return 0;
        used += n;
    }
    return used + 1;
}
inline bool decode(const char* data, size_t len, Report& out) {
    if (!len || len > kReportBytes || data[len-1] != 0 || strlen(data) != len-1) return false;
    const char* p = data;
    auto field = [&p](char separator, char* dst, size_t cap) -> bool {
        const char* end = strchr(p,separator);
        if (!end || size_t(end-p) >= cap) return false;
        size_t n = end-p; memcpy(dst,p,n); dst[n]=0; p=end+1; return true;
    };
    char value[kDetailSize*3], counter[11], overall[8];
    if (!field('|',value,sizeof(value)) || strcmp(value,"CFHEALTH1") ||
        !field('|',counter,sizeof(counter)) || !field('|',overall,sizeof(overall)) || !*counter) return false;
    uint64_t run = 0;
    for (const char* c = counter; *c; ++c) { if (*c < '0' || *c > '9') return false; run = run*10 + (*c-'0'); }
    if (!run || run > UINT32_MAX) return false;
    Report parsed; parsed.run = uint32_t(run);
    if (!strcmp(overall, "pass")) parsed.overall = Overall::Pass;
    else if (!strcmp(overall, "fail")) parsed.overall = Overall::Fail;
    else if (!strcmp(overall, "aborted")) parsed.overall = Overall::Aborted;
    else return false;
    if (!field('\n',value,sizeof(value)) || !untoken(value, parsed.firmware, sizeof(parsed.firmware)) || !parsed.firmware[0]) return false;
    bool failed = false;
    for (auto& s : parsed.steps) {
        if (!field('|',value,sizeof(value))) return false;
        if (!strcmp(value, "pass")) s.result = Result::Pass;
        else if (!strcmp(value, "fail")) { s.result = Result::Fail; failed = true; }
        else if (!strcmp(value, "skip")) s.result = Result::Skip;
        else return false;
        if (!field('\n',value,sizeof(value)) || !untoken(value, s.detail, sizeof(s.detail)) || !s.detail[0]) return false;
    }
    if (*p) return false;
    if (parsed.overall != Overall::Aborted && (failed ? Overall::Fail : Overall::Pass) != parsed.overall) return false;
    out = parsed; return true;
}
} // namespace SelfTestCore
#endif
