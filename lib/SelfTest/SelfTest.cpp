// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#include "SelfTest.h"
#include "SelfTestCore.h"
#include <Preferences.h>
#include <Wire.h>
#include <math.h>
#include "HAL.h"
#include "AudioManager.h"
#include "AudioEngineTask.h"
#include "ButtonManager.h"
#include "RGBController.h"
#include "MenuManager.h"
#include "AppManager.h"
#include "MicCapture.h"
#include "SyncProtocol.h"
#include "globals.h"

namespace {
using namespace SelfTestCore;
constexpr uint32_t kResultMs = 1000;
constexpr uint32_t kPatternPromptMs = 2500;
constexpr uint32_t kAnswerTimeoutMs = 30000;
constexpr uint32_t kAudioTimeoutMs = 5000;
constexpr uint32_t kAudioSettleMs = 200;
constexpr uint8_t kGaugeAddress = 0x36;
constexpr uint8_t kAccelAddress = 0x19;
constexpr uint8_t kLedLevel = 96;
const char* const kPatterns[] = {"Full white", "Checkerboard", "All off"};
const char* const kChannels[] = {"red", "green", "blue", "white"};
enum class Phase { Running, Result, PatternPrompt, Answer, Done };
Report report;
unsigned step = 0, item = 0;
Phase phase = Phase::Done;
uint32_t stepAt = 0, phaseAt = 0, backAt = 0;
bool active = false, running = false, backDown = false, micOwned = false;
bool abortedByHold = false;  // the Back release that follows a hold-abort must not leave the result screen
bool storageReady = false;
OverlayMode savedOverlay = OverlayMode::OVERLAY_OFF;
Preferences prefs;
ButtonsState buttons;
SliderState slider;
AccelState accel;
ToneMeter silence, tone;
unsigned audioPhase = 0;
uint32_t audioAt = 0;
char micError[32] = {};
char prompt[128] = {};
ConfirmState confirms;
bool enterAnswerDown = false, backAnswerDown = false;
char blob[kReportBytes];
char line[kDetailSize * 3 + 96];

// Render the existing Arial font directly. DisplayProxy's text API takes
// Arduino String, which would allocate on every 50 Hz repaint.
void text(int x, int y, const char* s) {
    auto& d = HAL::displayProxy();
    const uint8_t* font = ArialMT_Plain_10;
    const unsigned height = pgm_read_byte(font + 1);
    const unsigned first = pgm_read_byte(font + 2);
    const unsigned count = pgm_read_byte(font + 3);
    const unsigned rows = (height + 7) / 8;
    for (; *s && x < 128; ++s) {
        unsigned ch = static_cast<unsigned char>(*s);
        if (ch < first || ch-first >= count) continue;
        const uint8_t* jump = font + 4 + (ch-first)*4;
        unsigned offset = (pgm_read_byte(jump) << 8) | pgm_read_byte(jump+1);
        unsigned bytes = pgm_read_byte(jump+2), width = pgm_read_byte(jump+3);
        if (offset != 65535) {
            const uint8_t* glyph = font + 4 + count*4 + offset;
            for (unsigned i=0; i<bytes; ++i) {
                uint8_t bits = pgm_read_byte(glyph+i);
                for (unsigned bit=0; bit<8; ++bit) {
                    unsigned dy = (i%rows)*8 + bit;
                    if (dy < height && (bits & (1u<<bit))) d.setPixel(x+i/rows, y+dy);
                }
            }
        }
        x += width;
    }
}

bool readRegisters(uint8_t address, uint8_t reg, uint8_t* out, uint8_t count) {
    Wire.beginTransmission(address); Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom(address, count) != count) return false;
    for (uint8_t i=0; i<count; ++i) out[i] = Wire.read();
    return true;
}

