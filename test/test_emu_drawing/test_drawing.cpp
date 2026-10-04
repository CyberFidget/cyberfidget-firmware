// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Dismo Industries LLC
#include <unity.h>
#include <cstdio>
#include <random>
#include "Arduino.h"
#include "SSD1306Wire.h"
#include "OLEDDisplayFonts_real.h"
const uint8_t* ArialMT_Plain_10 = ArialMT_Plain_10_data;
const uint8_t* ArialMT_Plain_16 = ArialMT_Plain_16_data;
const uint8_t* ArialMT_Plain_24 = ArialMT_Plain_24_data;
void yield() {}
#include "thingpulse_reference.h"

static uint8_t pushed[8192];
static int pushCount;
extern "C" void js_push_framebuffer(const uint8_t* b, int n) {
    TEST_ASSERT_EQUAL_INT(8192, n);
    memcpy(pushed,b,n); ++pushCount;
}
static void pack(const SSD1306Wire& d, uint8_t* out) {
    const auto* pixels=d.getBuffer();
    memset(out,0,1024);
    for (int y=0;y<64;++y) for (int x=0;x<128;++x)
        out[(y>>3)*128+x] |= pixels[y*128+x] << (y&7);
}
static void equal(const ThingPulseReference& r, const SSD1306Wire& d, const char* context) {
    uint8_t actual[1024]; pack(d,actual);
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(r.buffer,actual,1024,context);
}
static void prepare(ThingPulseReference& r, SSD1306Wire& d, int color) {
    r.clear(); d.clear();
    // Mixed background exposes BLACK erasure and INVERSE write multiplicity.
    r.setColor(WHITE); d.setColor(WHITE);
    for (int y=0;y<64;++y) for (int x=0;x<128;++x) if ((x+3*y)%5==0) {
        r.setPixel(x,y); d.setPixel(x,y);
    }
    r.setColor((OLEDDISPLAY_COLOR)color); d.setColor((OLEDDISPLAY_COLOR)color);
}
static constexpr int centers[][2]={{64,32},{0,0},{127,63},{0,32},{64,0},{-12,20},{138,40},{60,-10},{60,75},{-40,-40}};
static void test_circles() {
    ThingPulseReference r; SSD1306Wire d;
    for (int color=0;color<3;++color) for (auto& c:centers) for (int radius=-3;radius<=31;++radius) {
        char context[100]; snprintf(context,sizeof(context),"circle c=%d center=%d,%d radius=%d",color,c[0],c[1],radius);
        prepare(r,d,color); r.drawCircle(c[0],c[1],radius); d.drawCircle(c[0],c[1],radius); equal(r,d,context);
        prepare(r,d,color); r.fillCircle(c[0],c[1],radius); d.fillCircle(c[0],c[1],radius); equal(r,d,context);
    }
}
static void test_circle_quadrants() {
    ThingPulseReference r; SSD1306Wire d;
    for (int color=0;color<3;++color) for (auto& c:centers) for (int radius=0;radius<=31;++radius) for (int q=0;q<16;++q) {
        prepare(r,d,color); r.drawCircleQuads(c[0],c[1],radius,q); d.drawCircleQuads(c[0],c[1],radius,q); equal(r,d,"circle quadrants");
    }
}
static void test_triangles() {
    ThingPulseReference r; SSD1306Wire d; std::mt19937 rng(0x51a7);
    const int16_t special[][6]={{3,48,22,18,50,45},{66,46,81,19,118,42},{1,5,50,5,20,5},{10,2,10,40,10,60},{2,2,2,2,2,2},{-20,-10,140,0,64,80},{30,10,20,50,70,50},{20,20,70,20,40,50}};
    for (int color=0;color<3;++color) for (int i=0;i<2008;++i) {
        int16_t v[6];
        for (int k=0;k<6;++k) v[k]=i<8 ? special[i][k] : (int)(rng()%(k%2 ? 121:201))-30;
        char context[160]; snprintf(context,sizeof(context),"triangle c=%d vertices=%d,%d %d,%d %d,%d",color,v[0],v[1],v[2],v[3],v[4],v[5]);
        prepare(r,d,color); r.fillTriangle(v[0],v[1],v[2],v[3],v[4],v[5]); d.fillTriangle(v[0],v[1],v[2],v[3],v[4],v[5]); equal(r,d,context);
        prepare(r,d,color); r.drawTriangle(v[0],v[1],v[2],v[3],v[4],v[5]); d.drawTriangle(v[0],v[1],v[2],v[3],v[4],v[5]); equal(r,d,context);
    }
}
static void test_pixels_lines_rectangles() {
    ThingPulseReference r; SSD1306Wire d;
    const int sizes[]={-20,-1,0,1,2,7,8,9,31,64,128,180};
    for (int color=0;color<3;++color) for (auto& c:centers) {
        prepare(r,d,color); r.setPixel(c[0],c[1]); d.setPixel(c[0],c[1]); equal(r,d,"setPixel");
        r.clearPixel(c[0],c[1]); d.clearPixel(c[0],c[1]); equal(r,d,"clearPixel");
        for (int pc=0;pc<3;++pc) { r.setPixelColor(c[0],c[1],(OLEDDISPLAY_COLOR)pc); d.setPixelColor(c[0],c[1],(OLEDDISPLAY_COLOR)pc); equal(r,d,"setPixelColor"); }
        for (int w:sizes) {
            prepare(r,d,color); r.drawHorizontalLine(c[0],c[1],w); d.drawHorizontalLine(c[0],c[1],w); equal(r,d,"horizontal line");
            prepare(r,d,color); r.drawVerticalLine(c[0],c[1],w); d.drawVerticalLine(c[0],c[1],w); equal(r,d,"vertical line");
            for (int h:sizes) {
                prepare(r,d,color); r.drawRect(c[0],c[1],w,h); d.drawRect(c[0],c[1],w,h); equal(r,d,"rectangle");
                prepare(r,d,color); r.fillRect(c[0],c[1],w,h); d.fillRect(c[0],c[1],w,h); equal(r,d,"filled rectangle");
            }
        }
    }
}
static void test_progress_bars() {
    ThingPulseReference r; SSD1306Wire d;
    for (int color=0;color<3;++color) for (auto& c:centers) for (int h:{0,1,2,3,4,5,10,20}) for (int w:{0,1,5,30,80,128}) for (int progress:{0,1,50,99,100,101,255}) {
        prepare(r,d,color); r.drawProgressBar(c[0],c[1],w,h,progress); d.drawProgressBar(c[0],c[1],w,h,progress);
        equal(r,d,"progress bar");
        // ThingPulse intentionally leaves drawing colour WHITE.
        r.setPixel(127,63); d.setPixel(127,63); equal(r,d,"progress color state");
    }
}
static void test_images() {
    ThingPulseReference r; SSD1306Wire d; uint8_t image[1024]; std::mt19937 rng(0xb17);
    for (auto& byte:image) byte=rng();
    for (int color=0;color<3;++color) for (auto& c:centers) for (int w:{-1,0,1,7,8,9,15,16,31}) for (int h:{-1,0,1,7,8,9,16,23}) {
        prepare(r,d,color); r.drawXbm(c[0],c[1],w,h,image); d.drawXbm(c[0],c[1],w,h,image); equal(r,d,"XBM");
        prepare(r,d,color); r.drawFastImage(c[0],c[1],w,h,image); d.drawFastImage(c[0],c[1],w,h,image); equal(r,d,"fast image");
    }
    for (int color=0;color<3;++color) for (auto& c:centers) for (bool inverse:{false,true}) {
        prepare(r,d,color); r.drawIco16x16(c[0],c[1],image,inverse); d.drawIco16x16(c[0],c[1],image,inverse); equal(r,d,"opaque icon");
        r.setPixel(65,32); d.setPixel(65,32); equal(r,d,"icon color state");
    }
}
static void test_text() {
    ThingPulseReference r; SSD1306Wire d; d.init();
    const char* texts[]={"", "Width", "LEFT\nRIGHT", "one\n\nthree", "word wrap / at-dashes too", "0123456789 ABC xyz", "caf\xc3\xa9 \xc2\xa3"};
    // ThingPulse indexes before the font table for control/signed UTF-8 bytes.
    // Padding makes those reference quirks deterministic without invalid reads.
    uint8_t font10[1024+sizeof(ArialMT_Plain_10_data)]={};
    uint8_t font16[1024+sizeof(ArialMT_Plain_16_data)]={};
    uint8_t font24[1024+sizeof(ArialMT_Plain_24_data)]={};
    memcpy(font10+1024,ArialMT_Plain_10_data,sizeof(ArialMT_Plain_10_data));
    memcpy(font16+1024,ArialMT_Plain_16_data,sizeof(ArialMT_Plain_16_data));
    memcpy(font24+1024,ArialMT_Plain_24_data,sizeof(ArialMT_Plain_24_data));
    for (const uint8_t* font:{font10+1024,font16+1024,font24+1024}) {
        r.setFont(font); d.setFont(font);
        for (int color=0;color<3;++color) for (int align=0;align<4;++align) for (auto& c:centers) for (auto* text:texts) {
            r.setTextAlignment((OLEDDISPLAY_TEXT_ALIGNMENT)align); d.setTextAlignment((OLEDDISPLAY_TEXT_ALIGNMENT)align);
            prepare(r,d,color);
            TEST_ASSERT_EQUAL_UINT16(r.drawString(c[0],c[1],text),d.drawString(c[0],c[1],text)); equal(r,d,"text alignment/UTF-8/newlines");
            // Width's raw-char API retains ThingPulse's unchecked lookup semantics.
            if (strchr(text,'\n')==nullptr && (unsigned char)text[0]<128 && strchr(text,'\xc3')==nullptr) {
                TEST_ASSERT_EQUAL_UINT16(r.getStringWidth(String(text)),d.getStringWidth(String(text)));
            }
            TEST_ASSERT_EQUAL_UINT16(r.getStringWidth(text,strlen(text),true),d.getStringWidth(text,strlen(text),true));
        }
        for (int align=0;align<4;++align) for (int width:{0,1,10,31,64,128}) for (auto* text:{"", "one two three four five six", "a-long/path word", "abcdef"}) for (auto& c:centers) {
            r.setTextAlignment((OLEDDISPLAY_TEXT_ALIGNMENT)align); d.setTextAlignment((OLEDDISPLAY_TEXT_ALIGNMENT)align);
            prepare(r,d,WHITE);
            TEST_ASSERT_EQUAL_UINT16(r.drawStringMaxWidth(c[0],c[1],width,text),d.drawStringMaxWidth(c[0],c[1],width,text)); equal(r,d,"wrapped text");
        }
    }
}
static const uint8_t bitmap16[32]={0xff,0xff,0x01,0x80,0x05,0xa0,0x09,0x90,0x11,0x88,0x21,0x84,0x41,0x82,0x81,0x81,0x81,0x81,0x41,0x82,0x21,0x84,0x11,0x88,0x09,0x90,0x05,0xa0,0x01,0x80,0xff,0xff};
// The previous-guest-triangle capture was drawn by this historical guest routine.
class CapturedGuestTriangle : public SSD1306Wire {
public:
    void fillTriangle(int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                      int16_t x2, int16_t y2) {
        int16_t minY = y0 < y1 ? (y0 < y2 ? y0 : y2) : (y1 < y2 ? y1 : y2);
        int16_t maxY = y0 > y1 ? (y0 > y2 ? y0 : y2) : (y1 > y2 ? y1 : y2);
        for (int16_t y = minY; y <= maxY; y++) {
            int16_t xmin = INT16_MAX, xmax = INT16_MIN;
            scanEdge(x0, y0, x1, y1, y, xmin, xmax);
            scanEdge(x1, y1, x2, y2, y, xmin, xmax);
            scanEdge(x2, y2, x0, y0, y, xmin, xmax);
            if (xmin <= xmax) drawHorizontalLine(xmin, y, (int16_t)(xmax - xmin + 1));
        }
    }
private:
    static void scanEdge(int16_t ax, int16_t ay, int16_t bx, int16_t by,
                         int16_t y, int16_t& xmin, int16_t& xmax) {
        if (!((ay <= y && by >= y) || (by <= y && ay >= y))) return;
        if (ay == by) {
            if (ax < xmin) xmin = ax;
            if (bx < xmin) xmin = bx;
            if (ax > xmax) xmax = ax;
            if (bx > xmax) xmax = bx;
            return;
        }
        int16_t xi = (int16_t)(ax + (long)(y - ay) * (bx - ax) / (by - ay));
        if (xi < xmin) xmin = xi;
        if (xi > xmax) xmax = xi;
    }
};
template<class Display> static void scene(Display& d,int page) {
    d.clear(); d.setColor(WHITE); d.setFont(ArialMT_Plain_10); d.setTextAlignment(TEXT_ALIGN_LEFT);
    switch(page) {
    case 1:
        d.drawString(0,0,"PIXELS / COLORS"); d.fillRect(3,18,34,30);
        d.setColor(BLACK); d.fillRect(9,24,12,12); d.setColor(INVERSE); d.fillRect(17,30,14,12);
        d.setColor(WHITE); d.setPixel(42,18); d.drawLine(44,18,83,48); d.drawLine(93,48,103,17); d.drawString(2,51,"BLACK WHITE INVERSE"); break;
    case 2:
        d.drawString(0,0,"SHAPES"); d.drawRect(3,17,25,25); d.fillRect(33,19,16,20);
        d.drawCircle(67,30,12); d.fillCircle(98,30,10); d.drawHorizontalLine(4,50,45); d.drawVerticalLine(119,17,36); break;
    case 3:
        d.drawString(0,0,"TRIANGLES"); d.drawTriangle(3,48,22,18,50,45); d.fillTriangle(66,46,81,19,118,42); d.drawString(2,52,"outline      filled"); break;
    case 4: {
        d.drawString(0,0,"LEFT");
        d.setTextAlignment(TEXT_ALIGN_RIGHT); d.drawString(127,12,"RIGHT");
        d.setFont(ArialMT_Plain_16); d.setTextAlignment(TEXT_ALIGN_CENTER); d.drawString(64,22,"CENTER");
        d.setFont(ArialMT_Plain_24); d.setTextAlignment(TEXT_ALIGN_CENTER_BOTH); d.drawString(64,48,"BOTH");
        d.setFont(ArialMT_Plain_10); d.setTextAlignment(TEXT_ALIGN_LEFT);
        const int width=d.getStringWidth("Width"); d.drawString(2,12,"Width"); d.drawRect(2,23,width,3); break;
    }
    case 8:
        d.drawString(0,0,"SOUND"); d.drawString(2,14,"440 Hz / 200 ms");
        d.drawString(2,27,"3 notes: C E G"); d.drawString(2,40,"stop at 300 / 1100 ms"); break;
    case 5:
        d.drawString(0,0,"XBM / ODD WIDTH"); d.drawXbm(5,19,16,16,bitmap16); d.drawXbm(41,19,15,16,bitmap16); d.drawString(2,43,"x=5 w=16 / x=41 w=15"); break;
    }
    d.setColor(WHITE); d.setFont(ArialMT_Plain_10); d.setTextAlignment(TEXT_ALIGN_RIGHT);
    char marker[16]; snprintf(marker,sizeof(marker),"P%d/10",page); d.drawString(127,0,marker); d.display();
}
static void golden(int page) {
    SSD1306Wire d; d.init(); scene(d,page);
    char name[1024];
    // PlatformIO runs from the project root; __FILE__ also supports direct host runs.
    std::string source=__FILE__; auto slash=source.find_last_of("/\\");
    auto path=source.substr(0,slash+1)+"fixtures/device-page-"+std::to_string(page)+".bin";
    FILE* f=fopen(path.c_str(),"rb"); TEST_ASSERT_NOT_NULL_MESSAGE(f,path.c_str());
    uint8_t expected[1024],actual[1024]; auto n=fread(expected,1,1024,f); int extra=fgetc(f); fclose(f);
    TEST_ASSERT_EQUAL_UINT(1024,n); TEST_ASSERT_EQUAL_INT(EOF,extra); pack(d,actual);
    snprintf(name,sizeof(name),"device page %d, all 1024 bytes, no masks",page);
    unsigned differences=0;
    for (int i=0;i<1024;++i) differences+=__builtin_popcount((unsigned)(expected[i]^actual[i]));
    if (differences) printf("Device page %d differs by %u pixels\n",page,differences);
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(expected,actual,1024,name);
    TEST_ASSERT_EQUAL_MEMORY(d.getBuffer(),pushed,8192);
}
static void test_capture_triangle_provenance() {
    CapturedGuestTriangle d; d.init(); scene(d,3);
    std::string source=__FILE__; auto slash=source.find_last_of("/\\");
    // Captured from a device running the previous guest triangle routine.
    auto path=source.substr(0,slash+1)+"fixtures/device-page-3-previous-guest-triangle.bin";
    FILE* f=fopen(path.c_str(),"rb"); TEST_ASSERT_NOT_NULL(f);
    uint8_t expected[1024],actual[1024]; TEST_ASSERT_EQUAL_UINT(1024,fread(expected,1,1024,f)); fclose(f); pack(d,actual);
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(expected,actual,1024,"capture matches historical guest triangle, including text");
}
static void test_device_page_1() { golden(1); }
static void test_device_page_2() { golden(2); }
static void test_device_page_3() { golden(3); }
static void test_device_page_5() { golden(5); }
static void test_device_page_4() { golden(4); }
static void test_device_page_8() { golden(8); }
static void test_clear_and_push() {
    SSD1306Wire d; d.fillRect(0,0,128,64); d.display();
    for (auto b:pushed) TEST_ASSERT_EQUAL_UINT8(1,b);
    int before=pushCount; d.resetDisplay(); TEST_ASSERT_EQUAL_INT(before+1,pushCount);
    for (auto b:pushed) TEST_ASSERT_EQUAL_UINT8(0,b);
    d.setPixel(1,1); before=pushCount; d.cls(); TEST_ASSERT_EQUAL_INT(before+1,pushCount);
    for (auto b:pushed) TEST_ASSERT_EQUAL_UINT8(0,b);
    TEST_ASSERT_EQUAL_INT(8192,d.getBufferSize());
}
void setUp() {}
void tearDown() {}
int main(int,char**) {
    UNITY_BEGIN();
    RUN_TEST(test_circles); RUN_TEST(test_circle_quadrants); RUN_TEST(test_triangles);
    RUN_TEST(test_pixels_lines_rectangles); RUN_TEST(test_progress_bars); RUN_TEST(test_images); RUN_TEST(test_text);
    RUN_TEST(test_device_page_1); RUN_TEST(test_device_page_2); RUN_TEST(test_device_page_3); RUN_TEST(test_device_page_5); RUN_TEST(test_device_page_4); RUN_TEST(test_device_page_8);
    RUN_TEST(test_clear_and_push); RUN_TEST(test_capture_triangle_provenance);
    return UNITY_END();
}
