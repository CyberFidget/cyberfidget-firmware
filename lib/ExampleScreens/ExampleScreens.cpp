// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#include "globals.h"
#include "ExampleScreens.h"
#include "images.h"
#include "RGBController.h"
#include "HAL.h"


// The same hardware references (NeoPixels, display)
static auto& strip   = HAL::strip();
static auto& display = HAL::displayProxy();

// --------------------------------------------------------------------
// Original draw functions remain as-is
// --------------------------------------------------------------------
void drawFontFaceDemo() {
  display.clear();
  display.setTextAlignment(TEXT_ALIGN_CENTER);
  display.setFont(ArialMT_Plain_16);
  display.drawString(64, 18, "Hello world");
  display.display();
}

void drawTextFlowDemo() {
  display.clear();
  display.setFont(ArialMT_Plain_10);
  display.setTextAlignment(TEXT_ALIGN_LEFT);
  display.drawStringMaxWidth(
      0, 0, 128,
      "Lorem ipsum confused the non-super nerds so here is some plain english filler to demo text wrapping"
  );
  display.display();
}

void drawTextAlignmentDemo() {
  display.clear();
  display.setFont(ArialMT_Plain_10);

  display.setTextAlignment(TEXT_ALIGN_LEFT);
  display.drawString(0, 10, "Left aligned (0,10)");

  display.setTextAlignment(TEXT_ALIGN_CENTER);
  display.drawString(64, 22, "Center aligned (64,22)");

  display.setTextAlignment(TEXT_ALIGN_RIGHT);
  display.drawString(128, 33, "Right aligned (128,33)");
  display.display();
}

void drawRectDemo() {
  display.clear();
  for (int i = 0; i < 10; i++) {
    display.setPixel(i, i);
    display.setPixel(10 - i, i);
  }
  display.drawRect(12, 12, 20, 20);
  display.fillRect(14, 14, 17, 17);
  display.drawHorizontalLine(0, 40, 20);
  display.drawVerticalLine(40, 0, 20);
  display.display();
}

void drawCircleDemo() {
  display.clear();
  for (int i = 1; i < 8; i++) {
    display.setColor(WHITE);
    display.drawCircle(32, 32, i * 3);
    if (i % 2 == 0) {
      display.setColor(BLACK);
    }
    display.fillCircle(96, 32, 32 - i * 3);
  }
  display.display();
}

void drawImageDemo_1() {
  display.clear();
  display.drawXbm(40, 0, Logo_Round_Tilted_width, Logo_Round_Tilted_height, Logo_Round_Tilted_bits);
  display.display();
}

void drawImageDemo_2() {
  display.clear();
  display.drawXbm(0, 0, 128, 64, epd_bitmap_cf_logo_w_icon);
  display.display();
}

void drawImageDemo_3() {
  display.clear();
  display.drawXbm(0, 0, 128, 64, epd_bitmap_cf_logo);
  display.display();
}

void drawImageDemo_4() {
  display.clear();
  display.drawXbm(0, 0, 128, 64, epd_bitmap_retro_car_sunset);
  display.display();
}

void drawBatteryProgressBar() {
  display.clear();
  display.drawProgressBar(0, 30, 120, 10, batteryVoltagePercentage);
  display.setTextAlignment(TEXT_ALIGN_CENTER);
  display.drawString(64, 15, "Battery: " + String(batteryVoltagePercentage) + "%");
  display.setFont(ArialMT_Plain_10);
  display.setTextAlignment(TEXT_ALIGN_CENTER);
  display.drawString(70, 40, "Battery (V): " + String(batteryVoltage));
  display.display();
}

void drawSliderProgressBar() {
  display.clear();
  display.setFont(ArialMT_Plain_10);
  display.setTextAlignment(TEXT_ALIGN_CENTER);
  display.drawString(64, 4, "Slider: " + String(sliderPosition_Percentage_Inverted_Filtered) + "%");
  display.drawProgressBar(9, 18, 108, 10, sliderPosition_Percentage_Inverted_Filtered);
  display.drawString(64, 30, "Slider (mV): " + String(sliderPosition_Millivolts));
  display.drawString(64, 40, "Slider (bits): " + String(sliderPosition_12Bits));
  display.drawString(64, 50, "Slider Filt (bits): " + String(sliderPosition_12Bits_Filtered));
  display.display();

  uint8_t red, green, blue;
  mapToRainbow(sliderPosition_12Bits_Filtered, 8, red, green, blue);
  HAL::setRgbLed(pixel_Front_Top, red, green, blue, 0);
  HAL::setRgbLed(pixel_Front_Middle, 8 - red, 8 - green, 8 - blue, 0);
  HAL::setRgbLed(pixel_Front_Bottom, red, green, blue, 0);
  updateStrip();
}