void quiet() {
    HAL::audioManager().stopTone();
    setColorsOff();
    HAL::showRgbLeds();
    if (micOwned) MicCapture::instance().stopStreaming();
}
void releaseMic() {
    if (micOwned) { MicCapture::instance().stopStreaming(); MicCapture::instance().release(); micOwned = false; }
}
void emitStep(unsigned s) {
    if (stepLine(s, report.steps[s], line, sizeof(line))) Serial.println(line);
}
void emitReport(const Report& value) {
    for (unsigned s=0;s<kSteps;++s) {
        if (stepLine(s, value.steps[s], line, sizeof(line))) Serial.println(line);
    }
    char unit[13]; SyncProtocol::formatDeviceId(ESP.getEfuseMac(), unit);
    if (summaryLine(value, unit, line, sizeof(line))) Serial.println(line);
    Serial.printf("[cmd] health.done=%s\n", name(value.overall));
}
void announce(const char* instruction) {
    snprintf(prompt, sizeof(prompt), "%s", instruction);
    char encoded[sizeof(prompt)*3]; token(prompt, encoded, sizeof(encoded));
    Serial.printf("[cmd] health.step=%s prompt=%s\n", id(step), encoded);
}
void audioDetail(char* out, size_t cap, const char* reason) {
    uint32_t s = silence.level(), t = tone.level();
    snprintf(out,cap,"silence=%lu tone=%lu ratio_milli=%lu hz=%lu min_ratio_milli=%lu reason=%s",
             (unsigned long)s,(unsigned long)t,(unsigned long)ratioMilli(s,t),(unsigned long)kToneHz,(unsigned long)kAudioRatioMilli,reason);
}
void finish(bool aborted) {
    quiet(); releaseMic();
    if (aborted) {
        unsigned first = step + (phase == Phase::Result ? 1 : 0);
        for (unsigned s=first; s<kSteps; ++s) {
            report.steps[s].result = s == Battery ? Result::Skip : Result::Fail;
            snprintf(report.steps[s].detail,kDetailSize,"%s",s == Battery ? "no battery check yet" : s == step ? "aborted during step" : "aborted before start");
            if (s == Audio && s == step) audioDetail(report.steps[s].detail,kDetailSize,"aborted during step");
            if ((s == Display || s == Leds) && s == step) {
                char partial[kDetailSize]; confirms.detail(s == Leds,partial,sizeof(partial));
                snprintf(report.steps[s].detail,kDetailSize,"aborted; %.170s",partial);
            }
            emitStep(s);
        }
    }
    report.judge(aborted);
    size_t bytes = encode(report,blob,sizeof(blob));
    bool saved = storageReady && bytes && prefs.putBytes("report",blob,bytes) == bytes;
    Serial.println(saved ? "[cmd] health.stored=ok" : "[err] health.store=failed");
    char unit[13]; SyncProtocol::formatDeviceId(ESP.getEfuseMac(),unit);
    if (summaryLine(report,unit,line,sizeof(line))) Serial.println(line);
    Serial.printf("[cmd] health.done=%s\n",name(report.overall));
    running = false; phase = Phase::Done;
}
void complete(Result result, const char* detail) {
    quiet();
    report.steps[step].result = result;
    snprintf(report.steps[step].detail,kDetailSize,"%s",detail);
    emitStep(step);
    phase = Phase::Result; phaseAt = millis();
}
void buttonPrompt() {
    char p[96]; snprintf(p,sizeof(p),"Press %s",ButtonsState::label(buttons.target)); announce(p);
}
void showChannel() {
    for (int led=0;led<4;++led) HAL::setRgbLed(led, item == 0 ? kLedLevel : 0, item == 1 ? kLedLevel : 0,
                                                          item == 2 ? kLedLevel : 0, item == 3 ? kLedLevel : 0);
    HAL::showRgbLeds();
}
void confirmPrompt() {
    quiet();
    enterAnswerDown = backAnswerDown = false;
    char p[128];
    if (step == Display) snprintf(p,sizeof(p),"%s next. Check pixels/traces. Enter: looks right; Back: problem",kPatterns[item]);
    else snprintf(p,sizeof(p),"%s next. Check all 4 LEDs, including back, when lit. Enter: all correct; Back: problem",kChannels[item]);
    announce(p);
    phase = Phase::PatternPrompt; phaseAt = millis();
}
void startStep() {
    quiet(); stepAt = phaseAt = millis(); phase = Phase::Running;
    switch (step) {
    case Charger: announce("Reading powered fuel gauge"); break;
    case Battery: announce("No battery check yet"); break;
    case Buttons: buttons = ButtonsState{}; buttons.since = stepAt; buttonPrompt(); break;
    case Slider: slider = SliderState{}; announce("Slide to both ends"); break;
    case Accel: accel = AccelState{}; announce("Start face up; turn face down, then face up. Tilt all axes"); break;
    case Audio:
        announce("Keep quiet: measuring silence, then speaker tone");
        silence = ToneMeter{}; tone = ToneMeter{}; audioPhase = 0; audioAt = stepAt;
        if (micOwned) MicCapture::instance().startStreaming(0);
        break;
    case Display: case Leds: item = 0; confirms = ConfirmState{}; confirmPrompt(); break;
    }
}

