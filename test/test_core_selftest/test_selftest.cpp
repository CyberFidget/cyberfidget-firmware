// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#include <unity.h>
#include <cmath>
#include "../../lib/SelfTest/SelfTestCore.h"
using namespace SelfTestCore;
void setUp() {}
void tearDown() {}

static void run_order_preserves_report_indexes_and_ids() {
    const unsigned expected[] = {Charger, Battery, Audio, Buttons, Slider, Accel, Display, Leds};
    const char* const storedIds[] = {"charger", "battery", "buttons", "slider", "accel", "audio", "display", "leds"};
    const unsigned storedValues[] = {Charger, Battery, Buttons, Slider, Accel, Audio, Display, Leds};
    for (unsigned i=0; i<kSteps; ++i) {
        TEST_ASSERT_EQUAL_UINT(i, storedValues[i]);
        TEST_ASSERT_EQUAL_STRING(storedIds[i], id(i));
        TEST_ASSERT_EQUAL_UINT(expected[i], runStep(i));
        TEST_ASSERT_EQUAL_UINT(i, runIndex(expected[i]));
    }
    TEST_ASSERT_EQUAL_UINT(kSteps, runStep(kSteps));
    TEST_ASSERT_EQUAL_UINT(kSteps, runIndex(kSteps));
}
static void ready_wait_has_no_deadline_or_release_only_start() {
    Phase phase = Phase::Ready;
    uint32_t phaseAt = 0;
    ButtonsState buttons;
    // Simulate six hours, then the millis wrap, without a physical press.
    const uint32_t times[] = {0, 15000, 360001, 21600000, UINT32_MAX, 0, 15000};
    for (uint32_t now : times) {
        TEST_ASSERT_FALSE(updateReady(phase, phaseAt, -1, true, now, buttons));
        TEST_ASSERT_TRUE(phase == Phase::Ready);
        TEST_ASSERT_FALSE(updateReady(phase, phaseAt, -1, false, now, buttons));
        TEST_ASSERT_TRUE(phase == Phase::Ready);
    }
    TEST_ASSERT_FALSE(updateReady(phase, phaseAt, 6, false, 15000, buttons));
    TEST_ASSERT_TRUE(phase == Phase::Ready);
    phase = Phase::Done;
    TEST_ASSERT_FALSE(updateReady(phase, phaseAt, 0, true, 15000, buttons));
    TEST_ASSERT_TRUE(phase == Phase::Done);
}
static void every_button_starts_only_after_all_are_released() {
    for (int button=0; button<6; ++button) {
        Phase phase = Phase::Ready;
        uint32_t phaseAt = 0;
        ButtonsState buttons;
        buttons.target = 4; buttons.wrong[button] = 7; buttons.since = 100;
        TEST_ASSERT_FALSE(updateReady(phase, phaseAt, button, false, 21600000, buttons));
        TEST_ASSERT_TRUE(phase == Phase::ReadyRelease);
        // Another button, or a held starting button, keeps the timer off
        // until the bounded release wait expires.
        for (unsigned tick=0; tick<100; ++tick) {
            TEST_ASSERT_FALSE(updateReady(phase, phaseAt, (button+1)%6, false, 21600000+tick*100, buttons));
            TEST_ASSERT_TRUE(phase == Phase::ReadyRelease);
        }
        TEST_ASSERT_FALSE(updateReady(phase, phaseAt, -1, true, 21610000, buttons));
        TEST_ASSERT_TRUE(phase == Phase::ReadySettle);
        TEST_ASSERT_FALSE(updateReady(phase, phaseAt, -1, true, 21610149, buttons));
        TEST_ASSERT_TRUE(updateReady(phase, phaseAt, -1, true, 21610150, buttons));
        TEST_ASSERT_TRUE(phase == Phase::Running);
        TEST_ASSERT_EQUAL_UINT(0, buttons.target);
        for (unsigned i=0; i<6; ++i) TEST_ASSERT_EQUAL_UINT(0, buttons.wrong[i]);
        TEST_ASSERT_EQUAL_UINT32(21610150, buttons.since);
        TEST_ASSERT_FALSE(buttons.timedOut(21610150));
        TEST_ASSERT_FALSE(buttons.timedOut(21625149));
        TEST_ASSERT_TRUE(buttons.timedOut(21625150));
        TEST_ASSERT_TRUE(buttons.press(ButtonsState::index(0), 21610151));
        TEST_ASSERT_EQUAL_UINT(1, buttons.target);
    }
}
static void ready_idle_sleep_depends_on_launch_source() {
    const Phase phases[] = {Phase::Running, Phase::Result, Phase::PatternPrompt, Phase::Answer,
                            Phase::Ready, Phase::ReadyRelease, Phase::ReadySettle, Phase::Done};
    for (Phase phase : phases) {
        TEST_ASSERT_TRUE(holdsIdleSleep(true, phase, true));
        TEST_ASSERT_EQUAL(!waiting(phase), holdsIdleSleep(true, phase, false));
        TEST_ASSERT_FALSE(holdsIdleSleep(false, phase, true));
        TEST_ASSERT_FALSE(holdsIdleSleep(false, phase, false));
    }
}
static void ready_release_timeout_leaves_stuck_button_unchecked() {
    const uint32_t starts[] = {100, UINT32_MAX-10000};
    for (uint32_t start : starts) for (int stuck=0; stuck<6; ++stuck) {
        Phase phase = Phase::Ready;
        uint32_t phaseAt = 0;
        ButtonsState buttons;
        TEST_ASSERT_FALSE(updateReady(phase, phaseAt, stuck, false, start, buttons));
        TEST_ASSERT_EQUAL_UINT32(start, phaseAt);
        TEST_ASSERT_FALSE(updateReady(phase, phaseAt, -1, false, start+kButtonTimeoutMs-1, buttons));
        TEST_ASSERT_TRUE(phase == Phase::ReadyRelease);
        const uint32_t began = start+kButtonTimeoutMs;
        TEST_ASSERT_TRUE(updateReady(phase, phaseAt, -1, false, began, buttons));
        TEST_ASSERT_TRUE(phase == Phase::Running);
        TEST_ASSERT_EQUAL_UINT(0, buttons.target);
        TEST_ASSERT_EQUAL_UINT32(began, buttons.since);
        for (unsigned i=0; i<6; ++i) TEST_ASSERT_EQUAL_UINT(0, buttons.wrong[i]);
        // The stuck button produces no new Pressed edge. Other checks can
        // pass, but its own check must reach the normal Buttons deadline.
        while (ButtonsState::index(buttons.target) != stuck) {
            TEST_ASSERT_TRUE(buttons.press(ButtonsState::index(buttons.target), began+1));
        }
        const unsigned missing = buttons.target;
        TEST_ASSERT_FALSE(buttons.timedOut(buttons.since+kButtonTimeoutMs-1));
        const uint32_t deadline = buttons.since+kButtonTimeoutMs;
        TEST_ASSERT_TRUE(buttons.timedOut(deadline));
        TEST_ASSERT_FALSE(buttons.press(stuck, deadline));
        TEST_ASSERT_EQUAL_UINT(missing, buttons.target);
        char detail[kDetailSize]; buttons.detail(detail, sizeof(detail));
        TEST_ASSERT_NOT_NULL(strstr(detail, "missing "));
        TEST_ASSERT_NOT_NULL(strstr(detail, ButtonsState::label(missing)));
    }
}
static void ready_release_requires_continuous_settle() {
    const uint32_t starts[] = {100, UINT32_MAX-100};
    for (uint32_t start : starts) {
        Phase phase = Phase::Ready;
        uint32_t phaseAt = 0;
        ButtonsState buttons;
        TEST_ASSERT_FALSE(updateReady(phase, phaseAt, 0, false, start, buttons));
        TEST_ASSERT_FALSE(updateReady(phase, phaseAt, -1, true, start+10, buttons));
        TEST_ASSERT_TRUE(phase == Phase::ReadySettle);
        // A release bounce is swallowed and restarts the settling interval.
        TEST_ASSERT_FALSE(updateReady(phase, phaseAt, 0, false, start+159, buttons));
        TEST_ASSERT_TRUE(phase == Phase::ReadyRelease);
        TEST_ASSERT_EQUAL_UINT(0, buttons.target);
        TEST_ASSERT_FALSE(updateReady(phase, phaseAt, -1, true, start+200, buttons));
        TEST_ASSERT_FALSE(updateReady(phase, phaseAt, -1, true, start+349, buttons));
        TEST_ASSERT_TRUE(updateReady(phase, phaseAt, -1, true, start+350, buttons));
        TEST_ASSERT_EQUAL_UINT32(start+350, buttons.since);
        TEST_ASSERT_EQUAL_UINT(0, buttons.target);
        TEST_ASSERT_FALSE(buttons.timedOut(start+350+kButtonTimeoutMs-1));
        TEST_ASSERT_TRUE(buttons.timedOut(start+350+kButtonTimeoutMs));
    }
    Phase phase = Phase::Ready;
    uint32_t phaseAt = 0;
    ButtonsState buttons;
    TEST_ASSERT_FALSE(updateReady(phase, phaseAt, 0, false, 0, buttons));
    TEST_ASSERT_FALSE(updateReady(phase, phaseAt, -1, true, kButtonTimeoutMs-1, buttons));
    // Settling cannot extend the maximum wait from the starting press.
    TEST_ASSERT_TRUE(updateReady(phase, phaseAt, -1, false, kButtonTimeoutMs, buttons));
    TEST_ASSERT_EQUAL_UINT32(kButtonTimeoutMs, buttons.since);
    TEST_ASSERT_EQUAL_UINT(0, buttons.target);
}
static void abort_uses_run_order_and_preserves_completed_audio() {
    for (unsigned i=0; i<kSteps; ++i) {
        TEST_ASSERT_EQUAL_UINT(i, firstUnfinished(runStep(i), Phase::Running));
        TEST_ASSERT_EQUAL_UINT(i+1, firstUnfinished(runStep(i), Phase::Result));
    }
    const Phase waitingPhases[] = {Phase::Ready, Phase::ReadyRelease, Phase::ReadySettle};
    for (Phase phase : waitingPhases) {
        TEST_ASSERT_EQUAL_UINT(3, firstUnfinished(Audio, phase));
        TEST_ASSERT_EQUAL_UINT(Buttons, runStep(firstUnfinished(Audio, phase)));
    }
    TEST_ASSERT_EQUAL_UINT(kSteps, firstUnfinished(Leds, Phase::Result));
}
static void ready_serial_lines_and_existing_status_formats() {
    char line[512];
    const Phase waitingPhases[] = {Phase::Ready, Phase::ReadyRelease, Phase::ReadySettle};
    for (Phase phase : waitingPhases) {
        TEST_ASSERT_TRUE(statusLine(Audio, phase, true, line, sizeof(line)));
        TEST_ASSERT_EQUAL_STRING("[cmd] health.status=ready state=ready", line);
        TEST_ASSERT_TRUE(promptLine(Audio, phase, kReadyPrompt, line, sizeof(line)));
        TEST_ASSERT_EQUAL_STRING("[cmd] health.step=ready prompt=Press%20any%20button%20to%20start", line);
    }
    const Phase phases[] = {Phase::Running, Phase::Result, Phase::PatternPrompt, Phase::Answer};
    const char* const expected[] = {
        "[cmd] health.status=buttons state=running", "[cmd] health.status=buttons state=result",
        "[cmd] health.status=buttons state=prompt", "[cmd] health.status=buttons state=answer"
    };
    for (unsigned i=0; i<4; ++i) {
        TEST_ASSERT_TRUE(statusLine(Buttons, phases[i], true, line, sizeof(line)));
        TEST_ASSERT_EQUAL_STRING(expected[i], line);
    }
    TEST_ASSERT_TRUE(statusLine(Audio, Phase::Ready, false, line, sizeof(line)));
    TEST_ASSERT_EQUAL_STRING("[cmd] health.status=idle", line);
    TEST_ASSERT_TRUE(promptLine(Buttons, Phase::Running, "Press Up (top left)", line, sizeof(line)));
    TEST_ASSERT_EQUAL_STRING("[cmd] health.step=buttons prompt=Press%20Up%20%28top%20left%29", line);
    TEST_ASSERT_FALSE(statusLine(Audio, Phase::Ready, true, line, 5));
    TEST_ASSERT_FALSE(promptLine(Audio, Phase::Ready, kReadyPrompt, line, 5));
}
static void gauge_requires_response_and_positive_voltage() {
    TEST_ASSERT_TRUE(charger(true, 1) == Result::Pass);
    TEST_ASSERT_TRUE(charger(true, 0) == Result::Fail);
    TEST_ASSERT_TRUE(charger(false, 4200) == Result::Fail);
}
static void buttons_order_wrong_press_and_deadline() {
    ButtonsState b; b.since = 100;
    TEST_ASSERT_FALSE(b.press(5, 101));
    TEST_ASSERT_EQUAL(1, b.wrong[5]);
    for (unsigned i = 0; i < 6; ++i) TEST_ASSERT_TRUE(b.press(ButtonsState::index(i), 200+i));
    TEST_ASSERT_EQUAL(6, b.target);
    TEST_ASSERT_FALSE(b.timedOut(100000));
    char detail[kDetailSize]; b.detail(detail, sizeof(detail));
    TEST_ASSERT_NOT_NULL(strstr(detail, "Enter:1"));
    b = ButtonsState{}; b.since = 100;
    TEST_ASSERT_FALSE(b.timedOut(15099));
    TEST_ASSERT_TRUE(b.timedOut(15100));
    TEST_ASSERT_FALSE(b.press(0, 15100));
    b.detail(detail, sizeof(detail)); TEST_ASSERT_NOT_NULL(strstr(detail, "missing Up"));
}
static void deadlines_wrap_safely() {
    TEST_ASSERT_FALSE(expired(998, UINT32_MAX-1000, 2000));
    TEST_ASSERT_TRUE(expired(999, UINT32_MAX-1000, 2000));
    // A step that started later in the same tick than `now` was read has not timed out.
    TEST_ASSERT_FALSE(expired(5000, 5003, 15000));
    ButtonsState fresh; fresh.since = 5003;
    TEST_ASSERT_FALSE(fresh.timedOut(5000));
    TEST_ASSERT_EQUAL(3000, kAbortHoldMs);
}
static void slider_requires_both_ends() {
    SliderState s; TEST_ASSERT_FALSE(s.passed());
    s.sample(4, 0); s.sample(96, 2000); TEST_ASSERT_FALSE(s.passed());
    // Touching an end is not enough: it has to stay there for a second.
    s.sample(3, 3000); s.sample(1, 3999); TEST_ASSERT_FALSE(s.lowHeld);
    s.sample(0, 4000); TEST_ASSERT_TRUE(s.lowHeld); TEST_ASSERT_FALSE(s.passed());
    s.sample(97, 5000); s.sample(99, 5500); TEST_ASSERT_FALSE(s.passed());
    // Leaving the end band restarts the hold.
    s.sample(96, 5600); s.sample(98, 5700); s.sample(100, 6600); TEST_ASSERT_FALSE(s.passed());
    s.sample(100, 6700); TEST_ASSERT_TRUE(s.passed());
    char detail[64]; s.detail(detail, sizeof(detail)); TEST_ASSERT_EQUAL_STRING("range 0-100%", detail);
    // Holds survive a millis() wrap.
    s = SliderState{}; s.sample(100, 0xFFFFFF00u); s.sample(100, 0x000002F0u); TEST_ASSERT_TRUE(s.highHeld);
    s = SliderState{}; s.sample(-1, 0); s.sample(101, 5000); TEST_ASSERT_FALSE(s.seen);
}
static void accel_requires_two_sign_changes_and_all_axes() {
    AccelState a; a.sample(0,0,1000); a.sample(350,0,-1000); a.sample(0,0,1000);
    TEST_ASSERT_FALSE(a.passed());
    a.sample(0,350,1000); TEST_ASSERT_TRUE(a.passed());
    a = AccelState{}; a.sample(0,0,1000); a.sample(400,400,-1000); TEST_ASSERT_FALSE(a.passed());
    a.sample(0,0,1000); TEST_ASSERT_TRUE(a.passed());
}
static void accel_rejects_sign_noise_and_stationary_input() {
    AccelState a;
    for (int i = 0; i < 100; ++i) a.sample(0,0, i%2 ? -499 : 499);
    TEST_ASSERT_FALSE(a.passed()); TEST_ASSERT_EQUAL(0, a.changes);
    a.sample(0,0,1000); a.sample(0,0,1000); TEST_ASSERT_FALSE(a.passed());
}
static void accel_conversion_preserves_sensor_mode_and_range() {
    TEST_ASSERT_EQUAL(1000,accelMg(16000,0x70,0x08));
    TEST_ASSERT_EQUAL(-1000,accelMg(-16000,0x70,0x08));
    TEST_ASSERT_EQUAL(2000,accelMg(16000,0x70,0x18));
    TEST_ASSERT_EQUAL(4000,accelMg(16000,0x70,0x28));
    TEST_ASSERT_EQUAL(12000,accelMg(16000,0x70,0x38));
    TEST_ASSERT_EQUAL(1000,accelMg(16000,0x70,0));
    TEST_ASSERT_EQUAL(992,accelMg(16000,0x78,0));
}
static void person_confirmation_requires_every_answer() {
    ConfirmState s; TEST_ASSERT_FALSE(s.passed(3));
    char partial[100]; s.record(false,false); s.detail(true,partial,sizeof(partial));
    TEST_ASSERT_EQUAL_STRING("cycle=fail off=not_checked",partial);
    s = ConfirmState{};
    s.record(true,false); s.record(true,false); TEST_ASSERT_FALSE(s.passed(3));
    s.record(true,false); TEST_ASSERT_TRUE(s.passed(3));
    // LEDs: two screens, color cycle then all off.
    s = ConfirmState{}; s.record(true,false); s.record(false,false);
    TEST_ASSERT_FALSE(s.passed(2)); char detail[100]; s.detail(true,detail,sizeof(detail));
    TEST_ASSERT_EQUAL_STRING("cycle=ok off=fail",detail);
    s = ConfirmState{}; s.record(true,false); s.record(true,false); TEST_ASSERT_TRUE(s.passed(2));
    s = ConfirmState{}; s.record(true,false); s.record(false,true); s.record(true,false);
    TEST_ASSERT_FALSE(s.passed(3)); s.detail(false,detail,sizeof(detail));
    TEST_ASSERT_EQUAL_STRING("white=ok checker=timeout off=ok",detail);
}
static void loopback_margin_and_floor() {
    TEST_ASSERT_FALSE(loopbackPass(0,0)); TEST_ASSERT_FALSE(loopbackPass(0,3));
    TEST_ASSERT_TRUE(loopbackPass(0,4));
    TEST_ASSERT_FALSE(loopbackPass(100,399)); TEST_ASSERT_TRUE(loopbackPass(100,400));
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, ratioMilli(0,UINT32_MAX));
}
static uint32_t measure(unsigned hz, unsigned amplitude) {
    ToneMeter m;
    for (unsigned i = 0; i < 4096; ++i) m.sample(int16_t(amplitude * sin(2*3.141592653589793*hz*i/kMicRate)));
    TEST_ASSERT_TRUE(m.ready()); return m.level();
}
static void tone_meter_selects_frequency_and_handles_clipping() {
    uint32_t on = measure(kToneHz, 16000), off = measure(2000, 16000);
    TEST_ASSERT_GREATER_THAN_UINT32(1000, on);
    TEST_ASSERT_TRUE(loopbackPass(off, on));
    TEST_ASSERT_EQUAL_UINT32(0, measure(kToneHz, 0));
    TEST_ASSERT_GREATER_THAN_UINT32(on, measure(kToneHz,32767));
    ToneMeter m; for (unsigned i=0; i<4096; ++i) m.sample(i%2 ? INT16_MIN : INT16_MAX);
    TEST_ASSERT_TRUE(m.ready()); TEST_ASSERT_LESS_THAN_UINT32(100, m.level());
}
static Report example() {
    Report r; r.run = 42; snprintf(r.firmware,sizeof(r.firmware),"1.2.3+abc");
    for (auto& s:r.steps) { s.result = Result::Pass; snprintf(s.detail,sizeof(s.detail),"measured"); }
    r.steps[Battery].result = Result::Skip;
    snprintf(r.steps[Battery].detail,kDetailSize,"no battery check yet");
    r.judge(false); return r;
}
static void stored_report_and_summary_keep_the_original_order() {
    Report r = example(); char buf[kReportBytes];
    for (unsigned s=0; s<kSteps; ++s) snprintf(r.steps[s].detail,kDetailSize,"%s",id(s));
    TEST_ASSERT_GREATER_THAN(0, encode(r,buf,sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("CFHEALTH1|42|pass|1.2.3%2Babc\n"
        "pass|charger\nskip|battery\npass|buttons\npass|slider\npass|accel\npass|audio\npass|display\npass|leds\n", buf);
    r.steps[Buttons].result = Result::Fail;
    TEST_ASSERT_TRUE(summaryLine(r,"0123456789ab",buf,sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("[cmd] health.summary id=0123456789ab fw=1.2.3%2Babc run=42 charger=pass battery=skip buttons=fail slider=pass accel=pass audio=pass display=pass leds=pass", buf);
}
static void overall_never_passes_unmeasured() {
    Report r; r.judge(false); TEST_ASSERT_TRUE(r.overall == Overall::Fail);
    r = example(); TEST_ASSERT_TRUE(r.overall == Overall::Pass);
    r.steps[Audio].result = Result::Fail; r.judge(false); TEST_ASSERT_TRUE(r.overall == Overall::Fail);
    r.judge(true); TEST_ASSERT_TRUE(r.overall == Overall::Aborted);
}
static void serial_tokens_are_lossless_and_bounded() {
    char encoded[64], raw[32];
    TEST_ASSERT_TRUE(token("range 0-100%;x=ok",encoded,sizeof(encoded)));
    TEST_ASSERT_NULL(strchr(encoded,' ')); TEST_ASSERT_NOT_NULL(strstr(encoded,"%20"));
    TEST_ASSERT_TRUE(untoken(encoded,raw,sizeof(raw))); TEST_ASSERT_EQUAL_STRING("range 0-100%;x=ok",raw);
    TEST_ASSERT_FALSE(token(" ",encoded,3)); TEST_ASSERT_FALSE(untoken("%",raw,sizeof(raw)));
    TEST_ASSERT_FALSE(untoken("%GG",raw,sizeof(raw))); TEST_ASSERT_FALSE(untoken("%00",raw,sizeof(raw)));
    TEST_ASSERT_FALSE(untoken("%0A",raw,sizeof(raw)));
}
static void report_roundtrip_and_line_formats() {
    Report r = example(), parsed; char buf[kReportBytes];
    size_t n = encode(r,buf,sizeof(buf)); TEST_ASSERT_GREATER_THAN(0,n);
    TEST_ASSERT_TRUE(decode(buf,n,parsed)); TEST_ASSERT_EQUAL(42,parsed.run);
    TEST_ASSERT_EQUAL_STRING(r.firmware,parsed.firmware);
    TEST_ASSERT_EQUAL_STRING("no battery check yet",parsed.steps[Battery].detail);
    TEST_ASSERT_TRUE(stepLine(Battery,parsed.steps[Battery],buf,sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("[cmd] health.battery=skip detail=no%20battery%20check%20yet",buf);
    TEST_ASSERT_TRUE(summaryLine(parsed,"0123456789ab",buf,sizeof(buf)));
    TEST_ASSERT_NOT_NULL(strstr(buf,"id=0123456789ab")); TEST_ASSERT_NOT_NULL(strstr(buf,"battery=skip"));
    TEST_ASSERT_FALSE(summaryLine(parsed,"id",buf,5));
}
static void report_rejects_truncation_and_invalid_storage() {
    Report r = example(), unchanged; unchanged.run = 99; char buf[kReportBytes];
    size_t n = encode(r,buf,sizeof(buf));
    for (size_t i=0;i<n;++i) TEST_ASSERT_FALSE(decode(buf,i,unchanged));
    TEST_ASSERT_EQUAL(99,unchanged.run);
    buf[0]='X'; TEST_ASSERT_FALSE(decode(buf,n,unchanged));
    TEST_ASSERT_EQUAL_UINT32(0,encode(r,buf,8));
    r.steps[Audio].result = Result::Fail; // inconsistent pass summary
    n=encode(r,buf,sizeof(buf)); TEST_ASSERT_FALSE(decode(buf,n,unchanged));
    r.judge(true); n=encode(r,buf,sizeof(buf)); TEST_ASSERT_TRUE(decode(buf,n,unchanged));
    TEST_ASSERT_TRUE(unchanged.overall == Overall::Aborted);
}
static void maximum_report_fits_and_roundtrips() {
    Report r = example(), parsed; r.run = UINT32_MAX;
    memset(r.firmware,'%',sizeof(r.firmware)-1);
    for (auto& s:r.steps) { memset(s.detail,'=',sizeof(s.detail)-1); s.detail[sizeof(s.detail)-1]=0; }
    char buf[kReportBytes]; size_t n=encode(r,buf,sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0,n); TEST_ASSERT_TRUE(decode(buf,n,parsed));
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX,parsed.run);
    TEST_ASSERT_EQUAL_STRING(r.steps[Leds].detail,parsed.steps[Leds].detail);
    TEST_ASSERT_EQUAL_STRING(r.firmware,parsed.firmware);
}
int main() {
    UNITY_BEGIN();
    RUN_TEST(run_order_preserves_report_indexes_and_ids);
    RUN_TEST(ready_wait_has_no_deadline_or_release_only_start);
    RUN_TEST(every_button_starts_only_after_all_are_released);
    RUN_TEST(ready_idle_sleep_depends_on_launch_source);
    RUN_TEST(ready_release_timeout_leaves_stuck_button_unchecked);
    RUN_TEST(ready_release_requires_continuous_settle);
    RUN_TEST(abort_uses_run_order_and_preserves_completed_audio);
    RUN_TEST(ready_serial_lines_and_existing_status_formats);
    RUN_TEST(stored_report_and_summary_keep_the_original_order);
    RUN_TEST(gauge_requires_response_and_positive_voltage);
    RUN_TEST(buttons_order_wrong_press_and_deadline);
    RUN_TEST(deadlines_wrap_safely);
    RUN_TEST(slider_requires_both_ends);
    RUN_TEST(accel_requires_two_sign_changes_and_all_axes);
    RUN_TEST(accel_rejects_sign_noise_and_stationary_input);
    RUN_TEST(accel_conversion_preserves_sensor_mode_and_range);
    RUN_TEST(person_confirmation_requires_every_answer);
    RUN_TEST(loopback_margin_and_floor);
    RUN_TEST(tone_meter_selects_frequency_and_handles_clipping);
    RUN_TEST(overall_never_passes_unmeasured);
    RUN_TEST(serial_tokens_are_lossless_and_bounded);
    RUN_TEST(report_roundtrip_and_line_formats);
    RUN_TEST(report_rejects_truncation_and_invalid_storage);
    RUN_TEST(maximum_report_fits_and_roundtrips);
    return UNITY_END();
}
