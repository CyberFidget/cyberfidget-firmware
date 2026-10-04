// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2026 Dismo Industries LLC
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <utility>
#include <unity.h>
#include "../../wasm/device_module/shims/DisplayProxy.h"
const uint8_t ArialMT_Plain_10[]={0};
const uint8_t ArialMT_Plain_16[]={0};
const uint8_t ArialMT_Plain_24[]={0};
void yield() {}
#include "../test_emu_drawing/thingpulse_reference.h"
static uint8_t actual[1024];
static ThingPulseReference host;
extern "C" void cf_display_draw_hline(int32_t x,int32_t y,int32_t length) {
    host.drawHorizontalLine(x,y,length);
}
static void test_guest_triangle_matches_native() {
    ThingPulseReference expected; DisplayProxy d; std::mt19937 rng(0x51a7);
    const int16_t special[][6]={{66,46,81,19,118,42},{1,5,50,5,20,5},{10,2,10,40,10,60},{2,2,2,2,2,2},{-20,-10,140,0,64,80},{30,10,20,50,70,50},{20,20,70,20,40,50}};
    for (int color=0;color<3;++color) for (int i=0;i<2007;++i) {
        expected.clear(); host.clear(); expected.setColor(WHITE); host.setColor(WHITE);
        for (int y=0;y<64;++y) for (int x=0;x<128;++x) if ((x+3*y)%5==0) { expected.setPixel(x,y); host.setPixel(x,y); }
        expected.setColor((OLEDDISPLAY_COLOR)color); host.setColor((OLEDDISPLAY_COLOR)color);
        int16_t v[6]; for (int k=0;k<6;++k) v[k]=i<7?special[i][k]:(int)(rng()%(k%2?121:201))-30;
        expected.fillTriangle(v[0],v[1],v[2],v[3],v[4],v[5]); d.fillTriangle(v[0],v[1],v[2],v[3],v[4],v[5]);
        memcpy(actual,host.buffer,1024);
        TEST_ASSERT_EQUAL_MEMORY_MESSAGE(expected.buffer,actual,1024,"actual guest header vs native ThingPulse");
    }
}
void setUp() {}
void tearDown() {}
int main(int,char**) { UNITY_BEGIN(); RUN_TEST(test_guest_triangle_matches_native); return UNITY_END(); }