bool answer(bool pass, bool timeout = false) {
    if (!running || (step != Display && step != Leds) || phase != Phase::Answer) return false;
    confirms.record(pass,timeout);
    quiet();
    if (++item < (step == Display ? 3u : 4u)) confirmPrompt();
    else {
        char detail[kDetailSize];
        confirms.detail(step == Leds,detail,sizeof(detail));
        complete(confirms.passed(item) ? Result::Pass : Result::Fail,detail);
    }
    return true;
}
void onButton(const ButtonEvent& ev) {
    if (!active) return;
    uint32_t now = millis();
    if (ev.eventType == ButtonEvent_Pressed && phase == Phase::Answer) {
        if (ev.buttonIndex == button_EnterIndex) enterAnswerDown = true;
        if (ev.buttonIndex == button_SelectIndex) backAnswerDown = true;
    }
    if (ev.buttonIndex == button_SelectIndex) {
        if (ev.eventType == ButtonEvent_Pressed) { backDown = true; backAt = now; }
        if (ev.eventType == ButtonEvent_Released) {
            bool held = backDown && expired(now,backAt,kAbortHoldMs); backDown = false;
            if (held && running) { finish(true); return; }
            if (abortedByHold) { abortedByHold = false; return; }  // stay on the ABORTED screen
            if (!running) { MenuManager::instance().returnToMenu(); return; }
            bool pending = backAnswerDown; backAnswerDown = false;
            if (!held && pending) answer(false);
        }
    }
    if (ev.eventType == ButtonEvent_Pressed && running && phase == Phase::Running && step == Buttons) {
        if (buttons.press(ev.buttonIndex,now)) {
            if (buttons.target == 6) { char detail[kDetailSize]; buttons.detail(detail,sizeof(detail)); complete(Result::Pass,detail); }
            else buttonPrompt();
        }
    } else if (ev.eventType == ButtonEvent_Released && ev.buttonIndex == button_EnterIndex) {
        bool pending = enterAnswerDown; enterAnswerDown = false; if (pending) answer(true);
    }
}

void completeAudio(const char* reason, bool measured) {
    uint32_t s = silence.level(), t = tone.level();
    char detail[kDetailSize];
    audioDetail(detail,sizeof(detail),reason);
    complete(measured && loopbackPass(s,t) ? Result::Pass : Result::Fail,detail);
    releaseMic();
}
void updateAudio(uint32_t now) {
    if (!micOwned) { completeAudio(micError[0] ? micError : "mic unavailable",false); return; }
    if (!AudioEngineTask::running()) { completeAudio("speaker engine unavailable",false); return; }
    if (expired(now,stepAt,kAudioTimeoutMs)) { completeAudio("mic capture timeout",false); return; }
    auto& mic = MicCapture::instance();
    if (mic.ring().droppedBytes()) { completeAudio("mic samples dropped",false); return; }
    int16_t samples[512];
    // Drain at most 2048 samples per tick; no waiting for the capture task.
    for (unsigned batch=0;batch<4;++batch) {
        unsigned n = mic.ring().pop(reinterpret_cast<uint8_t*>(samples),sizeof(samples))/2;
        if (!n) break;
        if (audioPhase == 0 || audioPhase == 2) continue;
        auto& meter = audioPhase == 1 ? silence : tone;
        for (unsigned i=0;i<n;++i) meter.sample(samples[i]);
    }
    if ((audioPhase == 0 || audioPhase == 2) && expired(now,audioAt,kAudioSettleMs)) {
        ++audioPhase;
    } else if (audioPhase == 1 && silence.ready()) {
        HAL::audioManager().playTone(kToneHz,2000);
        audioPhase = 2; audioAt = now;
    } else if (audioPhase == 3 && tone.ready()) {
        completeAudio("measured",true);
    }
}

