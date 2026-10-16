// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#include "HomeRemoteClient.h"

#ifndef HOST_TEST

#include <Arduino.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <atomic>
#include <new>
#include <stdio.h>
#include <string.h>

#include "CloudSync.h"
#include "HomeRemoteConfig.h"
#include "SavedWifi.h"

namespace HomeRemote {
namespace Client {
namespace {

constexpr uint32_t kCallMs = 4000;          // per HTTP call
constexpr uint32_t kJoinMs = 10000;         // each join attempt
constexpr uint32_t kStackBytes = 8192;
constexpr size_t kRequestMax = 4096;        // a template body is ~1.5 KB
constexpr size_t kResponseMax = 8192;       // 32 rows of at most ~200 bytes

// Shared between the app (loop task) and the worker, under g_lock.
struct Shared {
    bool fetchPending = false;
    bool fetchRunning = false;
    Fetch fetchKind = Fetch::None;
    char fetchArg[kIdMax + 1] = {0};
    uint32_t fetchSeq = 0;

    bool actionPending = false;
    bool actionRunning = false;
    Action action;
    uint32_t actionSeq = 0;

    bool fetchReady = false;
    FetchResult fetchMeta;
    Entity rows[kMaxRows];
    Area areas[kMaxRows];
    char effects[kMaxEffects][kEffectMax + 1] = {};

    bool actionReady = false;
    uint32_t actionDoneSeq = 0;
    Outcome actionOutcome = Outcome::Ok;