void drawAccelerometerScreen() {
  display.clear();
  display.setFont(ArialMT_Plain_10);
  display.setTextAlignment(TEXT_ALIGN_LEFT);
  display.drawString(0, 20, "X: " + String(accelX) + " Y: " + String(accelY) + " Z: " + String(accelZ));

  uint8_t redMap   = map(accelX, -1030, 1030, 0, 255);
  uint8_t greenMap = map(accelY, -1030, 1030, 0, 255);
  uint8_t blueMap  = map(accelZ, -1030, 1030, 0, 255);
  setDeterminedColorsFront(redMap, greenMap, blueMap, 0); 

  display.drawString(0, 10, "R: " + String(redMap) + " G: " + String(greenMap) + " B: " + String(blueMap));
  display.display();
}

void drawButtonCounters() {
  display.clear();
  display.setFont(ArialMT_Plain_10);
  display.setTextAlignment(TEXT_ALIGN_LEFT);
  display.drawString(0, 10, 
    "Btns: 0=" + String(buttonCounter[0]) +
    ", 1=" + String(buttonCounter[1]) +
    ", 2=" + String(buttonCounter[2]));
  display.drawString(0, 20, 
    " 3=" + String(buttonCounter[3]) +
    ", 4=" + String(buttonCounter[4]) +
    ", 5=" + String(buttonCounter[5]));
  display.display();
}

void drawTimeOnCounter() {
  display.clear();
  display.setFont(ArialMT_Plain_10);
  display.setTextAlignment(TEXT_ALIGN_RIGHT);
  display.drawString(128, 20, String(millis()));
  display.display();
}

void drawProgressBarDemo() {
  display.clear();
  int progress = (buttonCounter[5] / 5) % 100;
  display.drawProgressBar(0, 30, 120, 10, progress);
  display.setTextAlignment(TEXT_ALIGN_CENTER);
  display.drawString(64, 15, String(progress) + "%");
  display.display();
}

// --------------------------------------------------------------------
// Now define the actual app classes that wrap the above draw functions
// --------------------------------------------------------------------

// ========== 1) FontFaceApp ==========
FontFaceApp* FontFaceApp::instance = nullptr;

FontFaceApp::FontFaceApp(ButtonManager& btnMgr)
    : buttonManager(btnMgr)
{
    instance = this;
}

void FontFaceApp::begin() {
    buttonManager.registerCallback(button_BottomLeftIndex, onButtonBackPressed);
}

void FontFaceApp::end() {
    buttonManager.unregisterCallback(button_BottomLeftIndex);
}

void FontFaceApp::update() {
    drawFontFaceDemo();
}

void FontFaceApp::onButtonBackPressed(const ButtonEvent& event) {
    if (event.eventType == ButtonEvent_Released) {
        ESP_LOGI(TAG_MAIN, "[FontFaceApp] Back pressed => returning to menu...");
        instance->end();
        MenuManager::instance().returnToMenu();
    }
}
FontFaceApp fontFaceApp(HAL::buttonManager());

// ========== 2) TextFlowApp ==========
TextFlowApp* TextFlowApp::instance = nullptr;

TextFlowApp::TextFlowApp(ButtonManager& btnMgr)
    : buttonManager(btnMgr)
{
    instance = this;
}

void TextFlowApp::begin() {
    buttonManager.registerCallback(button_BottomLeftIndex, onButtonBackPressed);
}

void TextFlowApp::end() {
    buttonManager.unregisterCallback(button_BottomLeftIndex);
}

void TextFlowApp::update() {
    drawTextFlowDemo();
}

void TextFlowApp::onButtonBackPressed(const ButtonEvent& event) {
    if (event.eventType == ButtonEvent_Released) {
        ESP_LOGI(TAG_MAIN, "[TextFlowApp] Back pressed => returning to menu...");
        instance->end();
        MenuManager::instance().returnToMenu();
    }
}
TextFlowApp textFlowApp(HAL::buttonManager());

