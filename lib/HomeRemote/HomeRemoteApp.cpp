// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#include "HomeRemoteApp.h"

#ifndef HOST_TEST

#include <Arduino.h>
#include <Preferences.h>
#include <esp_bt.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <new>
#include <stdio.h>
#include <string.h>

#include "DisplayProxy.h"
#include "HAL.h"
#include "HomeRemoteClient.h"
#include "HomeRemoteConfig.h"
#include "HomeRemoteLogic.h"
#include "MenuManager.h"
#include "RGBController.h"
#include "ScrollLabel.h"
#include "WifiList.h"
#include "globals.h"

namespace HomeRemoteApp {
namespace {

using namespace HomeRemote;
using Client::Fetch;
using Client::FetchResult;

auto& display = HAL::displayProxy();

constexpr int kScreenW = 128;
constexpr int kScreenH = 64;
constexpr int kTitleH  = 14;
constexpr int kListY   = 16;
constexpr int kRowH    = 12;
constexpr int kRows    = 4;
constexpr int kTextX   = 4;
constexpr int kGutter  = 4;
constexpr int kRowW    = kScreenW - kGutter;

constexpr uint32_t kPollMs = 1000;            // the open light, while its screen is up
constexpr uint32_t kToastMs = 2000;
constexpr uint32_t kAwakeCapMs = 5UL * 60 * 1000;   // a light screen keeps the device awake this long after use
constexpr uint32_t kFlushWaitMs = 6500;       // end(): the last change still goes out (one request)
// Status rows: the connection (Enter checks again), the LED mirror setting
// (Enter turns it on or off), the address, the WiFi network.
constexpr int kStatusRows = 4;

enum class Screen : uint8_t { Message, Top, Areas, AreaList, Detail, Effects, Status };
enum class Msg : uint8_t {
    None, Joining, NotSetUp, NoWifi, RadioBusy, NoMemory, JoinFailed, Unreachable, Rejected, BadReply
};
enum class Row : uint8_t { Power, Bri, R, G, B, Preset, Effect, Playback, Volume, Track, Mute };

// Lists live in PSRAM when there is some (allocated once, kept).
struct Lists {
    Entity fav[kMaxRows];
    int favCount = 0;
    Entity roomRows[kMaxRows];
    int roomRowCount = 0;
    Area areas[kMaxRows];
    int areaCount = 0;
    char effects[kMaxEffects][kEffectMax + 1] = {};
    int effectCount = 0;
    char areaId[kIdMax + 1] = {0};
    char areaName[kNameMax + 1] = {0};
    Entity poll;
};
Lists* L = nullptr;

Screen screen = Screen::Message;
Msg msg = Msg::None;
Screen detailParent = Screen::Top;
bool favLoaded = false;
bool areasLoaded = false;
bool roomLoaded = false;
bool effectsLoaded = false;
Fetch lastReqKind = Fetch::None;
uint32_t lastReqSeq = 0;
uint32_t lastPollMs = 0;
bool refreshAfterAction = false;
bool everAnswered = false;
Outcome lastOutcome = Outcome::Ok;

ListCursor topCur, areaCur, roomCur, detailCur, fxCur, statusCur;
bool ledsLit = false;
LedColor ledShown;
ScrollLabel focusLabel;
ScrollLabel titleLabel;
ScrollLabel mediaLabel;
int focusFor = -1;
Screen focusScreen = Screen::Message;

char host[kUrlMax + 1] = {0};   // the address, read once per start
char toast[40] = {0};
uint32_t toastAt = 0;
uint32_t lastFailToastMs = 0;
bool enterArmed = false;
bool heldFired = false;
uint32_t lastUseMs = 0;
int lastSlider = 0;

// ---- the open light ------------------------------------------------------------------
Entity d;
Row detailRows[7];
int detailRowCount = 0;
int presetIdx = 0;
bool dirtyPower = false, dirtyBri = false, dirtyRgb = false, dirtyEffect = false;
bool dirtyVolume = false;
bool leaveAfterVolume = false;
SendGate lightGate, volumeGate;
// Keep the speaker's send clock across screen changes and reopening.
SendGate& sendGate() { return d.kind == Kind::Speaker ? volumeGate : lightGate; }
PollGuard guard;
SoftTakeover take;
RepeatAccel accel;
int accelDir = 0;
Row accelRow = Row::Power;

uint32_t now() { return (uint32_t)millis(); }

int sliderNow() {
    int v = sliderPosition_8Bits_Inverted_Filtered;
    return v < 0 ? 0 : (v > 255 ? 255 : v);
}

void showToast(const char* text) {
    strncpy(toast, text, sizeof(toast) - 1);
    toast[sizeof(toast) - 1] = '\0';
    toastAt = now();
}

void failToast(Outcome o) {
    // Rate-limited: a light screen polls every second.
    if (now() - lastFailToastMs < 3000) return;
    lastFailToastMs = now();
    showToast(o == Outcome::Unreachable ? "Can't reach Home Assistant" : "Unexpected answer");
}

void showMessage(Msg m) {
    screen = Screen::Message;
    msg = m;
}

void request(Fetch kind, const char* arg) {
    lastReqKind = kind;
    lastReqSeq = Client::requestFetch(kind, arg);
}

// ---- rows and labels ------------------------------------------------------------------

int favRows() { return favLoaded && L->favCount > 0 ? L->favCount : 1; }
int topCount() { return favRows() + 2; }
bool topIsFav(int i) { return favLoaded && i < L->favCount; }
bool topIsRooms(int i) { return i == favRows(); }
bool topIsStatus(int i) { return i == favRows() + 1; }

const char* entityRight(const Entity& e) {
    if (e.kind == Kind::Speaker) return e.available ? speakerStateText(e.speakerState) : "--";
    if (e.kind != Kind::Light) return "Run";
    if (!e.available) return "--";
    return e.on ? "On" : "Off";
}

void topLabel(int i, char* out, size_t cap, const char** right) {
    *right = nullptr;
    if (topIsRooms(i)) { snprintf(out, cap, "Rooms"); *right = ">"; return; }
    if (topIsStatus(i)) { snprintf(out, cap, "Status"); return; }
    if (!favLoaded) { snprintf(out, cap, "Loading..."); return; }
    if (L->favCount == 0) { snprintf(out, cap, "No favorites"); return; }
    snprintf(out, cap, "%s", L->fav[i].name);
    *right = entityRight(L->fav[i]);
}

int areaRows() { return areasLoaded && L->areaCount > 0 ? L->areaCount : 1; }
void areaLabel(int i, char* out, size_t cap, const char** right) {
    *right = nullptr;
    if (!areasLoaded) { snprintf(out, cap, "Loading..."); return; }
    if (L->areaCount == 0) { snprintf(out, cap, "No rooms found"); return; }
    snprintf(out, cap, "%s", L->areas[i].name);
    *right = ">";
}

int roomRows() { return roomLoaded && L->roomRowCount > 0 ? L->roomRowCount : 1; }
void roomLabel(int i, char* out, size_t cap, const char** right) {
    *right = nullptr;
    if (!roomLoaded) { snprintf(out, cap, "Loading..."); return; }
    if (L->roomRowCount == 0) { snprintf(out, cap, "Nothing here"); return; }
    snprintf(out, cap, "%s", L->roomRows[i].name);
    *right = entityRight(L->roomRows[i]);
}

int fxRows() { return effectsLoaded && L->effectCount > 0 ? L->effectCount : 1; }
void fxLabel(int i, char* out, size_t cap, const char** right) {
    *right = nullptr;
    if (!effectsLoaded) { snprintf(out, cap, "Loading..."); return; }
    if (L->effectCount == 0) { snprintf(out, cap, "No effects"); return; }
    displayText(L->effects[i], out, cap);   // the exact name is kept for sending
    if (strcmp(L->effects[i], d.effect) == 0) *right = "On";
}

// ---- the open light -------------------------------------------------------------------

void rebuildDetailRows() {
    int n = 0;
    if (d.kind == Kind::Speaker) {
        if (speakerCanPlayPause(d.features, d.speakerState)) detailRows[n++] = Row::Playback;
        if (speakerSupports(d.features, VolumeSet)) detailRows[n++] = Row::Volume;
        if (speakerSupports(d.features, PreviousTrack) || speakerSupports(d.features, NextTrack))
            detailRows[n++] = Row::Track;
        if (speakerSupports(d.features, VolumeMute)) detailRows[n++] = Row::Mute;
        detailRowCount = n;
        detailCur.setCount(n);
        return;
    }
    detailRows[n++] = Row::Power;
    if (d.canDim) detailRows[n++] = Row::Bri;
    if (d.canColor) {
        detailRows[n++] = Row::R;
        detailRows[n++] = Row::G;
        detailRows[n++] = Row::B;
        detailRows[n++] = Row::Preset;
    }
    if (d.effectCount > 0) detailRows[n++] = Row::Effect;
    detailRowCount = n;
    detailCur.setCount(n);
}

Row currentRow() { return detailRowCount ? detailRows[detailCur.selected()] : Row::Playback; }

void touch(PollGuard::Control c) {
    guard.touched(c, now());
    sendGate().markDirty();
    lastUseMs = now();
}

void setPower(bool on) {
    d.on = on;
    dirtyPower = true;
    if (!on) { dirtyBri = dirtyRgb = dirtyEffect = false; }
    touch(PollGuard::Power);
    take.setReal(on ? d.brightness : 0, sliderNow());
}

// Every change but Off also turns the light on (Home Assistant does).
void markOn() {
    if (!d.on) { d.on = true; dirtyPower = true; }
    guard.touched(PollGuard::Power, now());
}

void setBrightness(int level, bool fromSlider) {
    if (level < 1) level = 1;
    if (level > 255) level = 255;
    d.brightness = (uint8_t)level;
    markOn();
    dirtyBri = true;
    touch(PollGuard::Brightness);
    // A button change moves the level away from the slider: it has to
    // pick the new level up again.
    if (!fromSlider) take.setReal(level, sliderNow());
}

void setRgb(int r, int g, int b) {
    d.r = (uint8_t)(r < 0 ? 0 : (r > 255 ? 255 : r));
    d.g = (uint8_t)(g < 0 ? 0 : (g > 255 ? 255 : g));
    d.b = (uint8_t)(b < 0 ? 0 : (b > 255 ? 255 : b));
    d.hasRgb = true;
    markOn();
    dirtyRgb = true;
    dirtyEffect = false;
    touch(PollGuard::Color);
}

void applyPreset() {
    const Preset& p = preset(presetIdx);
    setRgb(p.r, p.g, p.b);
}

void setEffect(const char* name) {
    strncpy(d.effect, name, sizeof(d.effect) - 1);
    d.effect[sizeof(d.effect) - 1] = '\0';
    markOn();
    dirtyEffect = true;
    touch(PollGuard::Effect);
}

void setVolume(int level, bool fromSlider) {
    if (!d.available || !d.hasVolume || !speakerSupports(d.features, VolumeSet)) return;
    const uint8_t next = volumeByte(volumePercent(level));
    if (!fromSlider) take.arm(next, sliderNow(), true);
    if (volumePercent(next) == volumePercent(d.volume)) return;
    d.volume = next;
    dirtyVolume = true;
    touch(PollGuard::Brightness);   // this screen's level uses the same guarded control
}

void speakerAction(Action::Type type) {
    if (!d.available) { showToast("Not available"); return; }
    if (type == Action::Type::PlayPause && !speakerCanPlayPause(d.features, d.speakerState)) return;
    if (type == Action::Type::Previous && !speakerSupports(d.features, PreviousTrack)) return;
    if (type == Action::Type::Next && !speakerSupports(d.features, NextTrack)) return;
    if (type == Action::Type::Mute && !speakerSupports(d.features, VolumeMute)) return;
    // Preserve a deliberate volume adjustment before posting another action
    // to the worker's latest-wins slot. Try again once it has gone out.
    if (sendGate().dirty() || Client::actionBusy()) { showToast("Please wait"); return; }
    Action a;
    strncpy(a.entity, d.id, sizeof(a.entity) - 1);
    a.type = type;
    if (type == Action::Type::Mute) {
        d.muted = !d.muted;
        a.muted = d.muted;
        guard.touched(PollGuard::Effect, now());
    }
    Client::requestAction(a);
    lastPollMs = now() - kPollMs;
}

void adjust(Row row, int dir, int amount) {
    switch (row) {
        case Row::Volume: setVolume(steppedVolume(d.volume, dir, amount), false); break;
        case Row::Track: speakerAction(dir < 0 ? Action::Type::Previous : Action::Type::Next); break;
        case Row::Playback:
        case Row::Mute: break;
        case Row::Power: setPower(dir > 0); break;
        case Row::Bri: {
            // In whole percent, so every step shows.
            int pct = (d.on ? brightnessPercent(d.brightness) : 0) + dir * amount;
            if (pct < 1) pct = 1;
            if (pct > 100) pct = 100;
            setBrightness((pct * 255 + 50) / 100, false);
            break;
        }
        case Row::R: setRgb(d.r + dir * amount, d.g, d.b); break;
        case Row::G: setRgb(d.r, d.g + dir * amount, d.b); break;
        case Row::B: setRgb(d.r, d.g, d.b + dir * amount); break;
        case Row::Preset:
            presetIdx = (presetIdx + (dir > 0 ? 1 : kPresetCount - 1)) % kPresetCount;
            applyPreset();
            break;
        case Row::Effect: break;
    }
}

Action pendingAction() {
    Action a;
    strncpy(a.entity, d.id, sizeof(a.entity) - 1);
    if (d.kind == Kind::Speaker) {
        if (dirtyVolume) {
            a.type = Action::Type::SetVolume;
            a.volumePercent = volumePercent(d.volume);
        }
        return a;
    }
    if (dirtyPower && !d.on) {
        a.type = Action::Type::TurnOff;
        return a;
    }
    a.type = Action::Type::TurnOn;
    if (dirtyBri) { a.setBrightness = true; a.brightness = d.brightness ? d.brightness : 1; }
    if (dirtyRgb) { a.setRgb = true; a.r = d.r; a.g = d.g; a.b = d.b; }
    if (dirtyEffect && d.effect[0]) {
        a.setEffect = true;
        strncpy(a.effect, d.effect, sizeof(a.effect) - 1);
    }
    return a;
}

void clearDirty() { dirtyPower = dirtyBri = dirtyRgb = dirtyEffect = dirtyVolume = false; }

void sendNow() {
    Client::requestAction(pendingAction());
    sendGate().sent(now());
    clearDirty();
}

void sendIfDue() {
    if (sendGate().ready(now(), Client::actionBusy())) sendNow();
}

void openDetail(const Entity& e, Screen parent) {
    d = e;
    if (!d.on) d.brightness = 0;
    detailParent = parent;
    presetIdx = 0;
    clearDirty();
    leaveAfterVolume = false;
    if (d.kind == Kind::Speaker) volumeGate.clear();
    else lightGate = SendGate();
    guard.reset();
    accel.release();
    // Opening never changes the light: the slider must reach its level first.
    take.arm(d.kind == Kind::Speaker ? d.volume : (d.on ? d.brightness : 0),
             sliderNow(), d.kind == Kind::Speaker);
    titleLabel.restart(now());
    mediaLabel.restart(now());
    detailCur.reset(1, d.kind == Kind::Speaker ? 3 : kRows);
    rebuildDetailRows();
    screen = Screen::Detail;
    lastPollMs = now() - kPollMs;   // read it fresh at once
    lastUseMs = now();
}

// The front LEDs (1-3) mirror the open light's colour; everything else
// keeps them off. Only a change is written.
//
// Idle sleep does not run end(), so the mirror is lit only while this app is
// holding sleep off (a light screen within kAwakeCapMs of its last use, the
// same test update() uses). Once that window closes the LEDs go dark, and
// the idle sleep can only come TASK_LASTINTERACT (60 s) after it.
void updateLeds() {
    const bool lightScreen = screen == Screen::Detail || screen == Screen::Effects;
    const bool keptAwake = now() - lastUseMs < kAwakeCapMs;
    LedColor want;
    if (lightScreen && d.kind == Kind::Light && keptAwake && HomeRemote::Config::mirror() && d.available)
        want = mirrorColor(d.on, d.hasRgb, d.r, d.g, d.b, d.brightness);
    const bool lit = want != LedColor();
    if (!lit) {
        if (ledsLit) setColorsOff();
        ledsLit = false;
        return;
    }
    if (ledsLit && want == ledShown) return;
    setDeterminedColorsFront(want.r, want.g, want.b, 0);
    ledShown = want;
    ledsLit = true;
}

void ledsOff() {
    setColorsOff();
    ledsLit = false;
}

void leaveDetail() {
    accel.release();
    ledsOff();
    if (d.kind == Kind::Speaker && sendGate().dirty() && !sendGate().ready(now(), Client::actionBusy())) {
        leaveAfterVolume = true;
        return;
    }
    leaveAfterVolume = false;
    if (sendGate().dirty()) sendNow();   // the last change still goes out
    screen = detailParent;
    // The list behind shows on/off: read it again.
    if (screen == Screen::AreaList) request(Fetch::AreaEntities, L->areaId);
    else request(Fetch::Favorites, nullptr);
}

void applyPolled(const Entity& e) {
    if (strcmp(e.id, d.id) != 0) return;
    // A change of ours still going out would be undone by an older reading.
    if (Client::actionBusy() || sendGate().dirty()) return;
    const uint32_t t = now();
    const bool availableChanged = d.available != e.available;
    d.available = e.available;
    if (d.kind == Kind::Speaker) {
        const bool changedVolume = d.hasVolume != e.hasVolume;
        d.features = e.features;
        if (strcmp(d.title, e.title) || strcmp(d.artist, e.artist) || availableChanged || d.speakerState != e.speakerState)
            mediaLabel.restart(t);
        d.speakerState = e.speakerState;
        memcpy(d.name, e.name, sizeof(d.name));
        memcpy(d.title, e.title, sizeof(d.title));
        memcpy(d.artist, e.artist, sizeof(d.artist));
        if (guard.accepts(PollGuard::Effect, t)) d.muted = e.muted;
        if (guard.accepts(PollGuard::Brightness, t)) {
            d.hasVolume = e.hasVolume;
            d.volume = e.volume;
            if (changedVolume) take.arm(d.volume, sliderNow(), true);
            else take.setReal(d.volume, sliderNow());
        }
        if (!d.available) {
            accel.release();
            take.arm(d.volume, sliderNow(), true);
        }
        rebuildDetailRows();
        return;
    }
    d.canDim = e.canDim;
    d.canColor = e.canColor;
    d.effectCount = e.effectCount;
    if (guard.accepts(PollGuard::Power, t)) d.on = e.on;
    if (guard.accepts(PollGuard::Brightness, t)) {
        d.brightness = e.on ? e.brightness : 0;
        take.setReal(d.brightness, sliderNow());
    }
    if (guard.accepts(PollGuard::Color, t) && e.hasRgb) {
        d.r = e.r; d.g = e.g; d.b = e.b;
        d.hasRgb = true;
    }
    if (guard.accepts(PollGuard::Effect, t)) memcpy(d.effect, e.effect, sizeof(d.effect));
    rebuildDetailRows();
}

// ---- actions from the lists -----------------------------------------------------------

void runOrToggle(Entity& e, bool toggle) {
    if (!e.available) { showToast("Not available"); return; }
    Action a;
    strncpy(a.entity, e.id, sizeof(a.entity) - 1);
    if (e.kind == Kind::Light) {
        a.type = Action::Type::Toggle;
        e.on = !e.on;   // shown at once; the list is read again after
    } else if (e.kind == Kind::Speaker) {
        if (!speakerCanPlayPause(e.features, e.speakerState)) { showToast("Not supported"); return; }
        a.type = Action::Type::PlayPause;
    } else {
        a.type = Action::Type::Run;
        showToast(e.kind == Kind::Scene ? "Scene on" : "Script started");
    }
    (void)toggle;
    Client::requestAction(a);
    refreshAfterAction = true;
}

void openEntity(Entity& e, Screen from) {
    if (!e.available) { showToast("Not available"); return; }
    if (e.kind == Kind::Light || e.kind == Kind::Speaker) openDetail(e, from);
    else runOrToggle(e, false);
}

void openRooms() {
    screen = Screen::Areas;
    areasLoaded = false;
    areaCur.reset(1, kRows);
    request(Fetch::Areas, nullptr);
}

void openRoom(const Area& a) {
    memcpy(L->areaId, a.id, sizeof(L->areaId));
    memcpy(L->areaName, a.name, sizeof(L->areaName));
    screen = Screen::AreaList;
    roomLoaded = false;
    roomCur.reset(1, kRows);
    request(Fetch::AreaEntities, L->areaId);
}

void openEffects() {
    screen = Screen::Effects;
    effectsLoaded = false;
    fxCur.reset(1, kRows);
    request(Fetch::Effects, d.id);
}

void retry() {
    showMessage(Msg::Joining);
    favLoaded = false;
    request(Fetch::Favorites, nullptr);
}

void exitApp() { MenuManager::instance().returnToMenu(); }

// ---- results --------------------------------------------------------------------------

void failed(Outcome o, bool firstLoad) {
    if (o == Outcome::Rejected) { showMessage(Msg::Rejected); return; }
    if (firstLoad) {
        if (o == Outcome::Unreachable)
            showMessage(Client::link() == Client::Link::JoinFailed ? Msg::JoinFailed : Msg::Unreachable);
        else showMessage(Msg::BadReply);
        return;
    }
    failToast(o);
}

void handleFetch() {
    FetchResult meta;
    Entity* rows = nullptr;
    int maxRows = 0;
    switch (lastReqKind) {
        case Fetch::Favorites:    rows = L->fav; maxRows = kMaxRows; break;
        case Fetch::AreaEntities: rows = L->roomRows; maxRows = kMaxRows; break;
        case Fetch::Entity:       rows = &L->poll; maxRows = 1; break;
        default: break;
    }
    if (!Client::takeFetch(meta, rows, maxRows, L->areas, kMaxRows, L->effects, kMaxEffects)) return;
    if (meta.seq != lastReqSeq || meta.kind != lastReqKind) return;
    const bool ok = meta.outcome == Outcome::Ok;
    if (meta.outcome != Outcome::Unreachable || Client::link() == Client::Link::Ready) {
        everAnswered = true;
        lastOutcome = meta.outcome;
    }
    switch (meta.kind) {
        case Fetch::Favorites: {
            const bool first = screen == Screen::Message;
            if (!ok) {
                // A list read again in the background keeps what it showed.
                failed(meta.outcome, first && msg == Msg::Joining);
                return;
            }
            L->favCount = meta.count;
            favLoaded = true;
            topCur.setCount(topCount());
            if (first && msg == Msg::Joining) screen = Screen::Top;
            break;
        }
        case Fetch::Areas:
            if (!ok) { failed(meta.outcome, false); if (screen == Screen::Areas) screen = Screen::Top; return; }
            L->areaCount = meta.count;
            areasLoaded = true;
            areaCur.setCount(areaRows());
            break;
        case Fetch::AreaEntities:
            if (!ok) { failed(meta.outcome, false); if (screen == Screen::AreaList && !roomLoaded) screen = Screen::Areas; return; }
            L->roomRowCount = meta.count;
            roomLoaded = true;
            roomCur.setCount(roomRows());
            break;
        case Fetch::Entity:
            if (!ok) { failed(meta.outcome, false); return; }
            if (meta.count == 1) applyPolled(L->poll);
            break;
        case Fetch::Effects:
            if (!ok) { failed(meta.outcome, false); if (screen == Screen::Effects) screen = Screen::Detail; return; }
            L->effectCount = meta.count;
            effectsLoaded = true;
            fxCur.reset(fxRows(), kRows);
            // Start on the effect that is running, if any.
            for (int i = 0; i < L->effectCount; i++) {
                if (strcmp(L->effects[i], d.effect) != 0) continue;
                for (int k = 0; k < i; k++) fxCur.down();
                break;
            }
            break;
        default: break;
    }
}

void handleAction() {
    uint32_t seq = 0;
    Outcome o = Outcome::Ok;
    if (!Client::takeAction(seq, o)) return;
    if (o != Outcome::Ok) {
        refreshAfterAction = true;   // show what really happened
        if (o == Outcome::Rejected) { showMessage(Msg::Rejected); return; }
        lastFailToastMs = 0;
        failToast(o);
    }
    if (!refreshAfterAction) return;
    refreshAfterAction = false;
    if (screen == Screen::Top) request(Fetch::Favorites, nullptr);
    else if (screen == Screen::AreaList) request(Fetch::AreaEntities, L->areaId);
}

// ---- buttons --------------------------------------------------------------------------

ListCursor* activeCursor() {
    switch (screen) {
        case Screen::Top:      return &topCur;
        case Screen::Areas:    return &areaCur;
        case Screen::AreaList: return &roomCur;
        case Screen::Detail:   return &detailCur;
        case Screen::Effects:  return &fxCur;
        case Screen::Status:   return &statusCur;
        default:               return nullptr;
    }
}

void onUp(const ButtonEvent& ev) {
    if (ev.eventType != ButtonEvent_Pressed) return;
    lastUseMs = now();
    if (ListCursor* c = activeCursor()) c->up();
    accel.release();
}

void onDown(const ButtonEvent& ev) {
    if (ev.eventType != ButtonEvent_Pressed) return;
    lastUseMs = now();
    if (ListCursor* c = activeCursor()) c->down();
    accel.release();
}

void onSide(const ButtonEvent& ev, int dir) {
    if (screen != Screen::Detail || detailRowCount == 0) return;
    if (ev.eventType == ButtonEvent_Released) { if (accelDir == dir) accel.release(); return; }
    if (ev.eventType != ButtonEvent_Pressed) return;
    lastUseMs = now();
    const Row row = currentRow();
    adjust(row, dir, 1);
    if (row == Row::Bri || row == Row::R || row == Row::G || row == Row::B || row == Row::Volume) {
        accel.press(now());
        accelDir = dir;
        accelRow = row;
    }
}
void onLeft(const ButtonEvent& ev) { onSide(ev, -1); }
void onRight(const ButtonEvent& ev) { onSide(ev, +1); }

// A short press acts on release; a hold of a second acts at once (Held)
// and its release does nothing more.
void activate() {
    switch (screen) {
        case Screen::Message:
            if (msg == Msg::JoinFailed || msg == Msg::Unreachable || msg == Msg::BadReply) retry();
            break;
        case Screen::Top: {
            const int i = topCur.selected();
            if (topIsRooms(i)) openRooms();
            else if (topIsStatus(i)) { screen = Screen::Status; statusCur.reset(kStatusRows, kRows); }
            else if (topIsFav(i)) openEntity(L->fav[i], Screen::Top);
            break;
        }
        case Screen::Areas:
            if (areasLoaded && L->areaCount > 0) openRoom(L->areas[areaCur.selected()]);
            break;
        case Screen::AreaList:
            if (roomLoaded && L->roomRowCount > 0) openEntity(L->roomRows[roomCur.selected()], Screen::AreaList);
            break;
        case Screen::Detail:
            if (!detailRowCount) break;
            switch (currentRow()) {
                case Row::Playback: speakerAction(Action::Type::PlayPause); break;
                case Row::Mute: speakerAction(Action::Type::Mute); break;
                case Row::Power:  setPower(!d.on); break;
                case Row::Preset: applyPreset(); break;
                case Row::Effect: openEffects(); break;
                default: break;
            }
            break;
        case Screen::Effects:
            if (effectsLoaded && L->effectCount > 0) setEffect(L->effects[fxCur.selected()]);
            screen = Screen::Detail;
            break;
        case Screen::Status:
            if (statusCur.selected() == 0) request(Fetch::Favorites, nullptr);
            else if (statusCur.selected() == 1) HomeRemote::Config::setMirror(!HomeRemote::Config::mirror());
            break;
    }
}

void longHold() {
    if (screen == Screen::Top) {
        const int i = topCur.selected();
        if (topIsFav(i)) runOrToggle(L->fav[i], true);
    } else if (screen == Screen::AreaList) {
        if (roomLoaded && L->roomRowCount > 0) runOrToggle(L->roomRows[roomCur.selected()], true);
    }
}

void onEnter(const ButtonEvent& ev) {
    if (ev.eventType == ButtonEvent_Pressed) {
        enterArmed = true;
        heldFired = false;
        lastUseMs = now();
        return;
    }
    if (ev.eventType == ButtonEvent_Held) {
        if (!enterArmed || heldFired) return;
        if (screen == Screen::Top || screen == Screen::AreaList) {
            heldFired = true;
            longHold();
        }
        return;
    }
    if (ev.eventType != ButtonEvent_Released || !enterArmed) return;
    enterArmed = false;
    if (heldFired) { heldFired = false; return; }
    activate();
}

void onBack(const ButtonEvent& ev) {
    if (ev.eventType != ButtonEvent_Released) return;
    lastUseMs = now();
    switch (screen) {
        case Screen::Message:
        case Screen::Top:      exitApp(); break;
        case Screen::Status:   screen = Screen::Top; break;
        case Screen::Areas:    screen = Screen::Top; break;
        case Screen::AreaList: screen = Screen::Areas; break;
        case Screen::Detail:   leaveDetail(); break;
        case Screen::Effects:  screen = Screen::Detail; break;
    }
}

// ---- drawing --------------------------------------------------------------------------

// Cuts text to fit width pixels, ending in ".." when cut.
void fitText(const char* in, char* out, size_t cap, int width) {
    snprintf(out, cap, "%s", in);
    size_t n = strlen(out);
    if (display.getStringWidth(out, (uint16_t)n) <= width) return;
    while (n > 0) {
        n--;
        if (n + 3 > cap) continue;
        out[n] = '.';
        out[n + 1] = '.';
        out[n + 2] = '\0';
        if (display.getStringWidth(out, (uint16_t)(n + 2)) <= width) return;
    }
    out[0] = '\0';
}

bool busyNow() {
    return Client::actionBusy() || (Client::fetchBusy() && lastReqKind != Fetch::Entity) ||
           Client::link() == Client::Link::Joining;
}

void drawTitle(const char* text) {
    char line[kNameMax + 8];
    display.setColor(WHITE);
    display.fillRect(0, 0, kScreenW, kTitleH);
    display.setColor(BLACK);
    display.setTextAlignment(TEXT_ALIGN_CENTER);
    fitText(text, line, sizeof(line), kScreenW - 20);
    display.drawString(kScreenW / 2, 1, line);
    if (busyNow()) {
        // A small "working" mark in the corner.
        const int dots = (int)((now() / 300) % 3) + 1;
        for (int i = 0; i < dots; i++) display.fillRect(kScreenW - 10 + i * 3, 9, 2, 2);
    }
    display.setColor(WHITE);
    display.setTextAlignment(TEXT_ALIGN_LEFT);
}

using LabelFn = void (*)(int, char*, size_t, const char**);

void drawList(const ListCursor& cur, LabelFn label) {
    char line[kNameMax + 16];
    char cut[kNameMax + 16];
    if (cur.selected() != focusFor || focusScreen != screen) {
        focusFor = cur.selected();
        focusScreen = screen;
        focusLabel.restart(now());
    }
    for (int r = 0; r < cur.visible(); r++) {
        const int idx = cur.top() + r;
        const int y = kListY + r * kRowH;
        const char* right = nullptr;
        label(idx, line, sizeof(line), &right);
        const int rightW = right ? display.getStringWidth(right, (uint16_t)strlen(right)) + 4 : 0;
        const int boxW = kRowW - 2 * kTextX - rightW;
        const bool sel = idx == cur.selected();
        if (sel) {
            display.setColor(WHITE);
            display.fillRect(0, y, kRowW, kRowH);
            display.setColor(BLACK);
            focusLabel.draw(kTextX, y - 1, boxW, line, false);
            // The marquee may run under the right-hand mark: clear it.
            if (right) {
                display.setColor(WHITE);
                display.fillRect(kRowW - rightW - 2, y, rightW + 2, kRowH);
                display.setColor(BLACK);
            }
        } else {
            display.setColor(WHITE);
            display.setTextAlignment(TEXT_ALIGN_LEFT);
            fitText(line, cut, sizeof(cut), boxW);
            display.drawString(kTextX, y - 1, cut);
        }
        if (right) {
            display.setTextAlignment(TEXT_ALIGN_RIGHT);
            display.drawString(kRowW - 2, y - 1, right);
        }
        display.setColor(WHITE);
        display.setTextAlignment(TEXT_ALIGN_LEFT);
    }
    // Scroll bar in the gutter.
    const int listH = kRows * kRowH;
    display.setColor(BLACK);
    display.fillRect(kRowW, kListY, kGutter, listH);
    display.setColor(WHITE);
    if (cur.count() > kRows) {
        const int thumbH = (listH * cur.visible()) / cur.count();
        const int thumbY = kListY + (listH * cur.top()) / cur.count();
        display.drawRect(kScreenW - 2, kListY, 2, listH);
        display.fillRect(kScreenW - 2, thumbY, 2, thumbH);
    }
}

void drawBar(int x, int y, int w, int value, OLEDDISPLAY_COLOR fg) {
    display.setColor(fg);
    display.drawRect(x, y, w, 7);
    const int fill = (value * (w - 2)) / 255;
    if (fill > 0) display.fillRect(x + 1, y + 1, fill, 5);
}

const char* rowName(Row row) {
    switch (row) {
        case Row::Playback: return "Play/Pause";
        case Row::Volume: return "Volume";
        case Row::Track: return "Track";
        case Row::Mute: return "Mute";
        case Row::Power:  return "Power";
        case Row::Bri:    return "Bright";
        case Row::R:      return "R";
        case Row::G:      return "G";
        case Row::B:      return "B";
        case Row::Preset: return "Color";
        case Row::Effect: return "Effect";
    }
    return "";
}

void detailValue(Row row, char* out, size_t cap) {
    switch (row) {
        case Row::Playback: snprintf(out, cap, "%s", speakerStateText(d.speakerState)); break;
        case Row::Volume:
            if (d.hasVolume) snprintf(out, cap, "%d%%", volumePercent(d.volume));
            else snprintf(out, cap, "--");
            break;
        case Row::Track: snprintf(out, cap, "%s %s", speakerSupports(d.features, PreviousTrack) ? "<" : "",
                                  speakerSupports(d.features, NextTrack) ? ">" : ""); break;
        case Row::Mute: snprintf(out, cap, "%s", d.muted ? "On" : "Off"); break;
        case Row::Power:  snprintf(out, cap, "%s", !d.available ? "Not available" : (d.on ? "On" : "Off")); break;
        case Row::Bri:    snprintf(out, cap, "%d%%", d.on ? brightnessPercent(d.brightness) : 0); break;
        case Row::R:      snprintf(out, cap, "%u", (unsigned)d.r); break;
        case Row::G:      snprintf(out, cap, "%u", (unsigned)d.g); break;
        case Row::B:      snprintf(out, cap, "%u", (unsigned)d.b); break;
        case Row::Preset: snprintf(out, cap, "< %s >", preset(presetIdx).name); break;
        case Row::Effect: snprintf(out, cap, "%s >", d.effect[0] ? d.effect : "None"); break;
    }
    // The effect is kept exact for sending; only this copy is drawn.
    displayText(out, out, cap);
}

void drawDetail() {
    char hex[8];
    char value[kEffectMax + 16];
    char cut[kEffectMax + 16];
    // Title: the name, with the colour code always at the right.
    display.setColor(WHITE);
    display.fillRect(0, 0, kScreenW, kTitleH);
    display.setColor(BLACK);
    const int hexW = d.canColor ? 46 : 0;
    titleLabel.draw(2, 1, kScreenW - 4 - hexW, d.name, false);
    if (d.canColor) {
        display.setColor(WHITE);
        display.fillRect(kScreenW - hexW - 2, 0, hexW + 2, kTitleH);
        display.setColor(BLACK);
        formatHex(d.r, d.g, d.b, hex);
        display.setTextAlignment(TEXT_ALIGN_RIGHT);
        display.drawString(kScreenW - 1, 1, hex);
    }
    display.setColor(WHITE);
    display.setTextAlignment(TEXT_ALIGN_LEFT);

    const bool speaker = d.kind == Kind::Speaker;
    const int listY = speaker ? 28 : kListY;
    const int visibleRows = speaker ? 3 : kRows;
    if (speaker) {
        char track[kMediaTextMax * 2 + 4];
        if (!d.available) snprintf(track, sizeof(track), "Not available");
        else if (d.title[0] && (d.speakerState == SpeakerState::Playing || d.speakerState == SpeakerState::Paused))
            snprintf(track, sizeof(track), "%s%s%s", d.title, d.artist[0] ? " - " : "", d.artist);
        else snprintf(track, sizeof(track), "%s", speakerStateText(d.speakerState));
        mediaLabel.draw(2, 14, kScreenW - 4, track, false);
        if (!detailRowCount) display.drawString(2, listY, "No controls available");
    }

    const int slider = sliderNow();
    for (int r = 0; r < detailCur.visible(); r++) {
        const int idx = detailCur.top() + r;
        const Row row = detailRows[idx];
        const int y = listY + r * kRowH;
        const bool sel = idx == detailCur.selected();
        const OLEDDISPLAY_COLOR fg = sel ? BLACK : WHITE;
        if (sel) {
            display.setColor(WHITE);
            display.fillRect(0, y, kRowW, kRowH);
        }
        display.setColor(fg);
        display.setTextAlignment(TEXT_ALIGN_LEFT);
        display.drawString(2, y - 1, rowName(row));
        detailValue(row, value, sizeof(value));
        const bool bar = (row == Row::Volume && d.hasVolume) || row == Row::Bri || row == Row::R || row == Row::G || row == Row::B;
        if (bar) {
            const int level = row == Row::Volume ? d.volume : row == Row::Bri ? (d.on ? d.brightness : 0)
                            : row == Row::R ? d.r : row == Row::G ? d.g : d.b;
            // The value is right-aligned at kRowW - 2; "100%" is ~25 px wide, so
            // the bar ends ~8 px before the widest value can start.
            const int kBarX = speaker ? 42 : 36, kBarW = speaker ? 46 : 52;
            drawBar(kBarX, y + 3, kBarW, level, fg);
            if ((row == Row::Bri || row == Row::Volume) && !take.engaged()) {
                // Where the slider is, until it picks the level up.
                const int gx = kBarX + 1 + (slider * (kBarW - 3)) / 255;
                display.setColor(INVERSE);
                display.drawVerticalLine(gx, y + 1, kRowH - 2);
                display.drawVerticalLine(gx + 1, y + 1, kRowH - 2);
            }
            display.setColor(fg);
            display.setTextAlignment(TEXT_ALIGN_RIGHT);
            display.drawString(kRowW - 2, y - 1, value);
        } else {
            fitText(value, cut, sizeof(cut), kRowW - (row == Row::Playback ? 66 : 44));
            display.setTextAlignment(TEXT_ALIGN_RIGHT);
            display.drawString(kRowW - 2, y - 1, cut);
        }
        display.setColor(WHITE);
        display.setTextAlignment(TEXT_ALIGN_LEFT);
    }
    const int listH = visibleRows * kRowH;
    display.setColor(BLACK);
    display.fillRect(kRowW, listY, kGutter, listH);
    display.setColor(WHITE);
    if (detailRowCount > visibleRows) {
        const int thumbH = (listH * detailCur.visible()) / detailRowCount;
        const int thumbY = listY + (listH * detailCur.top()) / detailRowCount;
        display.drawRect(kScreenW - 2, listY, 2, listH);
        display.fillRect(kScreenW - 2, thumbY, 2, thumbH);
    }
}

void drawLines(const char* a, const char* b, const char* c, const char* hint) {
    display.setColor(WHITE);
    display.setTextAlignment(TEXT_ALIGN_CENTER);
    if (a) display.drawString(kScreenW / 2, 16, a);
    if (b) display.drawString(kScreenW / 2, 28, b);
    if (c) display.drawString(kScreenW / 2, 40, c);
    if (hint) display.drawString(kScreenW / 2, 52, hint);
    display.setTextAlignment(TEXT_ALIGN_LEFT);
}

void drawMessage() {
    drawTitle("Home Remote");
    switch (msg) {
        case Msg::Joining:
            drawLines(Client::link() == Client::Link::Joining ? "Joining WiFi..." : "Loading...",
                      nullptr, nullptr, "Back: exit");
            break;
        case Msg::NotSetUp:
            drawLines("Not set up yet.", "Connect over USB", "to set it up.", "Back: exit");
            break;
        case Msg::NoWifi:
            drawLines("Set up WiFi first", "in Settings.", nullptr, "Back: exit");
            break;
        case Msg::RadioBusy:
            drawLines("WiFi is busy.", "Try again soon.", nullptr, "Back: exit");
            break;
        case Msg::NoMemory:
            drawLines("Not enough memory.", "Restart and", "try again.", "Back: exit");
            break;
        case Msg::JoinFailed:
            drawLines("Couldn't join WiFi.", nullptr, nullptr, "Enter: retry  Back: exit");
            break;
        case Msg::Unreachable:
            drawLines("Can't reach", "Home Assistant.", nullptr, "Enter: retry  Back: exit");
            break;
        case Msg::Rejected:
            drawLines("Home Assistant did", "not accept the", "access key.", "Back: exit");
            break;
        case Msg::BadReply:
            drawLines("Unexpected answer", "from Home Assistant.", nullptr, "Enter: retry  Back: exit");
            break;
        case Msg::None:
            break;
    }
}


const char* connectionState() {
    const char* state = "Not checked";
    if (Client::fetchBusy() && lastReqKind == Fetch::Favorites) state = "Checking...";
    else if (Client::link() == Client::Link::JoinFailed) state = "WiFi not joined";
    else if (everAnswered) {
        switch (lastOutcome) {
            case Outcome::Ok:          state = "Connected"; break;
            case Outcome::Unreachable: state = "Can't reach it"; break;
            case Outcome::Rejected:    state = "Key not accepted"; break;
            default:                   state = "Unexpected answer"; break;
        }
    }
    return state;
}

void statusLabel(int i, char* out, size_t cap, const char** right) {
    *right = nullptr;
    switch (i) {
        case 0:
            snprintf(out, cap, "%s", connectionState());
            *right = "Check";
            break;
        case 1:
            snprintf(out, cap, "Mirror on LEDs");
            *right = HomeRemote::Config::mirror() ? "On" : "Off";
            break;
        case 2:
            snprintf(out, cap, "%s", host[0] ? host + 7 : "Not set");   // after "http://"
            break;
        default: {
            char net[WifiList::kNameMax + 1];
            Client::networkName(net, sizeof(net));
            snprintf(out, cap, "WiFi: %s", net[0] ? net : "-");
            break;
        }
    }
}

void drawStatus() {
    drawTitle("Status");
    statusCur.setCount(kStatusRows);
    drawList(statusCur, statusLabel);
}

void drawToast() {
    if (!toast[0]) return;
    if (now() - toastAt > kToastMs) { toast[0] = '\0'; return; }
    char cut[sizeof(toast)];
    fitText(toast, cut, sizeof(cut), kScreenW - 6);
    display.setColor(BLACK);
    display.fillRect(0, kScreenH - 13, kScreenW, 13);
    display.setColor(WHITE);
    display.drawRect(0, kScreenH - 13, kScreenW, 13);
    display.setTextAlignment(TEXT_ALIGN_CENTER);
    display.drawString(kScreenW / 2, kScreenH - 13, cut);
    display.setTextAlignment(TEXT_ALIGN_LEFT);
}

#ifdef CF_TEST_CLI
const char* screenName(Screen s) {
    switch (s) {
        case Screen::Message:  return "message";
        case Screen::Top:      return "top";
        case Screen::Areas:    return "rooms";
        case Screen::AreaList: return "room";
        case Screen::Detail:   return d.kind == Kind::Speaker ? "speaker" : "light";
        case Screen::Effects:  return "effects";
        case Screen::Status:   return "status";
    }
    return "?";
}

const char* msgName(Msg m) {
    switch (m) {
        case Msg::None:        return "none";
        case Msg::Joining:     return "joining";
        case Msg::NotSetUp:    return "not-set-up";
        case Msg::NoWifi:      return "no-wifi";
        case Msg::RadioBusy:   return "radio-busy";
        case Msg::NoMemory:    return "no-memory";
        case Msg::JoinFailed:  return "join-failed";
        case Msg::Unreachable: return "unreachable";
        case Msg::Rejected:    return "rejected";
        case Msg::BadReply:    return "bad-reply";
    }
    return "?";
}
#endif

template <typename T>
T* allocate() {
    void* mem = heap_caps_calloc(1, sizeof(T), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!mem) mem = calloc(1, sizeof(T));
    return mem ? new (mem) T() : nullptr;
}

} // namespace

void begin() {
    // WiFi never starts beside a Bluetooth stack started earlier in this
    // power cycle (the Music Player keeps it): as the portal does, restart
    // and reopen this app in a clean power cycle (`boothome`, AppManager).
    if (esp_bt_controller_get_status() != ESP_BT_CONTROLLER_STATUS_IDLE) {
        display.clear();
        display.setColor(WHITE);
        display.setFont(ArialMT_Plain_10);
        display.setTextAlignment(TEXT_ALIGN_CENTER);
        display.drawString(64, 27, "Opening Home Remote...");
        display.display();
        Preferences prefs;
        if (prefs.begin("bootcfg", false)) {
            prefs.putBool("skipanim", true);
            prefs.putBool("boothome", true);
            prefs.end();
        }
        Serial.println("[hr] begin=bluetooth-active restart=1");
        Serial.flush();
        delay(50);
        esp_restart();
    }

    auto& buttons = HAL::buttonManager();
    buttons.registerCallback(button_UpIndex, onUp);
    buttons.registerCallback(button_DownIndex, onDown);
    buttons.registerCallback(button_LeftIndex, onLeft);
    buttons.registerCallback(button_RightIndex, onRight);
    buttons.registerCallback(button_EnterIndex, onEnter);
    buttons.registerCallback(button_SelectIndex, onBack);
    setColorsOff();
    ledsLit = false;

    enterArmed = heldFired = false;
    favLoaded = areasLoaded = roomLoaded = effectsLoaded = false;
    refreshAfterAction = false;
    everAnswered = false;
    lastReqKind = Fetch::None;
    lastReqSeq = 0;
    toast[0] = '\0';
    focusFor = -1;
    accel.release();
    lastUseMs = now();
    lastSlider = sliderNow();
    topCur.reset(1, kRows);
    if (!HomeRemote::Config::url(host, sizeof(host))) host[0] = '\0';

    if (!L) L = allocate<Lists>();
    if (!L) { showMessage(Msg::NoMemory); return; }
    L->favCount = L->roomRowCount = L->areaCount = L->effectCount = 0;
    topCur.reset(topCount(), kRows);

    switch (Client::start()) {
        case Client::StartError::None:
            showMessage(Msg::Joining);
            request(Fetch::Favorites, nullptr);
            break;
        case Client::StartError::NotConfigured: showMessage(Msg::NotSetUp); break;
        case Client::StartError::NoWifi:        showMessage(Msg::NoWifi); break;
        case Client::StartError::RadioBusy:     showMessage(Msg::RadioBusy); break;
        case Client::StartError::NoMemory:      showMessage(Msg::NoMemory); break;
    }
}

void end() {
    auto& buttons = HAL::buttonManager();
    buttons.unregisterCallback(button_UpIndex);
    buttons.unregisterCallback(button_DownIndex);
    buttons.unregisterCallback(button_LeftIndex);
    buttons.unregisterCallback(button_RightIndex);
    buttons.unregisterCallback(button_EnterIndex);
    buttons.unregisterCallback(button_SelectIndex);
    accel.release();
    if (Client::workerAlive()) {
        display.clear();
        drawTitle("Home Remote");
        drawLines("Closing...", nullptr, nullptr, nullptr);
        display.display();
        if ((screen == Screen::Detail || screen == Screen::Effects) && sendGate().dirty()) {
            // Volume obeys the same send interval even when the app closes.
            const uint32_t flushAt = now();
            while (d.kind == Kind::Speaker && !sendGate().ready(now(), Client::actionBusy()) &&
                   now() - flushAt < kFlushWaitMs) delay(10);
            if (d.kind != Kind::Speaker || sendGate().ready(now(), Client::actionBusy())) sendNow();
        }
        const uint32_t started = now();
        while (Client::actionBusy() && now() - started < kFlushWaitMs) delay(10);
        Client::stop();
    }
    screen = Screen::Message;
    msg = Msg::None;
    ledsOff();
}

void update() {
    const uint32_t t = now();
    if (L) {
        handleFetch();
        handleAction();
    }

    if (screen == Screen::Detail || screen == Screen::Effects) {
        if (screen == Screen::Detail && accel.active()) {
            const int btn = accelDir > 0 ? button_RightIndex : button_LeftIndex;
            if (!HAL::buttonManager().isPressed(btn)) accel.release();
            else if (const int steps = accel.steps(t)) adjust(accelRow, accelDir, steps);
        }
        const int slider = sliderNow();
        if (slider - lastSlider > SoftTakeover::kTolerance || lastSlider - slider > SoftTakeover::kTolerance) {
            lastSlider = slider;
            lastUseMs = t;
        }
        int level = 0;
        if (screen == Screen::Detail && !leaveAfterVolume && d.available) {
            if (d.kind == Kind::Speaker) {
                if (d.hasVolume && speakerSupports(d.features, VolumeSet) && take.update(slider, &level))
                    setVolume(level, true);
            } else if (d.canDim && take.update(slider, &level)) setBrightness(level, true);
        }
        sendIfDue();
        if (leaveAfterVolume && !sendGate().dirty()) leaveDetail();
        // While the light screen is up, read it about once a second.
        if ((screen == Screen::Detail || screen == Screen::Effects) &&
            !Client::fetchBusy() && !sendGate().dirty() && t - lastPollMs >= kPollMs) {
            lastPollMs = t;
            request(Fetch::Entity, d.id);
        }
    }

    // Idle sleep: held off while the network is working for a screen and
    // for a while after the last use of a light screen.
    const bool lightScreen = screen == Screen::Detail || screen == Screen::Effects;
    if ((lightScreen && t - lastUseMs < kAwakeCapMs) || (!lightScreen && busyNow()) ||
        Client::actionBusy()) {
        millis_APP_LASTINTERACTION = millis_NOW;
    }

    display.clear();
    display.setFont(ArialMT_Plain_10);
    switch (screen) {
        case Screen::Message:
            drawMessage();
            break;
        case Screen::Top:
            drawTitle("Home Remote");
            topCur.setCount(topCount());
            drawList(topCur, topLabel);
            break;
        case Screen::Areas:
            drawTitle("Rooms");
            areaCur.setCount(areaRows());
            drawList(areaCur, areaLabel);
            break;
        case Screen::AreaList:
            drawTitle(L->areaName);
            roomCur.setCount(roomRows());
            drawList(roomCur, roomLabel);
            break;
        case Screen::Detail:
            drawDetail();
            break;
        case Screen::Effects:
            drawTitle("Effect");
            fxCur.setCount(fxRows());
            drawList(fxCur, fxLabel);
            break;
        case Screen::Status:
            drawStatus();
            break;
    }
    drawToast();
    updateLeds();
    display.display();
}

#ifdef CF_TEST_CLI
void printState() {
    char label[kNameMax + 16] = {0};
    const char* right = nullptr;
    int sel = -1, rows = 0;
    switch (screen) {
        case Screen::Top:      sel = topCur.selected(); rows = topCount(); topLabel(sel, label, sizeof(label), &right); break;
        case Screen::Areas:    sel = areaCur.selected(); rows = areaRows(); areaLabel(sel, label, sizeof(label), &right); break;
        case Screen::AreaList: sel = roomCur.selected(); rows = roomRows(); roomLabel(sel, label, sizeof(label), &right); break;
        case Screen::Effects:  sel = fxCur.selected(); rows = fxRows(); fxLabel(sel, label, sizeof(label), &right); break;
        case Screen::Status:   sel = statusCur.selected(); rows = kStatusRows; statusLabel(sel, label, sizeof(label), &right); break;
        case Screen::Detail:
            sel = detailCur.selected();
            rows = detailRowCount;
            snprintf(label, sizeof(label), "%s", detailRowCount ? rowName(currentRow()) : "");
            break;
        default: break;
    }
    const char* selectedId = "-";
    if (screen == Screen::Top && topIsFav(sel)) selectedId = L->fav[sel].id;
    else if (screen == Screen::AreaList && roomLoaded && L->roomRowCount > 0) selectedId = L->roomRows[sel].id;
    else if (screen == Screen::Detail || screen == Screen::Effects) selectedId = d.id;
    const Client::Link link = Client::link();
    const char* linkName = link == Client::Link::Ready ? "ready"
                         : link == Client::Link::Joining ? "joining"
                         : link == Client::Link::JoinFailed ? "join-failed" : "off";
    Serial.printf("[cmd] hr.state=%s msg=%s link=%s sel=%d rows=%d fav=%d right=%s id=%s label=%s\n",
                  screenName(screen), msgName(msg), linkName, sel, rows,
                  (L && favLoaded) ? L->favCount : -1, right ? right : "-", selectedId, label);
    if (screen == Screen::Detail && d.kind == Kind::Speaker) {
        Serial.printf("[cmd] hr.speaker=%s volume=%d known=%d muted=%d features=%lu takeover=%d\n",
                      d.id, volumePercent(d.volume), d.hasVolume ? 1 : 0, d.muted ? 1 : 0,
                      (unsigned long)d.features, take.engaged() ? 1 : 0);
    } else if (screen == Screen::Detail || screen == Screen::Effects) {
        char hex[8];
        formatHex(d.r, d.g, d.b, hex);
        Serial.printf("[cmd] hr.light=%s on=%d bri=%u rgb=%u,%u,%u hex=%s effect=%s takeover=%d\n", d.id,
                      d.on ? 1 : 0, (unsigned)d.brightness, (unsigned)d.r, (unsigned)d.g,
                      (unsigned)d.b, hex, d.effect[0] ? d.effect : "-", take.engaged() ? 1 : 0);
    }
}
#endif

} // namespace HomeRemoteApp

#endif // HOST_TEST