    Link link = Link::Off;
    char network[WifiList::kNameMax + 1] = {0};
};

// The worker's own buffers (it alone touches them while it runs).
struct Private {
    char base[kUrlMax + 1] = {0};
    char token[kTokenMax + 1] = {0};
    char url[kUrlMax + 64] = {0};
    char request[kRequestMax] = {0};
    char response[kResponseMax] = {0};
    esp_http_client_handle_t client = nullptr;
};

Shared* g_shared = nullptr;
Private* g_priv = nullptr;
SemaphoreHandle_t g_lock = nullptr;
TaskHandle_t g_task = nullptr;
std::atomic<bool> g_stop{false};
std::atomic<bool> g_alive{false};
uint32_t g_seq = 0;

// PSRAM when there is some: allocated on first use, kept for the power
// cycle (a worker that is still finishing may hold them).
template <typename T>
T* allocate() {
    void* mem = heap_caps_calloc(1, sizeof(T), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!mem) mem = calloc(1, sizeof(T));
    return mem ? new (mem) T() : nullptr;
}

struct Lock {
    Lock() { xSemaphoreTake(g_lock, portMAX_DELAY); }
    ~Lock() { xSemaphoreGive(g_lock); }
};

void setLink(Link l) {
    Lock lock;
    g_shared->link = l;
}

bool stopping() {
    return g_stop.load() || CloudSync::radioReleaseRequested();
}

#ifdef CF_TEST_CLI
const char* fetchName(Fetch kind) {
    switch (kind) {
        case Fetch::Favorites:    return "favorites";
        case Fetch::Areas:        return "areas";
        case Fetch::AreaEntities: return "area";
        case Fetch::Entity:       return "entity";
        case Fetch::Effects:      return "effects";
        default:                  return "none";
    }
}

const char* outcomeName(Outcome o) {
    switch (o) {
        case Outcome::Ok:          return "ok";
        case Outcome::Unreachable: return "unreachable";
        case Outcome::Rejected:    return "rejected";
        default:                   return "bad-reply";
    }
}
#endif

void closeClient(Private& p) {
    if (p.client) {
        esp_http_client_cleanup(p.client);
        p.client = nullptr;
    }
}

// One POST to the configured address. Returns true when a status line came
// back (status set); the body, when keepBody, is in p.response (cut at the
// last whole line if it did not fit) and its length in *respLen.
bool post(Private& p, const char* path, const char* body, size_t bodyLen, int& status,
          bool keepBody, size_t* respLen, bool retried = false) {
    status = 0;
    if (respLen) *respLen = 0;
    p.response[0] = '\0';
    if (WiFi.status() != WL_CONNECTED || stopping()) return false;
    const int n = snprintf(p.url, sizeof(p.url), "%s%s", p.base, path);
    if (n <= 0 || (size_t)n >= sizeof(p.url)) return false;
    const bool reused = p.client != nullptr;
    if (!p.client) {
        esp_http_client_config_t config = {};
        config.url = p.url;
        config.method = HTTP_METHOD_POST;
        config.timeout_ms = kCallMs;
        config.disable_auto_redirect = true;
        config.buffer_size_tx = 1024;   // the request line and headers, key included
        p.client = esp_http_client_init(&config);
        if (!p.client) return false;
    } else if (esp_http_client_set_url(p.client, p.url) != ESP_OK) {
        closeClient(p);
        return false;
    }
    esp_http_client_set_method(p.client, HTTP_METHOD_POST);
    esp_http_client_set_timeout_ms(p.client, kCallMs);
    char auth[kTokenMax + 8];
    snprintf(auth, sizeof(auth), "Bearer %s", p.token);
    const bool authSet = esp_http_client_set_header(p.client, "Authorization", auth) == ESP_OK;
    memset(auth, 0, sizeof(auth));
    if (!authSet ||
        esp_http_client_set_header(p.client, "Content-Type", "application/json") != ESP_OK) {
        closeClient(p);
        return false;
    }
    bool ok = esp_http_client_open(p.client, (int)bodyLen) == ESP_OK;
    if (ok) ok = esp_http_client_write(p.client, body, (int)bodyLen) == (int)bodyLen;
    if (ok) ok = esp_http_client_fetch_headers(p.client) >= 0;
    if (!ok) {
        closeClient(p);
        // The server may have closed the kept connection while idle:
        // nothing was answered, so try once on a fresh one.
        if (reused && !retried && !stopping())
            return post(p, path, body, bodyLen, status, keepBody, respLen, true);
        return false;
    }
    status = esp_http_client_get_status_code(p.client);
    size_t len = 0;
    bool truncated = false;
    char drain[256];
    for (;;) {
        int got;
        if (keepBody && !truncated && len + 1 < sizeof(p.response)) {
            got = esp_http_client_read(p.client, p.response + len, (int)(sizeof(p.response) - 1 - len));
            if (got > 0) len += (size_t)got;
        } else {
            got = esp_http_client_read(p.client, drain, sizeof(drain));
            if (got > 0 && keepBody) truncated = true;
        }
        if (got < 0) { ok = false; break; }
        if (got == 0) {
            ok = esp_http_client_is_complete_data_received(p.client);
            break;
        }
        if (keepBody && len + 1 >= sizeof(p.response)) truncated = true;
        if (stopping()) { ok = false; break; }
    }
    if (keepBody) {
        if (truncated) {
            // Keep only whole lines.
            while (len && p.response[len - 1] != '\n') len--;
        }
        p.response[len] = '\0';
        if (respLen) *respLen = len;
    }
    if (!ok || status < 200 || status >= 300) closeClient(p);
    // A status line came back even when the body was cut short.
    return status != 0;
}

void publishFetch(Fetch kind, uint32_t seq, Outcome outcome, int status, const Private& p,
                  size_t len) {
    Lock lock;
    Shared& s = *g_shared;
    s.fetchRunning = false;
    if (s.fetchPending) return;   // a newer request replaced this one
    FetchResult meta;
    meta.seq = seq;
    meta.kind = kind;
    meta.outcome = outcome;
    meta.status = status;
    if (outcome == Outcome::Ok) {
        switch (kind) {
            case Fetch::Areas:
                meta.count = parseAreas(p.response, len, s.areas, kMaxRows);
                break;
            case Fetch::Effects:
                meta.count = parseEffects(p.response, len, s.effects, kMaxEffects);
                break;
            default:
                meta.count = parseEntities(p.response, len, s.rows, kMaxRows);
                break;
        }
    }
    s.fetchMeta = meta;
    s.fetchReady = true;
}

void runFetch(Private& p, Fetch kind, const char* arg, uint32_t seq) {
    const uint32_t started = millis();
    size_t n = 0;
    switch (kind) {
        case Fetch::Favorites:    n = favoritesBody(p.request, sizeof(p.request)); break;
        case Fetch::Areas:        n = areasBody(p.request, sizeof(p.request)); break;
        case Fetch::AreaEntities: n = areaEntitiesBody(arg, p.request, sizeof(p.request)); break;
        case Fetch::Entity:       n = entityBody(arg, p.request, sizeof(p.request)); break;
        case Fetch::Effects:      n = effectsBody(arg, p.request, sizeof(p.request)); break;
        default: break;
    }
    int status = 0;
    size_t len = 0;
    Outcome outcome = Outcome::BadReply;
    if (n > 0) outcome = classify(post(p, "/api/template", p.request, n, status, true, &len), status);
    publishFetch(kind, seq, outcome, status, p, len);
#ifdef CF_TEST_CLI
    Serial.printf("[hr] fetch=%s status=%d outcome=%s bytes=%u ms=%lu\n", fetchName(kind), status,
                  outcomeName(outcome), (unsigned)len, (unsigned long)(millis() - started));
#else
    (void)started;
#endif
}

void runAction(Private& p, const Action& a, uint32_t seq) {
    const uint32_t started = millis();
    char path[48] = {0};
    int status = 0;
    Outcome outcome = Outcome::BadReply;
    const size_t n = serviceBody(a, p.request, sizeof(p.request));
    if (n > 0 && servicePath(a, path, sizeof(path)))
        outcome = classify(post(p, path, p.request, n, status, false, nullptr), status);
    {
        Lock lock;
        g_shared->actionRunning = false;
        g_shared->actionDoneSeq = seq;
        g_shared->actionOutcome = outcome;
        g_shared->actionReady = true;
    }
#ifdef CF_TEST_CLI
    Serial.printf("[hr] action=%s entity=%s status=%d outcome=%s ms=%lu\n", path[0] ? path : "-", a.entity, status,
                  outcomeName(outcome), (unsigned long)(millis() - started));
#else
    (void)started;
#endif
}

// Joins a saved network (the station is on). Publishes the link state.
bool joinWifi() {
    setLink(Link::Joining);
    if (WiFi.status() == WL_CONNECTED) WiFi.disconnect(false);
    SavedWifi::JoinOptions opt;
    opt.firstMs = kJoinMs;
    opt.fallbackMs = kJoinMs;
    opt.stop = [](void*) { return stopping(); };
    SavedWifi::JoinResult joined;
    const bool ok = SavedWifi::join(opt, joined);
    Lock lock;
    g_shared->link = ok ? Link::Ready : Link::JoinFailed;
    memcpy(g_shared->network, joined.name, sizeof(g_shared->network));
    g_shared->network[sizeof(g_shared->network) - 1] = '\0';
    if (!ok) g_shared->network[0] = '\0';
    return ok;
}

// Fails whatever is waiting (no network to send it on).
void failPending() {
    Lock lock;
    Shared& s = *g_shared;
    if (s.actionPending) {
        s.actionPending = false;
        s.actionDoneSeq = s.actionSeq;
        s.actionOutcome = Outcome::Unreachable;
        s.actionReady = true;
    }
    if (s.fetchPending) {
        s.fetchPending = false;
        FetchResult meta;
        meta.seq = s.fetchSeq;
        meta.kind = s.fetchKind;
        meta.outcome = Outcome::Unreachable;
        s.fetchMeta = meta;
        s.fetchReady = true;
    }
}

void serve(Private& p) {
    bool joined = false;
    if (WiFi.mode(WIFI_STA)) joined = joinWifi();
    else setLink(Link::JoinFailed);
    if (!joined) failPending();
    while (!stopping()) {
        bool haveAction = false, haveFetch = false;
        Action action;
        uint32_t seq = 0;
        Fetch kind = Fetch::None;
        char arg[kIdMax + 1] = {0};
        {
            Lock lock;
            Shared& s = *g_shared;
            if (s.actionPending) {
                haveAction = true;
                action = s.action;
                seq = s.actionSeq;
            } else if (s.fetchPending) {
                haveFetch = true;
                kind = s.fetchKind;
                memcpy(arg, s.fetchArg, sizeof(arg));
                seq = s.fetchSeq;
            }
        }
        if (!haveAction && !haveFetch) {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
            continue;
        }
        if (WiFi.status() != WL_CONNECTED) {
            // Lost (or never joined): one more join, else fail the request.
            closeClient(p);
            if (!joinWifi()) {
                failPending();
                continue;
            }
        }
        {
            Lock lock;
            Shared& s = *g_shared;
            if (haveAction) {
                if (!s.actionPending || s.actionSeq != seq) continue;
                s.actionPending = false;
                s.actionRunning = true;
            } else {
                if (!s.fetchPending || s.fetchSeq != seq) continue;
                s.fetchPending = false;
                s.fetchRunning = true;
            }
        }
        if (haveAction) runAction(p, action, seq);
        else runFetch(p, kind, arg, seq);
    }
}

void worker(void*) {
    Private& p = *g_priv;
    WiFi.persistent(false);   // nothing is written to the radio's own storage
    serve(p);
    closeClient(p);
    memset(p.token, 0, sizeof(p.token));
    memset(p.request, 0, sizeof(p.request));
    WiFi.disconnect(true);
    const bool off = WiFi.mode(WIFI_OFF) && WiFi.getMode() == WIFI_OFF;
    if (!off) Serial.println("[hr] radio_off=failed");
    {
        Lock lock;
        g_shared->link = Link::Off;
        g_shared->network[0] = '\0';
        g_shared->fetchRunning = false;
        g_shared->actionRunning = false;
    }
#ifdef CF_TEST_CLI
    Serial.printf("[hr] worker=end wifi=%s\n", off ? "off" : "on");
#endif
    // Released only now: whoever waits for the radio sees it off.
    CloudSync::releaseRadio();
    g_task = nullptr;
    g_alive = false;
    vTaskDelete(nullptr);
}

} // namespace

StartError start() {
    if (g_alive.load()) return StartError::RadioBusy;
    if (!g_lock) g_lock = xSemaphoreCreateMutex();
    if (!g_shared) g_shared = allocate<Shared>();
    if (!g_priv) g_priv = allocate<Private>();
    if (!g_lock || !g_shared || !g_priv) return StartError::NoMemory;

    Private& p = *g_priv;
    if (!SavedWifi::anySaved()) return StartError::NoWifi;
    if (!Config::url(p.base, sizeof(p.base)) || !Config::token(p.token, sizeof(p.token))) {
        memset(p.token, 0, sizeof(p.token));
        return StartError::NotConfigured;
    }
    if (!CloudSync::claimRadio()) {
        memset(p.token, 0, sizeof(p.token));
        return StartError::RadioBusy;
    }
    {
        Lock lock;
        Shared& s = *g_shared;
        s.fetchPending = s.fetchRunning = s.fetchReady = false;
        s.actionPending = s.actionRunning = s.actionReady = false;
        s.link = Link::Joining;
        s.network[0] = '\0';
    }
    g_stop = false;
    g_alive = true;
    if (xTaskCreate(worker, "homeremote", kStackBytes, nullptr, 1, &g_task) != pdPASS) {
        g_alive = false;
        g_task = nullptr;
        memset(p.token, 0, sizeof(p.token));
        setLink(Link::Off);
        CloudSync::releaseRadio();
        return StartError::NoMemory;
    }
#ifdef CF_TEST_CLI
    Serial.println("[hr] worker=start");
#endif
    return StartError::None;
}

bool stop(uint32_t waitMs) {
    if (!g_alive.load()) return true;
    g_stop = true;
    if (TaskHandle_t t = g_task) xTaskNotifyGive(t);
    const uint32_t started = millis();
    while (g_alive.load() && millis() - started < waitMs) delay(10);
    return !g_alive.load();
}

bool workerAlive() { return g_alive.load(); }

Link link() {
    if (!g_shared || !g_lock) return Link::Off;
    Lock lock;
    return g_shared->link;
}

void networkName(char* out, size_t cap) {
    if (!out || cap == 0) return;
    out[0] = '\0';
    if (!g_shared || !g_lock) return;
    Lock lock;
    strncpy(out, g_shared->network, cap - 1);
    out[cap - 1] = '\0';
}

uint32_t requestFetch(Fetch kind, const char* arg) {
    if (!g_alive.load() || !g_shared) return 0;
    uint32_t seq;
    {
        Lock lock;
        Shared& s = *g_shared;
        seq = ++g_seq;
        s.fetchPending = true;
        s.fetchKind = kind;
        s.fetchSeq = seq;
        s.fetchReady = false;
        strncpy(s.fetchArg, arg ? arg : "", sizeof(s.fetchArg) - 1);
        s.fetchArg[sizeof(s.fetchArg) - 1] = '\0';
    }
    if (TaskHandle_t t = g_task) xTaskNotifyGive(t);
    return seq;
}

uint32_t requestAction(const Action& action) {
    if (!g_alive.load() || !g_shared) return 0;
    uint32_t seq;
    {
        Lock lock;
        Shared& s = *g_shared;
        seq = ++g_seq;
        s.actionPending = true;
        s.action = action;
        s.actionSeq = seq;
    }
    if (TaskHandle_t t = g_task) xTaskNotifyGive(t);
    return seq;
}

bool fetchBusy() {
    if (!g_shared || !g_lock) return false;
    Lock lock;
    return g_shared->fetchPending || g_shared->fetchRunning;
}

bool actionBusy() {
    if (!g_shared || !g_lock) return false;
    Lock lock;
    return g_shared->actionPending || g_shared->actionRunning;
}

bool takeFetch(FetchResult& meta, Entity* rows, int maxRows, Area* areas, int maxAreas,
               char (*effects)[kEffectMax + 1], int maxEffects) {
    if (!g_shared || !g_lock) return false;
    Lock lock;
    Shared& s = *g_shared;
    if (!s.fetchReady) return false;
    s.fetchReady = false;
    meta = s.fetchMeta;
    int count = meta.count;
    if (meta.outcome != Outcome::Ok) count = 0;
    switch (meta.kind) {
        case Fetch::Areas:
            if (count > maxAreas) count = maxAreas;
            for (int i = 0; i < count && areas; i++) areas[i] = s.areas[i];
            break;
        case Fetch::Effects:
            if (count > maxEffects) count = maxEffects;
            for (int i = 0; i < count && effects; i++) memcpy(effects[i], s.effects[i], kEffectMax + 1);
            break;
        default:
            if (count > maxRows) count = maxRows;
            for (int i = 0; i < count && rows; i++) rows[i] = s.rows[i];
            break;
    }
    meta.count = count;
    return true;
}

bool takeAction(uint32_t& seq, Outcome& outcome) {
    if (!g_shared || !g_lock) return false;
    Lock lock;
    if (!g_shared->actionReady) return false;
    g_shared->actionReady = false;
    seq = g_shared->actionDoneSeq;
    outcome = g_shared->actionOutcome;
    return true;
}

} // namespace Client
} // namespace HomeRemote

#endif // HOST_TEST