// ========== 3) TextAlignmentApp ==========
TextAlignmentApp* TextAlignmentApp::instance = nullptr;

TextAlignmentApp::TextAlignmentApp(ButtonManager& btnMgr)
    : buttonManager(btnMgr)
{
    instance = this;
}

void TextAlignmentApp::begin() {
    buttonManager.registerCallback(button_BottomLeftIndex, onButtonBackPressed);
}

void TextAlignmentApp::end() {
    buttonManager.unregisterCallback(button_BottomLeftIndex);
}

void TextAlignmentApp::update() {
    drawTextAlignmentDemo();
}

void TextAlignmentApp::onButtonBackPressed(const ButtonEvent& event) {
    if (event.eventType == ButtonEvent_Released) {
        ESP_LOGI(TAG_MAIN, "[TextAlignmentApp] Back pressed => returning to menu...");
        instance->end();
        MenuManager::instance().returnToMenu();
    }
}
TextAlignmentApp textAlignmentApp(HAL::buttonManager());

// ========== 4) RectApp ==========
RectApp* RectApp::instance = nullptr;

RectApp::RectApp(ButtonManager& btnMgr)
    : buttonManager(btnMgr)
{
    instance = this;
}

void RectApp::begin() {
    buttonManager.registerCallback(button_BottomLeftIndex, onButtonBackPressed);
}

void RectApp::end() {
    buttonManager.unregisterCallback(button_BottomLeftIndex);
}

void RectApp::update() {
    drawRectDemo();
}

void RectApp::onButtonBackPressed(const ButtonEvent& event) {
    if (event.eventType == ButtonEvent_Released) {
        ESP_LOGI(TAG_MAIN, "[RectApp] Back pressed => returning to menu...");
        instance->end();
        MenuManager::instance().returnToMenu();
    }
}
RectApp rectApp(HAL::buttonManager());

// ========== 5) CircleApp ==========
CircleApp* CircleApp::instance = nullptr;

CircleApp::CircleApp(ButtonManager& btnMgr)
    : buttonManager(btnMgr)
{
    instance = this;
}

void CircleApp::begin() {
    buttonManager.registerCallback(button_BottomLeftIndex, onButtonBackPressed);
}

void CircleApp::end() {
    buttonManager.unregisterCallback(button_BottomLeftIndex);
}

void CircleApp::update() {
    drawCircleDemo();
}

void CircleApp::onButtonBackPressed(const ButtonEvent& event) {
    if (event.eventType == ButtonEvent_Released) {
        ESP_LOGI(TAG_MAIN, "[CircleApp] Back pressed => returning to menu...");
        instance->end();
        MenuManager::instance().returnToMenu();
    }
}
CircleApp circleApp(HAL::buttonManager());

// ========== 6) ImageDemo1App ==========
ImageDemo1App* ImageDemo1App::instance = nullptr;

ImageDemo1App::ImageDemo1App(ButtonManager& btnMgr)
    : buttonManager(btnMgr)
{
    instance = this;
}

void ImageDemo1App::begin() {
    buttonManager.registerCallback(button_BottomLeftIndex, onButtonBackPressed);
}

void ImageDemo1App::end() {
    buttonManager.unregisterCallback(button_BottomLeftIndex);
}

void ImageDemo1App::update() {
    drawImageDemo_1();
}

void ImageDemo1App::onButtonBackPressed(const ButtonEvent& event) {
    if (event.eventType == ButtonEvent_Released) {
        ESP_LOGI(TAG_MAIN, "[ImageDemo1App] Back pressed => returning to menu...");
        instance->end();
        MenuManager::instance().returnToMenu();
    }
}
ImageDemo1App imageDemo1App(HAL::buttonManager());

// ========== 7) ImageDemo2App ==========
ImageDemo2App* ImageDemo2App::instance = nullptr;

ImageDemo2App::ImageDemo2App(ButtonManager& btnMgr)
    : buttonManager(btnMgr)
{
    instance = this;
}

void ImageDemo2App::begin() {
    buttonManager.registerCallback(button_BottomLeftIndex, onButtonBackPressed);
}

void ImageDemo2App::end() {
    buttonManager.unregisterCallback(button_BottomLeftIndex);
}

