// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#include <unity.h>
#include <cmath>
#include "../../lib/SelfTest/SelfTestCore.h"
using namespace SelfTestCore;
void setUp() {}
void tearDown() {}

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
    s.sample(4); s.sample(96); TEST_ASSERT_FALSE(s.passed());
    s.sample(3); TEST_ASSERT_FALSE(s.passed()); s.sample(97); TEST_ASSERT_TRUE(s.passed());
    char detail[64]; s.detail(detail, sizeof(detail)); TEST_ASSERT_EQUAL_STRING("range 3-97%", detail);
    s = SliderState{}; s.sample(100); s.sample(0); TEST_ASSERT_TRUE(s.passed());
    s = SliderState{}; s.sample(-1); s.sample(101); TEST_ASSERT_FALSE(s.seen);
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
    TEST_ASSERT_EQUAL_STRING("red=fail green=not_checked blue=not_checked white=not_checked",partial);
    s = ConfirmState{};
    s.record(true,false); s.record(true,false); TEST_ASSERT_FALSE(s.passed(3));
    s.record(true,false); TEST_ASSERT_TRUE(s.passed(3));
    s = ConfirmState{}; s.record(true,false); s.record(false,false); s.record(true,false); s.record(true,false);
    TEST_ASSERT_FALSE(s.passed(4)); char detail[100]; s.detail(true,detail,sizeof(detail));
    TEST_ASSERT_EQUAL_STRING("red=ok green=fail blue=ok white=ok",detail);
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