void draw() {
    auto& d = HAL::displayProxy(); d.clear(); d.setColor(WHITE);
    if (running && step == Display && phase == Phase::Answer) {
        if (item == 0) d.fillRect(0,0,128,64);
        if (item == 1) for (int y=0;y<64;y+=8) for (int x=0;x<128;x+=8) if (((x+y)/8)%2 == 0) d.fillRect(x,y,8,8);
        d.display(); return;
    }
    char buf[80];
    if (!running) {
        text(0,0,report.overall == Overall::Pass ? "Self Test: PASS" : report.overall == Overall::Aborted ? "Self Test: ABORTED" : "Self Test: FAIL");
        unsigned failed = 0;
        for (unsigned s=0;s<kSteps;++s) if (report.steps[s].result == Result::Fail) {
            // Two columns keep every failed id visible (including aborted runs).
            text((failed%2)*64,13+(failed/2)*10,id(s)); ++failed;
        }
        text(0,52,"Back: menu"); d.display(); return;
    }
    snprintf(buf,sizeof(buf),"%u/%u %s",step+1,kSteps,id(step)); text(0,0,buf);
    if (phase == Phase::Result) {
        text(0,16,name(report.steps[step].result));
        if (step == Battery) text(0,30,"No battery check yet");
        else if (step == Buttons && report.steps[step].result == Result::Fail) { text(0,30,"Not pressed in 15 s:"); text(0,42,ButtonsState::label(buttons.target)); }
        else if (step == Audio) { snprintf(buf,sizeof(buf),"quiet:%lu tone:%lu",(unsigned long)silence.level(),(unsigned long)tone.level()); text(0,30,buf); }
        else if (step == Slider) { snprintf(buf,sizeof(buf),"range %d-%d%%",slider.lo,slider.hi); text(0,30,buf); }
        else text(0,30,report.steps[step].detail);
    } else if (step == Buttons) {
        text(0,14,"Press:"); text(0,27,ButtonsState::label(buttons.target)); text(0,41,"15 s per button");
    } else if (step == Slider) {
        text(0,14,"Slide to both ends");
        snprintf(buf,sizeof(buf),"low:%d%% high:%d%%",slider.lo,slider.hi); text(0,27,buf);
        d.drawRect(0,44,128,8); if (slider.seen) d.fillRect(1,45,sliderPercent()*126/100,6);
    } else if (step == Accel) {
        text(0,13,"Start face up. Turn"); text(0,25,"face down, then face up");
        snprintf(buf,sizeof(buf),"Tilt axes; flips:%u/2",accel.changes); text(0,37,buf);
        snprintf(buf,sizeof(buf),"x:%s y:%s z:%s",accel.hi[0]-accel.lo[0] >= kAccelSpanMg ? "ok" : "--",
                 accel.hi[1]-accel.lo[1] >= kAccelSpanMg ? "ok" : "--",accel.hi[2]-accel.lo[2] >= kAccelSpanMg ? "ok" : "--"); text(0,49,buf);
    } else if (step == Display) {
        snprintf(buf,sizeof(buf),"Next: %s",kPatterns[item]); text(0,13,buf);
        text(0,25,"Check pixels and traces"); text(0,37,"Enter: looks right"); text(0,49,"Back: problem");
    } else if (step == Leds) {
        snprintf(buf,sizeof(buf),"%s %s",phase == Phase::PatternPrompt ? "Next, all 4:" : "All 4:",kChannels[item]); text(0,13,buf);
        text(0,25,"Check back LED too");
        if (phase == Phase::PatternPrompt) { text(0,37,"Answer when lit"); text(0,49,"Enter: OK  Back: problem"); }
        else { text(0,37,"Enter: all four correct"); text(0,49,"Back: problem"); }
    } else if (step == Audio) {
        text(0,14,"Keep quiet"); text(0,28,audioPhase < 2 ? "Measuring silence" : "Measuring speaker tone");
    } else text(0,16,step == Charger ? "Reading fuel gauge" : "No battery check yet");
    d.display();
}
} // namespace