void ImageDemo2App::update() {
    drawImageDemo_2();
}

void ImageDemo2App::onButtonBackPressed(const ButtonEvent& event) {
    if (event.eventType == ButtonEvent_Released) {
        ESP_LOGI(TAG_MAIN, "[ImageDemo2App] Back pressed => returning to menu...");
        instance->end();
        MenuManager::instance().returnToMenu();
    }
}
ImageDemo2App imageDemo2App(HAL::buttonManager());

// ========== 8) ImageDemo3App ==========
ImageDemo3App* ImageDemo3App::instance = nullptr;

ImageDemo3App::ImageDemo3App(ButtonManager& btnMgr)
    : buttonManager(btnMgr)
{
    instance = this;
}

void ImageDemo3App::begin() {
    buttonManager.registerCallback(button_BottomLeftIndex, onButtonBackPressed);
}

void ImageDemo3App::end() {
    buttonManager.unregisterCallback(button_BottomLeftIndex);
}

void ImageDemo3App::update() {
    drawImageDemo_3();
}

void ImageDemo3App::onButtonBackPressed(const ButtonEvent& event) {
    if (event.eventType == ButtonEvent_Released) {
        ESP_LOGI(TAG_MAIN, "[ImageDemo3App] Back pressed => returning to menu...");
        instance->end();
        MenuManager::instance().returnToMenu();
    }
}
ImageDemo3App imageDemo3App(HAL::buttonManager());

// ========== 9) ImageDemo4App ==========
ImageDemo4App* ImageDemo4App::instance = nullptr;

ImageDemo4App::ImageDemo4App(ButtonManager& btnMgr)
    : buttonManager(btnMgr)
{
    instance = this;
}

void ImageDemo4App::begin() {
    buttonManager.registerCallback(button_BottomLeftIndex, onButtonBackPressed);
}

void ImageDemo4App::end() {
    buttonManager.unregisterCallback(button_BottomLeftIndex);
}

void ImageDemo4App::update() {
    drawImageDemo_4();
}

void ImageDemo4App::onButtonBackPressed(const ButtonEvent& event) {
    if (event.eventType == ButtonEvent_Released) {
        ESP_LOGI(TAG_MAIN, "[ImageDemo4App] Back pressed => returning to menu...");
        instance->end();
        MenuManager::instance().returnToMenu();
    }
}
ImageDemo4App imageDemo4App(HAL::buttonManager());

// ========== 10) BatteryBarApp ==========
BatteryBarApp* BatteryBarApp::instance = nullptr;

BatteryBarApp::BatteryBarApp(ButtonManager& btnMgr)
    : buttonManager(btnMgr)
{
    instance = this;
}

void BatteryBarApp::begin() {
    buttonManager.registerCallback(button_BottomLeftIndex, onButtonBackPressed);
    setColorsOff();
}

void BatteryBarApp::end() {
    buttonManager.unregisterCallback(button_BottomLeftIndex);
    setColorsOff();
    updateStrip();
}

void BatteryBarApp::update() {
    drawBatteryProgressBar();
}

void BatteryBarApp::onButtonBackPressed(const ButtonEvent& event) {
    if (event.eventType == ButtonEvent_Released) {
        ESP_LOGI(TAG_MAIN, "[BatteryBarApp] Back pressed => returning to menu...");
        instance->end();
        MenuManager::instance().returnToMenu();
    }
}
BatteryBarApp batteryBarApp(HAL::buttonManager());

// ========== 11) SliderBarApp ==========
// This one calls strip.setPixelColor() so we’ll call setColorsOff() on end().
SliderBarApp* SliderBarApp::instance = nullptr;

SliderBarApp::SliderBarApp(ButtonManager& btnMgr)
    : buttonManager(btnMgr)
{
    instance = this;
}

void SliderBarApp::begin() {
    buttonManager.registerCallback(button_BottomLeftIndex, onButtonBackPressed);
}

void SliderBarApp::end() {
    buttonManager.unregisterCallback(button_BottomLeftIndex);

    // Turn off any LEDs we manipulated
    setColorsOff();
    updateStrip();
}

void SliderBarApp::update() {
    drawSliderProgressBar();
}

