// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2026 Dismo Industries LLC

#ifndef HOST_TEST
#include "DataScreensaver.h"

#include <Arduino.h>
#include <LittleFS.h>

#include <new>
#include <string>
#include <string.h>

#include "ButtonManager.h"
#include "CfsFormat.h"
#include "DisplayProxy.h"
#include "HAL.h"
#include "LoadoutStore.h"
#include "MenuManager.h"
#include "RGBController.h"
#include "cf_gfx.h"

namespace DataScreensaver {
namespace {

// Data screensavers live here and nowhere else (the manifest's blobPath).
constexpr char kDir[] = "/assets/ss/";

std::string s_pendingPath;
std::string s_pendingLabel;

// Everything one playback needs, allocated at begin() and freed at end(),
// so the player costs no RAM while it is not on screen. All frames point
// at the one Sprite whose data is the reader's frame buffer.
struct Playback {
    File                         file;
    Cfs::Reader                  reader;
    cf::gfx::Sprite              sprite;
    const cf::gfx::Sprite*       frames[Cfs::kMaxFrames];
    cf::gfx::Animation           anim;
    cf::gfx::AnimationPlayer     player;
    int16_t                      x = 0;
    int16_t                      y = 0;
    int                          shownIndex = -2;  // -2 = nothing drawn yet, -1 = blank
};

Playback* s_play = nullptr;
bool      s_error = false;
bool      s_errorButtons = false;
bool      s_backButton = false;

#ifdef CF_TEST_CLI
// Bench only: frame pushes per second and how long a push takes.
uint32_t s_statSinceMs = 0;
uint32_t s_statFrames = 0;
uint32_t s_statPushSumUs = 0;
uint32_t s_statPushMaxUs = 0;
uint32_t s_statReadMaxUs = 0;
#endif

size_t readAt(void* ctx, uint32_t offset, uint8_t* dst, size_t len) {
    File* f = static_cast<File*>(ctx);
    if (!f->seek(offset)) return 0;
    return f->read(dst, len);
}

void drawError() {
    DisplayProxy& d = HAL::displayProxy();
    d.clear();
    d.setColor(WHITE);
    d.setFont(ArialMT_Plain_10);
    d.setTextAlignment(TEXT_ALIGN_CENTER);
    d.drawString(64, 18, "Can't play this one");
    d.drawString(64, 40, "Press any button");
    d.display();
}

void onErrorButton(const ButtonEvent& e) {
    if (e.eventType == ButtonEvent_Released) MenuManager::instance().returnToMenu();
}

void onBack(const ButtonEvent& e) {
    if (e.eventType == ButtonEvent_Released) MenuManager::instance().returnToMenu();
}

void registerErrorButtons() {
    if (s_errorButtons) return;
    for (int i = 0; i < 6; ++i) HAL::buttonManager().registerCallback(i, onErrorButton);
    s_errorButtons = true;
}

void unregisterButtons() {
    if (s_errorButtons) {
        for (int i = 0; i < 6; ++i) HAL::buttonManager().unregisterCallback(i);
        s_errorButtons = false;
        s_backButton = false;  // Back was one of the six
    }
    if (s_backButton) {
        HAL::buttonManager().unregisterCallback(button_BottomLeftIndex);
        s_backButton = false;
    }
}

void freePlayback() {
    if (!s_play) return;
    if (s_play->file) s_play->file.close();
    delete s_play;
    s_play = nullptr;
}

// Every failure ends here: free what was allocated, show the message, and
// hand every button the way back to the menu.
void fail(const char* why) {
    Serial.printf("[cfs] play=error reason=%s path=%s\n", why, s_pendingPath.c_str());
    freePlayback();
    unregisterButtons();
    s_error = true;
    registerErrorButtons();
    drawError();
}

}  // namespace

void setPending(const char* path, const char* label) {
    s_pendingPath  = path ? path : "";
    s_pendingLabel = label ? label : "";
}

void appBegin() {
    setColorsOff();
    s_error = false;
    freePlayback();
    unregisterButtons();

    const std::string& path = s_pendingPath;
    if (path.compare(0, sizeof(kDir) - 1, kDir) != 0 || path.find("..") != std::string::npos) {
        fail("path");
        return;
    }
    if (!LoadoutStore::begin()) { fail("fs"); return; }

    s_play = new (std::nothrow) Playback();
    if (!s_play) { fail("nomem"); return; }

    s_play->file = LittleFS.open(path.c_str(), FILE_READ);
    if (!s_play->file || s_play->file.isDirectory()) { fail("absent"); return; }

    const Cfs::Status st = s_play->reader.open(readAt, &s_play->file, (uint32_t)s_play->file.size());
    if (st != Cfs::Status::Ok) { fail(Cfs::statusName(st)); return; }
    if (!s_play->reader.loadFrame(0)) { fail("read_failed"); return; }

    const Cfs::Header& h = s_play->reader.header();
    s_play->sprite = cf::gfx::Sprite{s_play->reader.frame(), h.width, h.height, 0, 0,
                                     cf::gfx::BO_LSB_FIRST};
    for (uint16_t i = 0; i < h.frameCount; ++i) s_play->frames[i] = &s_play->sprite;
    s_play->anim = cf::gfx::Animation{"cfs", s_play->frames, s_play->reader.durations(), 0,
                                      (uint8_t)h.frameCount, (cf::gfx::LoopMode)h.loopMode};
    // Smaller than the screen: centered.
    s_play->x = (int16_t)((128 - (int)h.width) / 2);
    s_play->y = (int16_t)((64 - (int)h.height) / 2);
    s_play->player.play(&s_play->anim, millis());

    HAL::buttonManager().registerCallback(button_BottomLeftIndex, onBack);
    s_backButton = true;
    Serial.printf("[cfs] play=ok path=%s w=%u h=%u frames=%u loop=%u\n", path.c_str(),
                  (unsigned)h.width, (unsigned)h.height, (unsigned)h.frameCount,
                  (unsigned)h.loopMode);
#ifdef CF_TEST_CLI
    s_statSinceMs = millis();
    s_statFrames = s_statPushSumUs = s_statPushMaxUs = s_statReadMaxUs = 0;
#endif
}

void appUpdate() {
    if (s_error || !s_play) {
        // Hold the message (drawn once by fail()); the buttons stay wired
        // back to the menu.
        if (!s_error) fail("not_started");
        registerErrorButtons();
        return;
    }
    Playback& p = *s_play;
    p.player.update(millis());
    const cf::gfx::Sprite* sprite = p.player.sprite();
    const int want = sprite ? (int)p.player.frameIndex() : -1;
    if (want == p.shownIndex) return;  // nothing changed: no push

    if (want >= 0 && want != p.reader.loadedIndex()) {
#ifdef CF_TEST_CLI
        const uint32_t t0 = micros();
#endif
        if (!p.reader.loadFrame((uint16_t)want)) { fail("read_failed"); return; }
#ifdef CF_TEST_CLI
        const uint32_t readUs = micros() - t0;
        if (readUs > s_statReadMaxUs) s_statReadMaxUs = readUs;
#endif
    }

    DisplayProxy& d = HAL::displayProxy();
    d.clear();
    if (sprite) cf::gfx::drawSprite(d, *sprite, p.x, p.y);
#ifdef CF_TEST_CLI
    const uint32_t t1 = micros();
#endif
    d.display();
    p.shownIndex = want;
#ifdef CF_TEST_CLI
    const uint32_t pushUs = micros() - t1;
    ++s_statFrames;
    s_statPushSumUs += pushUs;
    if (pushUs > s_statPushMaxUs) s_statPushMaxUs = pushUs;
    const uint32_t now = millis();
    if (now - s_statSinceMs >= 5000) {
        const uint32_t elapsed = now - s_statSinceMs;
        Serial.printf("[cfs] stat frames=%u ms=%u fps_x10=%u push_us_avg=%u push_us_max=%u "
                      "read_us_max=%u w=%u h=%u\n",
                      (unsigned)s_statFrames, (unsigned)elapsed,
                      (unsigned)(s_statFrames * 10000u / (elapsed ? elapsed : 1)),
                      (unsigned)(s_statFrames ? s_statPushSumUs / s_statFrames : 0),
                      (unsigned)s_statPushMaxUs, (unsigned)s_statReadMaxUs,
                      (unsigned)p.reader.header().width, (unsigned)p.reader.header().height);
        s_statSinceMs = now;
        s_statFrames = s_statPushSumUs = s_statPushMaxUs = s_statReadMaxUs = 0;
    }
#endif
}

void appEnd() {
    unregisterButtons();
    freePlayback();
    s_error = false;
    setColorsOff();
}

}  // namespace DataScreensaver
#endif  // HOST_TEST
