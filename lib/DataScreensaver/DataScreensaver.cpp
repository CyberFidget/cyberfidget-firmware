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
#include "CfsPlayback.h"
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
// so the player costs no RAM while it is not on screen.
struct Playback {
    File            file;
    Cfs::Reader     reader;
    Cfs::Playback   playback;
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

    // Frame 0 goes on screen now, and its duration counts from when it is
    // there: however late the first update comes, the opening frame shows.
    DisplayProxy& d = HAL::displayProxy();
    d.clear();
    if (!s_play->playback.begin(s_play->reader, d)) { fail("read_failed"); return; }
    d.display();
    s_play->playback.startClock(millis());
    const Cfs::Header& h = s_play->reader.header();

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
#ifdef CF_TEST_CLI
    const uint32_t t0 = micros();
#endif
    const Cfs::Playback::Step step = p.playback.update(millis());
    if (step == Cfs::Playback::Step::Failed) { fail("read_failed"); return; }
    if (step == Cfs::Playback::Step::Unchanged) return;  // nothing changed: no push
#ifdef CF_TEST_CLI
    const uint32_t readUs = micros() - t0;  // includes the frame read
    if (readUs > s_statReadMaxUs) s_statReadMaxUs = readUs;
#endif

    DisplayProxy& d = HAL::displayProxy();
    d.clear();
    p.playback.draw(d);
#ifdef CF_TEST_CLI
    const uint32_t t1 = micros();
#endif
    d.display();
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