void SliderBarApp::onButtonBackPressed(const ButtonEvent& event) {
    if (event.eventType == ButtonEvent_Released) {
        ESP_LOGI(TAG_MAIN, "[SliderBarApp] Back pressed => returning to menu...");
        instance->end();
        MenuManager::instance().returnToMenu();
    }
}
SliderBarApp sliderBarApp(HAL::buttonManager());

// ========== 12) AccelerometerApp ==========
// Also manipulates colors, so setColorsOff() on end().
AccelerometerApp* AccelerometerApp::instance = nullptr;

AccelerometerApp::AccelerometerApp(ButtonManager& btnMgr)
    : buttonManager(btnMgr)
{
    instance = this;
}

void AccelerometerApp::begin() {
    buttonManager.registerCallback(button_BottomLeftIndex, onButtonBackPressed);
}

void AccelerometerApp::end() {
    buttonManager.unregisterCallback(button_BottomLeftIndex);

    // Turn off any LEDs we manipulated
    setColorsOff();
    updateStrip();
}

void AccelerometerApp::update() {
    drawAccelerometerScreen();
}

void AccelerometerApp::onButtonBackPressed(const ButtonEvent& event) {
    if (event.eventType == ButtonEvent_Released) {
        ESP_LOGI(TAG_MAIN, "[AccelerometerApp] Back pressed => returning to menu...");
        instance->end();
        MenuManager::instance().returnToMenu();
    }
}
AccelerometerApp accelerometerApp(HAL::buttonManager());

// ========== 13) ButtonCountersApp ==========
ButtonCountersApp* ButtonCountersApp::instance = nullptr;

ButtonCountersApp::ButtonCountersApp(ButtonManager& btnMgr)
    : buttonManager(btnMgr)
{
    instance = this;
}

void ButtonCountersApp::begin() {
    buttonManager.registerCallback(button_BottomLeftIndex, onButtonBackPressed);
}

void ButtonCountersApp::end() {
    buttonManager.unregisterCallback(button_BottomLeftIndex);
}

void ButtonCountersApp::update() {
    drawButtonCounters();
}

void ButtonCountersApp::onButtonBackPressed(const ButtonEvent& event) {
    if (event.eventType == ButtonEvent_Released) {
        ESP_LOGI(TAG_MAIN, "[ButtonCountersApp] Back pressed => returning to menu...");
        instance->end();
        MenuManager::instance().returnToMenu();
    }
}
ButtonCountersApp buttonCountersApp(HAL::buttonManager());

// ========== 14) TimeOnCounterApp ==========
TimeOnCounterApp* TimeOnCounterApp::instance = nullptr;

TimeOnCounterApp::TimeOnCounterApp(ButtonManager& btnMgr)
    : buttonManager(btnMgr)
{
    instance = this;
}

void TimeOnCounterApp::begin() {
    buttonManager.registerCallback(button_BottomLeftIndex, onButtonBackPressed);
}

void TimeOnCounterApp::end() {
    buttonManager.unregisterCallback(button_BottomLeftIndex);
}

void TimeOnCounterApp::update() {
    drawTimeOnCounter();
}

void TimeOnCounterApp::onButtonBackPressed(const ButtonEvent& event) {
    if (event.eventType == ButtonEvent_Released) {
        ESP_LOGI(TAG_MAIN, "[TimeOnCounterApp] Back pressed => returning to menu...");
        instance->end();
        MenuManager::instance().returnToMenu();
    }
}
TimeOnCounterApp timeOnCounterApp(HAL::buttonManager());

// ========== 15) ProgressBarApp ==========
ProgressBarApp* ProgressBarApp::instance = nullptr;

ProgressBarApp::ProgressBarApp(ButtonManager& btnMgr)
    : buttonManager(btnMgr)
{
    instance = this;
}

void ProgressBarApp::begin() {
    buttonManager.registerCallback(button_BottomLeftIndex, onButtonBackPressed);
}

void ProgressBarApp::end() {
    buttonManager.unregisterCallback(button_BottomLeftIndex);
}

void ProgressBarApp::update() {
    drawProgressBarDemo();
}

void ProgressBarApp::onButtonBackPressed(const ButtonEvent& event) {
    if (event.eventType == ButtonEvent_Released) {
        ESP_LOGI(TAG_MAIN, "[ProgressBarApp] Back pressed => returning to menu...");
        instance->end();
        MenuManager::instance().returnToMenu();
    }
}
ProgressBarApp progressBarApp(HAL::buttonManager());