namespace SelfTest {
void begin() {
    setColorsOff(); HAL::showRgbLeds(); HAL::audioManager().stopTone();
    savedOverlay = HAL::displayProxy().getOverlayMode();
    HAL::displayProxy().setOverlayMode(OverlayMode::OVERLAY_OFF);
    HAL::displayProxy().normalDisplay();
    active = running = true; abortedByHold = false;
    backDown = HAL::buttonManager().isPressed(button_SelectIndex); backAt = millis();
    for (int i=0;i<6;++i) HAL::buttonManager().registerCallback(i,onButton);
    report = Report{}; snprintf(report.firmware,sizeof(report.firmware),"%s",getFirmwareVersionString());
    report.steps[Battery].result = Result::Skip;
    snprintf(report.steps[Battery].detail,kDetailSize,"no battery check yet");
    storageReady = prefs.begin("selftest",false);
    uint32_t prior = storageReady ? prefs.getUInt("counter",0) : 0;
    report.run = prior == UINT32_MAX ? prior : prior+1;
    if (!storageReady || prior == UINT32_MAX || prefs.putUInt("counter",report.run) != sizeof(uint32_t)) {
        storageReady = false; Serial.println("[err] health.counter=unavailable");
    }
    // Acquire once here: allocation and task creation stay outside update().
    const char* err = nullptr;
    micOwned = MicCapture::instance().acquire("selftest",kMicRate,&err,4);
    snprintf(micError,sizeof(micError),"%s",micOwned ? "" : err ? err : "mic unavailable");
    step = Charger; startStep();
}
void update() {
    if (!active) return;
    uint32_t now = millis();
    // A run is use of the device: hold off the idle sleep until it ends. Every
    // step has its own deadline, so this cannot keep the device awake forever.
    if (running) millis_APP_LASTINTERACTION = now;
    if (running && backDown && expired(now,backAt,kAbortHoldMs)) { abortedByHold = true; finish(true); }
    if (running && phase == Phase::Result && expired(now,phaseAt,kResultMs)) {
        if (++step == kSteps) finish(false); else startStep();
    }
    if (running && phase == Phase::PatternPrompt && expired(now,phaseAt,kPatternPromptMs)) {
        phase = Phase::Answer; phaseAt = now; if (step == Leds) showChannel();
    }
    if (running && phase == Phase::Answer && expired(now,phaseAt,kAnswerTimeoutMs)) answer(false,true);
    if (running && phase == Phase::Running) {
        char detail[kDetailSize];
        switch (step) {
        case Charger: {
            uint8_t raw[2]; bool ok = readRegisters(kGaugeAddress,0x02,raw,2);
            uint32_t mv = ok ? ((uint32_t(raw[0])<<8) | raw[1])*5/64 : 0;
            if (ok) snprintf(detail,sizeof(detail),"vcell_mv=%lu",(unsigned long)mv);
            else snprintf(detail,sizeof(detail),"gauge read failed");
            complete(charger(ok,mv),detail); break;
        }
        case Battery: complete(Result::Skip,"no battery check yet"); break;
        case Buttons:
            if (buttons.timedOut(now)) { buttons.detail(detail,sizeof(detail)); complete(Result::Fail,detail); }
            break;
        case Slider: {
            if (expired(now,stepAt,kMotionTimeoutMs)) { slider.detail(detail,sizeof(detail)); complete(Result::Fail,detail); break; }
            float pct = sliderPosition_Percentage_Inverted_Filtered;
            // Floor the low end / ceil the high end so rounding cannot pass
            // a filtered 3.4% / 96.6% reading at the strict endpoint limits.
            if (isfinite(pct) && pct >= 0 && pct <= 100) {
                int p = pct <= 50 ? int(ceilf(pct)) : int(floorf(pct)); slider.sample(p);
            }
            if (slider.passed()) {
                slider.detail(detail,sizeof(detail)); complete(slider.passed() ? Result::Pass : Result::Fail,detail);
            }
            break;
        }
        case Accel: {
            if (expired(now,stepAt,kMotionTimeoutMs)) { accel.detail(detail,sizeof(detail)); complete(Result::Fail,detail); break; }
            // HAL also consumes data-ready, so read the live register values
            // with checked transactions rather than waiting on that flag.
            uint8_t who = 0, ctrl1 = 0, ctrl4 = 0, raw[6];
            if (!readRegisters(kAccelAddress,0x0F,&who,1) || who != 0x33 ||
                !readRegisters(kAccelAddress,0x20,&ctrl1,1) || !readRegisters(kAccelAddress,0x23,&ctrl4,1) ||
                !(ctrl1 & 0xF0) || !readRegisters(kAccelAddress,0xA8,raw,6)) {
                accel.detail(detail,sizeof(detail));
                char reason[kDetailSize]; snprintf(reason,sizeof(reason),"sensor unavailable; %.160s",detail);
                complete(Result::Fail,reason); break;
            }
            int mg[3];
            for (unsigned axis=0;axis<3;++axis) mg[axis] = accelMg(int16_t(uint16_t(raw[axis*2]) | uint16_t(raw[axis*2+1])<<8),ctrl1,ctrl4);
            accel.sample(mg[0],mg[1],mg[2]);
            if (accel.passed()) {
                accel.detail(detail,sizeof(detail)); complete(accel.passed() ? Result::Pass : Result::Fail,detail);
            }
            break;
        }
        case Audio: updateAudio(now); break;
        default: break;
        }
    }
    draw();
}
void end() {
    if (running) finish(true);
    for (int i=0;i<6;++i) HAL::buttonManager().unregisterCallback(i);
    quiet(); releaseMic(); prefs.end(); storageReady = false;
    HAL::displayProxy().setOverlayMode(savedOverlay);
    setColorsOff(); HAL::showRgbLeds(); active = false;
}
void command(const char* args) {
    if (!strcmp(args,"start")) {
        if (running) { Serial.println("[err] health.start=busy"); return; }
        if (AppManager::instance().activeApp() == APP_SELF_TEST) { end(); begin(); }
        else AppManager::instance().switchToApp(APP_SELF_TEST);
        Serial.println("[cmd] health.start=ok");
    } else if (!strcmp(args,"status")) {
        if (!running) Serial.println("[cmd] health.status=idle");
        else Serial.printf("[cmd] health.status=%s state=%s\n",id(step),phase == Phase::Result ? "result" : phase == Phase::Answer ? "answer" : phase == Phase::PatternPrompt ? "prompt" : "running");
    } else if (!strcmp(args,"answer pass") || !strcmp(args,"answer fail")) {
        if (answer(!strcmp(args,"answer pass"))) Serial.println("[cmd] health.answer=ok");
        else Serial.println("[err] health.answer=not_awaiting_answer");
    } else if (!strcmp(args,"abort")) {
        if (running) { finish(true); Serial.println("[cmd] health.abort=ok"); }
        else Serial.println("[err] health.abort=idle");
    } else if (!strcmp(args,"report")) {
        Preferences stored;
        if (!stored.begin("selftest",true)) { Serial.println("[err] health.report=none"); return; }
        size_t bytes = stored.getBytesLength("report");
        bool ok = bytes && bytes <= sizeof(blob) && stored.getBytes("report",blob,bytes) == bytes;
        stored.end(); Report last;
        if (ok && decode(blob,bytes,last)) emitReport(last);
        else Serial.println("[err] health.report=none_or_invalid");
    } else Serial.println("[err] health.usage=start|status|answer pass|answer fail|report|abort");
}
} // namespace SelfTest
