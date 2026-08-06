// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#include "WebPortalApp.h"
// portal_page.h is the editable source for the portal, but it is NOT what gets
// stored in the image: scripts/gzip_portal_page.py compresses it at build time
// into generated/portal_page_gz.h, which is what ships and what is served. The
// raw literal is not included here at all - including it would put both copies
// in flash and defeat the point.
#include "portal_page_gz.h"      // generated at build time from portal_page.h
#include "companion_shell_gz.h"  // generated: gzipped companion shell in flash
// companion_fallback_page.h is deliberately NOT included any more: with the
// shell embedded in flash, /web/ always renders, so the "not on the memory card
// yet" page became unreachable. The file is kept because its wording is being
// reused for an in-portal notice about the captions payload.
#include "AudioManager.h"
#include "MenuManager.h"
#include "MicCapture.h"  // shared mic pipeline for the live caption stream
#include "globals.h"
#include "RecNaming.h"   // shared index.csv row parser (lib/VoiceRecorderApp)

#include <SD.h>
#include <SPI.h>
#include <WiFi.h>
#include <esp_bt.h>
#include <esp_bt_main.h>
#include <esp_heap_caps.h>  // internal-heap health in /api/status
#include <esp_system.h>
#include <sys/time.h>    // settimeofday for POST /api/time + the time WS frame

// External fonts (thingpulse OLED lib) for the caption screen's large mode
extern const uint8_t ArialMT_Plain_16[];

// ---------------------------------------------------------------------------
// Debug logging
// ---------------------------------------------------------------------------
#define WEB_PORTAL_DEBUG 1

#if WEB_PORTAL_DEBUG
  #define WP_LOG(msg)          ESP_LOGD(TAG_MAIN, "[WebPortal] " msg)
  #define WP_LOGF(fmt, ...)    ESP_LOGD(TAG_MAIN, "[WebPortal] " fmt, ##__VA_ARGS__)
#else
  #define WP_LOG(msg)          ((void)0)
  #define WP_LOGF(fmt, ...)    ((void)0)
#endif

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
static const int PIN_SD_CLK  = 5;
static const int PIN_SD_MISO = 21;
static const int PIN_SD_MOSI = 19;
static const int PIN_SD_CS   = 8;

static const char* MEDIA_DIR      = "/media";
static const char* PLAYLIST_DIR   = "/media/playlists";
static const char* CACHE_PATH     = "/music.idx";
static const char* RECORDINGS_DIR = "/recordings";

// Live-caption internal-heap guards (the scarce resource: WiFi AP+STA + the
// browser's HTTP connection pool + the mic all draw from internal RAM).
// LIVE_HEAP_FLOOR gates starting a session at all (acquire wants ~12KB and
// must leave WiFi/lwIP a working margin); LIVE_FRAME_HEAP_FLOOR aborts a
// single 5KB frame enqueue when heap dips near empty mid-stream. Both refuse
// gracefully instead of risking an OOM hard-fault — the device must stay
// reachable. Tuned against measured hardware: AP+STA portal idles ~32KB,
// ~13KB with a browser pool attached.
static const size_t LIVE_HEAP_FLOOR       = 24000;
static const size_t LIVE_FRAME_HEAP_FLOOR = 9000;

// Max voice-note filename length (including ".wav") a portal rename may produce.
// Single source of truth shared with the recorder so both sides agree (see
// RecNaming::kMaxRecNameLen). Bounds the JSON/list buffers and keeps an
// index.csv row well within the 128-byte line reader.
static constexpr size_t REC_NAME_MAX = RecNaming::kMaxRecNameLen;

// Read one line (up to and excluding '\n') from an open File into buf. Returns
// false only at EOF with nothing read. Mirrors VoiceRecorderApp's helper —
// index.csv is small, so a byte-at-a-time read is fine. Used by the recordings
// list + the rename/delete index.csv rewrites.
static bool wpReadLine(File& f, char* buf, size_t bufSize) {
    size_t i = 0;
    bool any = false;
    int c;
    while ((c = f.read()) >= 0) {
        any = true;
        if (c == '\n') break;
        if (i + 1 < bufSize) buf[i++] = (char)c;
    }
    buf[i] = '\0';
    return any;
}

static auto& display = HAL::displayProxy();

// ---------------------------------------------------------------------------
// Lightweight ID3 tag reader (title, artist, album)
// ---------------------------------------------------------------------------
static uint32_t id3ReadSyncsafe(const uint8_t* b) {
    return ((uint32_t)b[0] << 21) | ((uint32_t)b[1] << 14) |
           ((uint32_t)b[2] << 7)  | (uint32_t)b[3];
}

static uint32_t id3ReadBE32(const uint8_t* b) {
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
           ((uint32_t)b[2] << 8)  | (uint32_t)b[3];
}

static String id3ExtractText(const uint8_t* data, int len) {
    if (len < 2) return "";
    uint8_t enc = data[0];
    const uint8_t* text = data + 1;
    int textLen = len - 1;

    if (enc == 0 || enc == 3) {
        char buf[128];
        int n = textLen > 127 ? 127 : textLen;
        memcpy(buf, text, n);
        buf[n] = '\0';
        while (n > 0 && (buf[n-1] == '\0' || buf[n-1] == ' ')) buf[--n] = '\0';
        return String(buf);
    }
    if (enc == 1 || enc == 2) {
        int start = 0;
        bool le = (enc == 1);
        if (textLen >= 2) {
            if (text[0] == 0xFF && text[1] == 0xFE) { le = true; start = 2; }
            else if (text[0] == 0xFE && text[1] == 0xFF) { le = false; start = 2; }
        }
        char buf[128];
        int out = 0;
        for (int i = start; i + 1 < textLen && out < 127; i += 2) {
            char c = le ? text[i] : text[i+1];
            if (c == '\0') break;
            buf[out++] = c;
        }
        buf[out] = '\0';
        return String(buf);
    }
    return "";
}

static void readTrackID3(const char* path, String& title, String& artist, String& album) {
    File f = SD.open(path, FILE_READ);
    if (!f) return;

    // --- ID3v2 ---
    uint8_t hdr[10];
    if (f.read(hdr, 10) == 10 && hdr[0]=='I' && hdr[1]=='D' && hdr[2]=='3') {
        uint8_t ver = hdr[3];
        uint32_t tagSize = id3ReadSyncsafe(hdr + 6);
        if (tagSize > 16384) tagSize = 16384;

        uint32_t pos = 0;
        while (pos + 10 < tagSize) {
            uint8_t fh[10];
            if (f.read(fh, 10) != 10) break;
            pos += 10;
            if (fh[0] == '\0') break;

            uint32_t fsz = (ver >= 4) ? id3ReadSyncsafe(fh + 4) : id3ReadBE32(fh + 4);
            if (fsz == 0 || fsz > tagSize - pos) break;

            bool isTIT2 = (fh[0]=='T'&&fh[1]=='I'&&fh[2]=='T'&&fh[3]=='2');
            bool isTPE1 = (fh[0]=='T'&&fh[1]=='P'&&fh[2]=='E'&&fh[3]=='1');
            bool isTALB = (fh[0]=='T'&&fh[1]=='A'&&fh[2]=='L'&&fh[3]=='B');

            if (isTIT2 || isTPE1 || isTALB) {
                int readSz = fsz > 512 ? 512 : fsz;
                uint8_t* data = (uint8_t*)malloc(readSz);
                if (data && f.read(data, readSz) == readSz) {
                    String text = id3ExtractText(data, readSz);
                    if (isTIT2) title = text;
                    else if (isTPE1) artist = text;
                    else if (isTALB) album = text;
                }
                free(data);
                if ((int)fsz > readSz) f.seek(f.position() + (fsz - readSz));
            } else {
                f.seek(f.position() + fsz);
            }
            pos += fsz;
            if (title.length() && artist.length() && album.length()) break;
        }
    }

    // --- ID3v1 fallback ---
    if (!title.length() || !artist.length()) {
        size_t sz = f.size();
        if (sz > 128) {
            f.seek(sz - 128);
            uint8_t tag[128];
            if (f.read(tag, 128) == 128 && tag[0]=='T' && tag[1]=='A' && tag[2]=='G') {
                auto trim = [](const uint8_t* src, int maxLen) -> String {
                    char buf[31];
                    memcpy(buf, src, maxLen);
                    buf[maxLen] = '\0';
                    int n = maxLen;
                    while (n > 0 && (buf[n-1]==' '||buf[n-1]=='\0')) buf[--n] = '\0';
                    return String(buf);
                };
                if (!title.length())  title  = trim(tag + 3, 30);
                if (!artist.length()) artist = trim(tag + 33, 30);
                if (!album.length())  album  = trim(tag + 63, 30);
            }
        }
    }
    f.close();
}

// JSON-escape a string (quotes, backslashes, control chars)
static String escJSON(const String& s) {
    String out;
    out.reserve(s.length() + 4);
    for (unsigned i = 0; i < s.length(); i++) {
        char c = s[i];
        if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c < 0x20) out += ' ';
        else out += c;
    }
    return out;
}

// Content type for a companion file by extension. serveStatic's built-in
// table omits .mjs and .wasm; ES-module and streaming-wasm loads fail under
// strict MIME checking unless these are exact.
static const char* webContentType(const String& path) {
    if (path.endsWith(".html")) return "text/html";
    if (path.endsWith(".mjs") || path.endsWith(".js")) return "text/javascript";
    if (path.endsWith(".css")) return "text/css";
    if (path.endsWith(".wasm")) return "application/wasm";
    if (path.endsWith(".json") || path.endsWith(".webmanifest")) return "application/json";
    if (path.endsWith(".svg")) return "image/svg+xml";
    if (path.endsWith(".woff2")) return "font/woff2";
    if (path.endsWith(".txt")) return "text/plain";
    return "application/octet-stream";
}

// A browse/download/delete/mkdir/move path from the raw file browser must be an
// absolute card path with no ".." escape. The portal trusts its own AP clients,
// but this keeps a malformed request from walking off the card root. ("/" alone
// is rejected by the callers that must never operate on the whole card.)
static bool wpPathSafe(const String& p) {
    if (p.length() == 0 || p[0] != '/') return false;
    if (p.indexOf("..") >= 0) return false;
    return true;
}

// Recursively delete a file or a (possibly non-empty) directory, like Explorer's
// "delete folder". To stay safe against SD/FAT directory-iterator invalidation we
// never mutate a directory we are actively iterating: each pass reopens the dir,
// grabs one child, closes, then deletes it. Folder sizes on this device are small,
// so the O(n^2) reopen cost is irrelevant next to the safety.
static bool wpRmRecursive(const String& path) {
    File node = SD.open(path);
    if (!node) return false;
    if (!node.isDirectory()) { node.close(); return SD.remove(path); }
    node.close();
    for (;;) {
        File dir = SD.open(path);
        if (!dir) return false;
        File child = dir.openNextFile();
        if (!child) { dir.close(); break; }   // empty -> ready to rmdir
        String childPath = child.path();
        bool childDir = child.isDirectory();
        child.close();
        dir.close();
        bool ok = childDir ? wpRmRecursive(childPath) : SD.remove(childPath);
        if (!ok) return false;
    }
    return SD.rmdir(path);
}

// Collect all .mp3 file paths recursively
static void collectMP3Paths(const char* dir, String& json, bool& first) {
    File root = SD.open(dir);
    if (!root || !root.isDirectory()) return;
    File entry;
    while ((entry = root.openNextFile())) {
        if (entry.isDirectory()) {
            String fullPath = entry.path();
            String entryName = fullPath;
            int slash = fullPath.lastIndexOf('/');
            if (slash >= 0) entryName = fullPath.substring(slash + 1);
            // Skip hidden dirs and playlist dir
            if (!entryName.startsWith(".") && fullPath != String(PLAYLIST_DIR)) {
                collectMP3Paths(fullPath.c_str(), json, first);
            }
        } else {
            String name = entry.name();
            String lower = name;
            lower.toLowerCase();
            if (lower.endsWith(".mp3")) {
                String fullPath = entry.path();
                size_t fileSize = entry.size();
                entry.close();

                // Read ID3 tags
                String title, artist, album;
                readTrackID3(fullPath.c_str(), title, artist, album);

                // Fallback title from filename
                if (!title.length()) {
                    title = name;
                    int dot = title.lastIndexOf('.');
                    if (dot > 0) title = title.substring(0, dot);
                }

                if (!first) json += ",";
                first = false;
                json += "{\"path\":\"" + escJSON(fullPath) + "\"";
                json += ",\"title\":\"" + escJSON(title) + "\"";
                json += ",\"artist\":\"" + escJSON(artist) + "\"";
                json += ",\"album\":\"" + escJSON(album) + "\"";
                json += ",\"size\":" + String((unsigned long)fileSize) + "}";
                continue;
            }
        }
        entry.close();
    }
    root.close();
}

// ---------------------------------------------------------------------------
// Singleton
// ---------------------------------------------------------------------------
WebPortalApp* WebPortalApp::instance = nullptr;
bool WebPortalApp::btReleasedThisPowerCycle = false;
WebPortalApp webPortalApp(HAL::buttonManager());

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------
WebPortalApp::WebPortalApp(ButtonManager& btnMgr)
    : buttonManager(btnMgr) {
    instance = this;
}

bool WebPortalApp::bluetoothReleasedThisPowerCycle() {
    return btReleasedThisPowerCycle;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------
void WebPortalApp::begin() {
    if (esp_bt_controller_get_status() != ESP_BT_CONTROLLER_STATUS_IDLE) {
        WP_LOG("begin: Bluetooth active; restarting before portal entry");

        display.clear();
        display.setColor(WHITE);
        display.setFont(ArialMT_Plain_10);
        display.setTextAlignment(TEXT_ALIGN_CENTER);
        display.drawString(64, 27, "Opening portal...");
        display.display();

        Preferences prefs;
        if (prefs.begin("bootcfg", false)) {
            if (prefs.putBool("skipanim", true) == 0) {
                WP_LOG("entry restart: failed to write animation skip flag");
            }
            if (prefs.putBool("bootapp", true) == 0) {
                WP_LOG("entry restart: failed to write portal boot flag");
            }
            prefs.end();
        } else {
            WP_LOG("entry restart: failed to open boot preferences");
        }

        Serial.flush();
        delay(50);
        esp_restart();
    }

    WP_LOG("begin: enter");
    instance = this;

    // Register portal controls + the caption-screen font toggle
    buttonManager.registerCallback(button_SelectIndex, onButtonBack);
    buttonManager.registerCallback(button_EnterIndex, onButtonEnter);
    buttonManager.registerCallback(button_UpIndex, onButtonUp);

    // Reset upload state
    uploadInProgress = false;
    uploadBytesReceived = 0;
    uploadBytesTotal = 0;

    // Reset live caption link state
    wsClaimedId = 0;
    rxConnectId = 0;
    rxDisconnect = false;
    rxTimeNew = false;
    rxPartialNew = false;
    rxFinalCount = 0;
    liveClientId = 0;
    liveMicHeld = false;
    liveSentFrames = 0;
    liveDroppedFrames = 0;
    liveFrameFill = 0;
    captionFinalCount = 0;
    mdnsStarted = false;
    lastMdnsAttempt = 0;
    exitConfirmPending = false;
    teardownDone = false;

    // Init SD
    initSD();

    // Count files
    fileCount = 0;
    if (sdReady) countFilesRecursive(MEDIA_DIR);

    // The portal does not play tones. Release I2S0 before WiFi claims its
    // DMA-capable internal heap; MicCapture uses the independent I2S1 port.
    HAL::audioManager().releaseI2S();

    // Stop Bluetooth to free the radio for WiFi
    WP_LOG("begin: stopping BT controller");
    btStop();
    delay(100);
    releaseBluetoothMemory();

    // Bring WiFi up in AP+STA dual mode. softAP() can fail at radio bring-up
    // when the WiFi driver can't allocate its DMA RX-buffer pool -- this build
    // runs very tight on internal RAM, so the portal context can be left with
    // too little free for esp_wifi to init. Its bool return was previously
    // ignored, which left the portal silently on "AP: 0.0.0.0" with nothing
    // broadcasting. Don't swallow it: surface on the OLED + gate captive DNS.
    staConnected = false;
    WP_LOG("begin: starting WiFi AP+STA");
    WiFi.mode(WIFI_AP_STA);
    apReady = WiFi.softAP(AP_SSID);
    delay(100);
    if (apReady) {
        WP_LOGF("begin: AP started, SSID=%s IP=%s", AP_SSID,
                WiFi.softAPIP().toString().c_str());
    } else {
        ESP_LOGE(TAG_MAIN,
                 "[WebPortal] begin: softAP() FAILED, free heap=%u -- AP not "
                 "broadcasting (WiFi DMA/RX-buffer starvation)",
                 (unsigned)ESP.getFreeHeap());
    }

    // Auto-connect to the saved network (only touches the radio if creds exist).
    loadWifiCreds();

    // Start captive portal DNS (binds to AP interface only). Skip if the AP
    // never came up -- otherwise it just binds the dead 0.0.0.0 address.
    if (apReady) {
        dnsServer.start(53, "*", WiFi.softAPIP());
    }

    // Create web server
    server = new AsyncWebServer(80);
    setupRoutes();
    server->begin();
    WP_LOG("begin: web server started");

    // Keep device awake
    millis_APP_LASTINTERACTION = millis_NOW;
}

void WebPortalApp::end() {
    teardown();
}

void WebPortalApp::teardown() {
    if (teardownDone) {
        WP_LOG("teardown: already complete");
        return;
    }
    teardownDone = true;
    WP_LOG("end: enter");

    // Tell a live listener the device is leaving, release the mic
    stopLiveSession("portal closed", /*notifyClient=*/true);

    // Close any in-progress upload
    if (uploadFile) {
        uploadFile.close();
    }
    uploadInProgress = false;

    // Stop web server (its handler list owns and frees the WebSocket)
    if (server) {
        server->end();
        delete server;
        server = nullptr;
        ws = nullptr;
    }

    // Stop DNS + mDNS
    dnsServer.stop();
    stopMDNS();

    // Stop WiFi (STA + AP)
    WiFi.disconnect(false);
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
    staConnected = false;
    delay(100);
    WP_LOG("end: WiFi stopped");

    HAL::audioManager().reclaimI2S();

    // Unregister callbacks
    buttonManager.unregisterCallback(button_SelectIndex);
    buttonManager.unregisterCallback(button_EnterIndex);
    buttonManager.unregisterCallback(button_UpIndex);

    WP_LOG("end: done");
}

void WebPortalApp::releaseBluetoothMemory() {
    esp_bluedroid_status_t bluedroidStatus = esp_bluedroid_get_status();
    if (bluedroidStatus == ESP_BLUEDROID_STATUS_ENABLED) {
        esp_err_t err = esp_bluedroid_disable();
        if (err != ESP_OK) {
            WP_LOGF("BT release: bluedroid disable failed: %s",
                    esp_err_to_name(err));
        }
        bluedroidStatus = esp_bluedroid_get_status();
    }

    if (bluedroidStatus == ESP_BLUEDROID_STATUS_INITIALIZED) {
        esp_err_t err = esp_bluedroid_deinit();
        if (err != ESP_OK) {
            WP_LOGF("BT release: bluedroid deinit failed: %s",
                    esp_err_to_name(err));
        }
    } else if (bluedroidStatus != ESP_BLUEDROID_STATUS_UNINITIALIZED) {
        WP_LOGF("BT release: unexpected bluedroid status after disable: %d",
                (int)bluedroidStatus);
    }

    esp_bt_controller_status_t controllerStatus =
        esp_bt_controller_get_status();
    if (controllerStatus == ESP_BT_CONTROLLER_STATUS_ENABLED) {
        esp_err_t err = esp_bt_controller_disable();
        if (err != ESP_OK) {
            WP_LOGF("BT release: controller disable failed: %s",
                    esp_err_to_name(err));
        }
        controllerStatus = esp_bt_controller_get_status();
    }

    if (controllerStatus == ESP_BT_CONTROLLER_STATUS_INITED) {
        esp_err_t err = esp_bt_controller_deinit();
        if (err != ESP_OK) {
            WP_LOGF("BT release: controller deinit failed: %s",
                    esp_err_to_name(err));
        }
        controllerStatus = esp_bt_controller_get_status();
    }
    if (controllerStatus != ESP_BT_CONTROLLER_STATUS_IDLE) {
        WP_LOGF("BT release: unexpected controller status before memory release: %d",
                (int)controllerStatus);
    }

    esp_err_t releaseErr = esp_bt_mem_release(ESP_BT_MODE_BTDM);
    if (releaseErr != ESP_OK) {
        // The audio runtime releases the BLE region on its own during static
        // init, and a combined-mode release fails outright once any part of
        // the region is gone (bench-measured: the failed BTDM call returned
        // ~0 KB). The Classic-BT region is the remaining reservation - claim
        // it explicitly.
        releaseErr = esp_bt_mem_release(ESP_BT_MODE_CLASSIC_BT);
    }
    if (releaseErr == ESP_OK) {
        WP_LOG("BT release: controller memory released");
    } else {
        WP_LOGF("BT release: memory release returned %s; continuing",
                esp_err_to_name(releaseErr));
    }

    // Even when a teardown call reports an unexpected state, do not let later
    // code in this power cycle attempt to rebuild a stack the portal dismantled.
    btReleasedThisPowerCycle = true;
}

void WebPortalApp::update() {
    // Keep device awake
    millis_APP_LASTINTERACTION = millis_NOW;

    // Track STA connection state
    if (staSSID.length() && !staConnected) {
        if (WiFi.status() == WL_CONNECTED) {
            staConnected = true;
            WP_LOGF("STA connected, IP=%s", WiFi.localIP().toString().c_str());
            tryStartMDNS();
        } else if (millis() - staConnectStart > STA_TIMEOUT_MS) {
            WP_LOG("STA connect timeout");
            staSSID = "";
        }
    }
    if (staConnected && WiFi.status() != WL_CONNECTED) {
        staConnected = false;
        stopMDNS();
        WP_LOG("STA connection lost");
    }
    if (staConnected && !mdnsStarted &&
        millis() - lastMdnsAttempt >= MDNS_RETRY_MS) {
        tryStartMDNS();
    }

    // Live caption link: apply what the AsyncTCP task mailed over, then
    // move captured audio toward the client.
    processLiveMailbox();
    pumpLiveAudio();

    // Render OLED
    render();
}

// ---------------------------------------------------------------------------
// Button callback
// ---------------------------------------------------------------------------
void WebPortalApp::onButtonBack(const ButtonEvent& event) {
    if (event.eventType == ButtonEvent_Released && instance != nullptr) {
        if (instance->exitConfirmPending) {
            WP_LOG("exit confirmation cancelled");
            instance->exitConfirmPending = false;
        } else {
            WP_LOG("exit confirmation opened");
            instance->exitConfirmPending = true;
        }
    }
}

void WebPortalApp::onButtonEnter(const ButtonEvent& event) {
    if (event.eventType == ButtonEvent_Released && instance != nullptr &&
        instance->exitConfirmPending) {
        instance->confirmExitAndRestart();
    }
}

// Up toggles the caption text size while a live session shows captions —
// legibility first, this screen is an accessibility aid.
void WebPortalApp::onButtonUp(const ButtonEvent& event) {
    if (event.eventType == ButtonEvent_Pressed && instance != nullptr &&
        instance->liveClientId != 0) {
        instance->captionLargeFont = !instance->captionLargeFont;
    }
}

// ---------------------------------------------------------------------------
// SD helpers
// ---------------------------------------------------------------------------
void WebPortalApp::initSD() {
    SPI.begin(PIN_SD_CLK, PIN_SD_MISO, PIN_SD_MOSI, PIN_SD_CS);
    sdReady = SD.begin(PIN_SD_CS);
    if (!sdReady) {
        WP_LOG("SD init failed");
        return;
    }
    // Ensure directories exist
    if (!SD.exists(MEDIA_DIR)) SD.mkdir(MEDIA_DIR);
    if (!SD.exists(PLAYLIST_DIR)) SD.mkdir(PLAYLIST_DIR);
}

void WebPortalApp::countFilesRecursive(const char* dir) {
    File root = SD.open(dir);
    if (!root || !root.isDirectory()) return;
    File entry;
    while ((entry = root.openNextFile())) {
        if (entry.isDirectory()) {
            countFilesRecursive(entry.path());
        } else {
            String name = entry.name();
            String lower = name;
            lower.toLowerCase();
            if (lower.endsWith(".mp3")) fileCount++;
        }
        entry.close();
    }
    root.close();
}

void WebPortalApp::invalidateMusicCache() {
    if (SD.exists(CACHE_PATH)) {
        SD.remove(CACHE_PATH);
        WP_LOG("invalidated music cache");
    }
}

// ---------------------------------------------------------------------------
// WiFi STA helpers
// ---------------------------------------------------------------------------
void WebPortalApp::loadWifiCreds() {
    Preferences prefs;
    prefs.begin("wificfg", true);  // read-only
    staSSID = prefs.getString("ssid", "");
    String pass = prefs.getString("pass", "");
    prefs.end();
    if (staSSID.length()) {
        WP_LOGF("auto-connecting to: %s", staSSID.c_str());
        WiFi.begin(staSSID.c_str(), pass.c_str());
        staConnectStart = millis();
    }
}

void WebPortalApp::connectSTA(const String& ssid, const String& pass, bool save) {
    stopMDNS();
    WiFi.disconnect(false);  // disconnect STA only, keep AP
    delay(100);
    WiFi.begin(ssid.c_str(), pass.c_str());
    staSSID = ssid;
    staConnectStart = millis();
    staConnected = false;
    WP_LOGF("connecting to: %s", ssid.c_str());
    if (save) {
        Preferences prefs;
        prefs.begin("wificfg", false);
        prefs.putString("ssid", ssid);
        prefs.putString("pass", pass);
        prefs.end();
        WP_LOG("saved WiFi credentials");
    }
}

void WebPortalApp::disconnectSTA() {
    stopMDNS();
    WiFi.disconnect(false);
    staSSID = "";
    staConnected = false;
    Preferences prefs;
    prefs.begin("wificfg", false);
    prefs.clear();
    prefs.end();
    WP_LOG("WiFi credentials cleared");
}

void WebPortalApp::tryStartMDNS() {
    if (!staConnected || mdnsStarted) return;
    lastMdnsAttempt = millis();
    if (MDNS.begin("cyberfidget")) {
        MDNS.addService("http", "tcp", 80);
        mdnsStarted = true;
        WP_LOG("mDNS started (cyberfidget.local)");
    } else {
        MDNS.end();
        WP_LOG("mDNS start failed; will retry");
    }
}

void WebPortalApp::stopMDNS() {
    if (!mdnsStarted) return;
    MDNS.end();
    mdnsStarted = false;
}

void WebPortalApp::confirmExitAndRestart() {
    WP_LOG("exit confirmed; restarting");
    teardown();

    Preferences prefs;
    if (prefs.begin("bootcfg", false)) {
        size_t written = prefs.putBool("skipanim", true);
        prefs.end();
        if (written == 0) {
            WP_LOG("exit restart: failed to write animation skip flag");
        }
    } else {
        WP_LOG("exit restart: failed to open boot preferences");
    }

    Serial.flush();
    delay(50);
    esp_restart();
}

// ---------------------------------------------------------------------------
// Web server routes
// ---------------------------------------------------------------------------
void WebPortalApp::setupRoutes() {
    // Main portal page, gzipped in flash (~87 KB raw -> ~23 KB stored).
    //
    // sizeof() with no -1 here, unlike the old string-literal form: this is a
    // uint8_t[] of exactly the compressed length, with no NUL terminator to skip.
    //
    // Captive-portal note: the OS probe requests (hotspot-detect.html,
    // generate_204, ...) are answered by the redirect in onNotFound and are
    // unaffected by this. By the time anything fetches "/", it is the captive
    // sign-in browser -- a full WebKit/Chromium engine that advertises and
    // handles gzip. If a quirky client ever turns up that does not, the tell
    // would be a blank sign-in sheet, and the check is whether its request
    // carried Accept-Encoding: gzip.
    server->on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
        AsyncWebServerResponse* resp = req->beginResponse(
            200, "text/html", PORTAL_PAGE_GZ, sizeof(PORTAL_PAGE_GZ));
        resp->addHeader("Content-Encoding", "gzip");
        resp->addHeader("Cache-Control", "no-cache");
        req->send(resp);
    });

    // Serve media files for audio playback
    server->serveStatic("/media/", SD, "/media/");

    // Serve voice notes for in-browser playback + download
    server->serveStatic("/recordings/", SD, "/recordings/");

    // Phone companion (the SD "pack" built from portal-companion/): served
    // by the onNotFound catch-all below, NOT serveStatic. serveStatic's
    // content-type table doesn't know `.mjs` or `.wasm` (it served them as
    // text/plain, which strict browsers reject for ES modules / streaming
    // wasm) — the companion's transcription runtime is exactly those. The
    // catch-all sets the right type per extension and a no-cache header so
    // an updated pack isn't shadowed by a stale browser copy.

    // Live caption link: one WebSocket streams mic audio out and accepts
    // typed-JSON frames back (see LiveLinkProtocol.h for the contract).
    // The event handler runs on the AsyncTCP task — it only fills the
    // mailbox; update() does the real work in main-loop context.
    ws = new AsyncWebSocket("/ws/live");
    ws->onEvent([this](AsyncWebSocket*, AsyncWebSocketClient* client,
                       AwsEventType type, void* arg, uint8_t* data, size_t len) {
        onWsEvent(client, type, arg, data, len);
    });
    server->addHandler(ws);

    // API: File list (recursive JSON — for folder tree view)
    server->on("/api/files", HTTP_GET, [this](AsyncWebServerRequest* req) {
        handleFileList(req);
    });

    // API: Raw single-level directory listing (for the Files browser tab)
    server->on("/api/browse", HTTP_GET, [this](AsyncWebServerRequest* req) {
        handleBrowse(req);
    });

    // API: Download any file off the card as an attachment (Files browser + zip)
    server->on("/api/download", HTTP_GET, [this](AsyncWebServerRequest* req) {
        handleDownload(req);
    });

    // API: Track list with ID3 metadata (for table view)
    server->on("/api/tracks", HTTP_GET, [this](AsyncWebServerRequest* req) {
        handleTrackList(req);
    });

    // API: Upload
    server->on("/api/upload", HTTP_POST,
        // onRequest (after upload finishes)
        [this](AsyncWebServerRequest* req) {
            uploadInProgress = false;
            fileCount = 0;
            countFilesRecursive(MEDIA_DIR);
            invalidateMusicCache();
            req->send(200, "text/plain", "OK");
        },
        // onUpload (each chunk)
        [this](AsyncWebServerRequest* req, const String& filename,
               size_t index, uint8_t* data, size_t len, bool final) {
            handleUpload(req, filename, index, data, len, final);
        }
    );

    // API: Delete
    server->on("/api/delete", HTTP_POST, [this](AsyncWebServerRequest* req) {
        handleDelete(req);
    });

    // API: Mkdir
    server->on("/api/mkdir", HTTP_POST, [this](AsyncWebServerRequest* req) {
        handleMkdir(req);
    });

    // API: Move / rename
    server->on("/api/move", HTTP_POST, [this](AsyncWebServerRequest* req) {
        handleMove(req);
    });

    // API: Status
    server->on("/api/status", HTTP_GET, [this](AsyncWebServerRequest* req) {
        handleStatus(req);
    });

    // API: Voice notes list (merges /recordings/ files with index.csv metadata)
    server->on("/api/recordings", HTTP_GET, [this](AsyncWebServerRequest* req) {
        handleRecordings(req);
    });

    // API: Set device clock (browser sends its local-naive epoch on page load)
    server->on("/api/time", HTTP_POST, [this](AsyncWebServerRequest* req) {
        handleSetTime(req);
    });

    // API: List playlists
    server->on("/api/playlists", HTTP_GET, [this](AsyncWebServerRequest* req) {
        handlePlaylistList(req);
    });

    // API: Get/save playlist
    server->on("/api/playlist", HTTP_GET, [this](AsyncWebServerRequest* req) {
        handlePlaylistGet(req);
    });
    // Playlist save (POST with JSON body)
    server->on("/api/playlist", HTTP_POST,
        [](AsyncWebServerRequest* req) {
            // Handled in body callback below
        },
        nullptr, // no upload handler
        [this](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t index, size_t total) {
            handlePlaylistSave(req, data, len, index, total);
        }
    );

    // API: Delete playlist
    server->on("/api/playlist/delete", HTTP_POST, [this](AsyncWebServerRequest* req) {
        handlePlaylistDelete(req);
    });

    // API: WiFi scan
    server->on("/api/wifi/scan", HTTP_GET, [this](AsyncWebServerRequest* req) {
        handleWifiScan(req);
    });

    // API: WiFi connect (POST with JSON body)
    server->on("/api/wifi/connect", HTTP_POST,
        [](AsyncWebServerRequest* req) {
            // Handled in body callback
        },
        nullptr,
        [this](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t index, size_t total) {
            handleWifiConnect(req, data, len, index, total);
        }
    );

    // API: WiFi status
    server->on("/api/wifi/status", HTTP_GET, [this](AsyncWebServerRequest* req) {
        handleWifiStatus(req);
    });

    // API: WiFi forget
    server->on("/api/wifi/forget", HTTP_POST, [this](AsyncWebServerRequest* req) {
        handleWifiForget(req);
    });

    // Catch-all: serves the companion from SD (with correct content-types)
    // and is the captive-portal redirect for everything else.
    server->onNotFound([this](AsyncWebServerRequest* req) {
        const String& u = req->url();
        if (u == "/web") {
            req->redirect("/web/");
            return;
        }
        if (u.startsWith("/web/")) {
            // Map to an SD path; a directory request gets index.html.
            String path = u;
            if (path.endsWith("/")) path += "index.html";
            if (!wpPathSafe(path)) {
                req->send(404, "text/plain", "not found");
                return;
            }
            // The shell (index.html) changes between SD-pack builds — revalidate
            // so an update isn't shadowed by a stale copy. The vendor runtime
            // (transformers.js + the multi-MB ort wasm) is immutable and loaded
            // through the browser HTTP cache, so cache it hard — otherwise the
            // big wasm re-downloads every visit.
            const char* cache = path.endsWith(".html")
                ? "no-cache"
                : "public, max-age=31536000, immutable";

            if (sdReady) {
                // 1. Exact file on the card.
                File f = SD.open(path);
                bool isFile = f && !f.isDirectory();
                if (f) f.close();
                if (isFile) {
                    AsyncWebServerResponse* resp =
                        req->beginResponse(SD, path, webContentType(path));
                    resp->addHeader("Cache-Control", cache);
                    req->send(resp);
                    return;
                }
                // 2. Pre-compressed sibling. The SD pack ships the vendor tree
                //    and the worker gzipped (32 MB -> 7.8 MB on the card, and
                //    proportionally less WiFi transfer). Content-Type comes from
                //    the REQUESTED path, not the .gz name, or the browser
                //    refuses the module; Content-Length is the compressed
                //    length, which beginResponse takes from the file itself.
                //    A card written by an older pack has the plain files and is
                //    served by branch 1, so this stays backward compatible.
                String gzPath = path + ".gz";
                File g = SD.open(gzPath);
                bool gzIsFile = g && !g.isDirectory();
                if (g) g.close();
                if (gzIsFile) {
                    AsyncWebServerResponse* resp =
                        req->beginResponse(SD, gzPath, webContentType(path));
                    resp->addHeader("Content-Encoding", "gzip");
                    resp->addHeader("Cache-Control", cache);
                    req->send(resp);
                    return;
                }
            }

            // 3. The shell is embedded in the firmware image, gzipped, so live
            //    listening works with an empty or absent memory card. A copy on
            //    the card still wins (branch 1) so a newer pack can be dropped
            //    in without reflashing.
            if (path == "/web/index.html") {
                AsyncWebServerResponse* resp = req->beginResponse(
                    200, "text/html", COMPANION_SHELL_GZ, sizeof(COMPANION_SHELL_GZ));
                resp->addHeader("Content-Encoding", "gzip");
                resp->addHeader("Cache-Control", "no-cache");
                req->send(resp);
                return;
            }

            // 4. A genuinely missing sub-resource. This must NOT be the
            //    fallback page: returning 200 + text/html for a missing module
            //    or wasm makes the browser reject it on MIME grounds and hides
            //    the real cause. The shell always loads now, so a
            //    miss here means the captions payload is not on the card, which
            //    the companion reports in-app.
            req->send(404, "text/plain", "not found");
            return;
        }
        req->redirect("http://192.168.4.1/");
    });
}

// ---------------------------------------------------------------------------
// Route: File list (recursive JSON tree)
// ---------------------------------------------------------------------------
static void buildFileListJSON(const char* dir, String& json, const char* rootPrefix) {
    File root = SD.open(dir);
    if (!root || !root.isDirectory()) return;

    bool first = true;
    File entry;
    while ((entry = root.openNextFile())) {
        String fullPath = entry.path();
        String entryName = fullPath;
        int lastSlash = fullPath.lastIndexOf('/');
        if (lastSlash >= 0) entryName = fullPath.substring(lastSlash + 1);

        // Skip hidden files and playlist directory in main listing
        if (entryName.startsWith(".")) { entry.close(); continue; }
        if (fullPath == String(PLAYLIST_DIR)) { entry.close(); continue; }

        // Skip non-mp3 files (e.g. idx.txt, .m3u, etc.)
        if (!entry.isDirectory()) {
            String lower = entryName;
            lower.toLowerCase();
            if (!lower.endsWith(".mp3")) { entry.close(); continue; }
        }

        if (!first) json += ",";
        first = false;

        if (entry.isDirectory()) {
            json += "{\"name\":\"" + entryName + "\",\"type\":\"dir\",\"children\":[";
            buildFileListJSON(fullPath.c_str(), json, rootPrefix);
            json += "]}";
        } else {
            json += "{\"name\":\"" + entryName + "\",\"type\":\"file\",\"size\":" + String(entry.size()) + "}";
        }
        entry.close();
    }
    root.close();
}

void WebPortalApp::handleFileList(AsyncWebServerRequest* req) {
    if (!sdReady) {
        req->send(500, "application/json", "{\"error\":\"SD not available\"}");
        return;
    }
    String json = "[";
    buildFileListJSON(MEDIA_DIR, json, MEDIA_DIR);
    json += "]";
    req->send(200, "application/json", json);
}

// ---------------------------------------------------------------------------
// Route: Raw single-level directory listing (Files browser tab)
//
// Unlike /api/files (recursive, mp3-filtered, music-tab coupled), this lists the
// *direct* children of one folder with no type filter — the Explorer/Finder model
// where you navigate into a folder at a time. Defaults to "/" (card root). Each
// entry carries name, type, size, and mtime (File::getLastWrite()). mtime is only
// meaningful once the device clock is set; files written before then read as 0,
// which the UI renders as "No date". Object response ({"sd":false} vs
// {"sd":true,...}) lets the client tell "no card" from "empty folder".
// ---------------------------------------------------------------------------
void WebPortalApp::handleBrowse(AsyncWebServerRequest* req) {
    if (!sdReady) {
        req->send(200, "application/json", "{\"sd\":false}");
        return;
    }
    String path = req->hasParam("path") ? req->getParam("path")->value() : String("/");
    // Normalize a trailing slash (but keep the bare root "/").
    while (path.length() > 1 && path.endsWith("/")) path.remove(path.length() - 1);
    if (!wpPathSafe(path)) {
        req->send(400, "application/json", "{\"error\":\"bad path\"}");
        return;
    }
    File dir = SD.open(path);
    if (!dir || !dir.isDirectory()) {
        if (dir) dir.close();
        req->send(404, "application/json", "{\"error\":\"not a folder\"}");
        return;
    }

    String json = "{\"sd\":true,\"path\":\"" + escJSON(path) + "\",\"entries\":[";
    bool first = true;
    File entry;
    while ((entry = dir.openNextFile())) {
        String full = entry.path();
        String name = full.substring(full.lastIndexOf('/') + 1);
        bool isDir = entry.isDirectory();
        uint32_t mtime = (uint32_t)entry.getLastWrite();
        uint64_t size = isDir ? 0 : (uint64_t)entry.size();
        entry.close();

        if (!first) json += ",";
        first = false;
        json += "{\"name\":\"" + escJSON(name) + "\",\"type\":\"";
        json += isDir ? "dir" : "file";
        json += "\",\"size\":" + String((unsigned long)size);
        json += ",\"mtime\":" + String((unsigned long)mtime) + "}";
    }
    dir.close();
    json += "]}";
    req->send(200, "application/json", json);
}

// ---------------------------------------------------------------------------
// Route: Download any file off the card as an attachment
//
// /media/ and /recordings/ serveStatic mounts serve files inline with a media
// content type (so they play in the browser). The Files browser needs to pull
// *any* file regardless of location/type, and the client-side zip builder fetches
// each selected file's bytes through here. Sending with download=true sets a
// Content-Disposition attachment so the browser saves with the right name (this
// also stops iOS Safari mis-naming blob downloads).
// ---------------------------------------------------------------------------
void WebPortalApp::handleDownload(AsyncWebServerRequest* req) {
    if (!sdReady) {
        req->send(503, "text/plain", "No SD card");
        return;
    }
    if (!req->hasParam("path")) {
        req->send(400, "text/plain", "Missing 'path'");
        return;
    }
    String path = req->getParam("path")->value();
    if (!wpPathSafe(path)) {
        req->send(400, "text/plain", "Bad path");
        return;
    }
    if (!SD.exists(path)) {
        req->send(404, "text/plain", "Not found");
        return;
    }
    File f = SD.open(path);
    bool isDir = f && f.isDirectory();
    if (f) f.close();
    if (isDir) {
        req->send(400, "text/plain", "Is a folder");
        return;
    }
    req->send(SD, path, "application/octet-stream", true);
}

// ---------------------------------------------------------------------------
// Route: Track list with ID3 metadata
// ---------------------------------------------------------------------------
void WebPortalApp::handleTrackList(AsyncWebServerRequest* req) {
    if (!sdReady) {
        req->send(500, "application/json", "[]");
        return;
    }
    String json = "[";
    bool first = true;
    collectMP3Paths(MEDIA_DIR, json, first);
    json += "]";
    WP_LOGF("tracks API: %u bytes JSON", (unsigned)json.length());
    req->send(200, "application/json", json);
}

// ---------------------------------------------------------------------------
// Route: Upload
// ---------------------------------------------------------------------------
void WebPortalApp::handleUpload(AsyncWebServerRequest* req, const String& filename,
                                 size_t index, uint8_t* data, size_t len, bool final) {
    if (index == 0) {
        WP_LOGF("upload start: %s", filename.c_str());
        // Determine target directory from query param or default to /media/.
        // The Files browser uploads into the current folder (any safe card path).
        String dir = MEDIA_DIR;
        if (req->hasParam("dir")) {
            dir = req->getParam("dir")->value();
        }
        while (dir.length() > 1 && dir.endsWith("/")) dir.remove(dir.length() - 1);
        if (!wpPathSafe(dir) || filename.indexOf('/') >= 0 || filename.indexOf("..") >= 0) {
            req->send(400, "text/plain", "Bad path");
            return;
        }
        String path = (dir == "/" ? String("") : dir) + "/" + filename;
        uploadFile = SD.open(path, FILE_WRITE);
        if (!uploadFile) {
            WP_LOG("upload: failed to open file");
            req->send(500, "text/plain", "Failed to open file");
            return;
        }
        uploadBytesTotal = req->contentLength();
        uploadBytesReceived = 0;
        uploadInProgress = true;
    }

    if (uploadFile && len > 0) {
        size_t written = uploadFile.write(data, len);
        uploadBytesReceived += written;
        if (written < len) {
            WP_LOGF("upload: write error, %u of %u", (unsigned)written, (unsigned)len);
            uploadFile.close();
            uploadInProgress = false;
            req->send(507, "text/plain", "SD card full");
            return;
        }
    }

    if (final) {
        WP_LOGF("upload complete: %s (%u bytes)", filename.c_str(), (unsigned)uploadBytesReceived);
        if (uploadFile) uploadFile.close();
        uploadInProgress = false;
    }
}

// ---------------------------------------------------------------------------
// Route: Delete
// ---------------------------------------------------------------------------
void WebPortalApp::handleDelete(AsyncWebServerRequest* req) {
    if (!req->hasParam("path")) {
        req->send(400, "text/plain", "Missing 'path'");
        return;
    }
    String path = req->getParam("path")->value();
    while (path.length() > 1 && path.endsWith("/")) path.remove(path.length() - 1);

    // The Files browser can delete anywhere on the card; guard only against a
    // malformed path or a delete of the whole card root.
    if (!wpPathSafe(path) || path == "/") {
        req->send(403, "text/plain", "Forbidden");
        return;
    }

    if (!SD.exists(path)) {
        req->send(404, "text/plain", "Not found");
        return;
    }

    // Directory: Explorer-style recursive delete (folder and everything inside).
    File node = SD.open(path);
    bool isDir = node && node.isDirectory();
    if (node) node.close();
    if (isDir) {
        if (!wpRmRecursive(path)) {
            req->send(500, "text/plain", "Delete failed");
            return;
        }
        WP_LOGF("removed dir tree: %s", path.c_str());
        if (path.startsWith(MEDIA_DIR)) {
            fileCount = 0;
            countFilesRecursive(MEDIA_DIR);
            invalidateMusicCache();
        }
        req->send(200, "text/plain", "OK");
        return;
    }

    // --- Voice note: drop the .wav, its index.csv row, and any transcript
    // sidecar together so the metadata never outlives the file. A voice note
    // is always a single file (no subdirs under /recordings/).
    if (path.startsWith("/recordings/")) {
        if (!SD.remove(path)) {
            req->send(500, "text/plain", "Delete failed");
            return;
        }
        String name = path.substring(path.lastIndexOf('/') + 1);
        deleteRecordingIndexRow(name.c_str());
        // Transcript sidecar (provisional ".txt"; on-device transcription is
        // not built yet — same provisional handling as VoiceRecorderApp).
        int dot = name.lastIndexOf('.');
        String sidecar = String(RECORDINGS_DIR) + "/" +
                         (dot > 0 ? name.substring(0, dot) : name) + ".txt";
        if (SD.exists(sidecar)) SD.remove(sidecar);
        WP_LOGF("deleted voice note: %s", path.c_str());
        req->send(200, "text/plain", "OK");
        return;
    }

    // Plain file anywhere else on the card.
    if (SD.remove(path)) {
        WP_LOGF("deleted: %s", path.c_str());
        if (path.startsWith(MEDIA_DIR)) {
            fileCount = 0;
            countFilesRecursive(MEDIA_DIR);
            invalidateMusicCache();
        }
        req->send(200, "text/plain", "OK");
    } else {
        req->send(500, "text/plain", "Delete failed");
    }
}

// ---------------------------------------------------------------------------
// Route: Mkdir
// ---------------------------------------------------------------------------
void WebPortalApp::handleMkdir(AsyncWebServerRequest* req) {
    if (!req->hasParam("path")) {
        req->send(400, "text/plain", "Missing 'path'");
        return;
    }
    String path = req->getParam("path")->value();
    while (path.length() > 1 && path.endsWith("/")) path.remove(path.length() - 1);

    // The Files browser can make a folder anywhere on the card; guard only
    // against a malformed path or the bare root.
    if (!wpPathSafe(path) || path == "/") {
        req->send(403, "text/plain", "Forbidden");
        return;
    }

    if (SD.mkdir(path)) {
        WP_LOGF("created dir: %s", path.c_str());
        req->send(200, "text/plain", "OK");
    } else {
        req->send(500, "text/plain", "Mkdir failed");
    }
}

// ---------------------------------------------------------------------------
// Route: Move / rename
// ---------------------------------------------------------------------------
void WebPortalApp::handleMove(AsyncWebServerRequest* req) {
    if (!req->hasParam("from") || !req->hasParam("to")) {
        req->send(400, "text/plain", "Missing 'from' and 'to'");
        return;
    }
    String from = req->getParam("from")->value();
    String to = req->getParam("to")->value();

    // Music (/media/) and voice notes (/recordings/) each have curated rules
    // below; the Files browser can rename/move anything else on the card. A move
    // must still stay within one of those roots (so the two curated stores never
    // bleed into each other) OR be a generic in-card move outside both.
    bool mediaMove = from.startsWith("/media/") && to.startsWith("/media/");
    bool recMove   = from.startsWith("/recordings/") && to.startsWith("/recordings/");
    bool crossCurated = (from.startsWith("/media/") || from.startsWith("/recordings/") ||
                         to.startsWith("/media/")   || to.startsWith("/recordings/")) &&
                        !mediaMove && !recMove;
    if (crossCurated) {
        // e.g. dragging a recording into /media — refused to keep the stores clean.
        req->send(403, "text/plain", "Forbidden");
        return;
    }
    if (!mediaMove && !recMove) {
        // Generic Files-browser move/rename: just needs safe in-card paths.
        if (!wpPathSafe(from) || !wpPathSafe(to) || from == "/" || to == "/") {
            req->send(403, "text/plain", "Forbidden");
            return;
        }
    }

    // A voice-note rename stays a flat .wav in /recordings/ (no subfolders, keep
    // the playable extension) so the file and its index row remain in lockstep.
    if (recMove) {
        String toName = to.substring(strlen("/recordings/"));
        if (toName.indexOf('/') >= 0 || !to.endsWith(".wav")) {
            req->send(400, "text/plain", "Voice notes stay as flat .wav files");
            return;
        }
        if (toName.length() > REC_NAME_MAX) {
            req->send(400, "text/plain", "Name too long");
            return;
        }
    }

    if (!SD.exists(from)) {
        req->send(404, "text/plain", "Source not found");
        return;
    }
    if (SD.exists(to)) {
        req->send(409, "text/plain", "Target already exists");
        return;
    }

    if (!SD.rename(from, to)) {
        req->send(500, "text/plain", "Move failed");
        return;
    }
    WP_LOGF("moved: %s -> %s", from.c_str(), to.c_str());

    if (recMove) {
        String fromName = from.substring(from.lastIndexOf('/') + 1);
        String toName   = to.substring(to.lastIndexOf('/') + 1);
        renameRecordingIndexRow(fromName.c_str(), toName.c_str());
        // Carry a transcript sidecar along, if one exists.
        int fd = fromName.lastIndexOf('.'), td = toName.lastIndexOf('.');
        String fromSide = String(RECORDINGS_DIR) + "/" +
                          (fd > 0 ? fromName.substring(0, fd) : fromName) + ".txt";
        String toSide = String(RECORDINGS_DIR) + "/" +
                        (td > 0 ? toName.substring(0, td) : toName) + ".txt";
        if (SD.exists(fromSide) && !SD.exists(toSide)) SD.rename(fromSide, toSide);
    } else if (mediaMove) {
        invalidateMusicCache();
    }
    req->send(200, "text/plain", "OK");
}

// ---------------------------------------------------------------------------
// Route: Status
// ---------------------------------------------------------------------------
void WebPortalApp::handleStatus(AsyncWebServerRequest* req) {
    // Card-less guard: f_getfree on an unmounted card spins in the SD-SPI
    // retry loop on the async_tcp task until the task watchdog reboots the
    // device (seen on hardware). Never touch SD here unless the mount worked.
    uint64_t totalBytes = sdReady ? SD.totalBytes() : 0;
    uint64_t usedBytes = sdReady ? SD.usedBytes() : 0;

    String json = "{";
    json += "\"files\":" + String(fileCount);
    json += ",\"totalBytes\":" + String((unsigned long)totalBytes);
    json += ",\"usedBytes\":" + String((unsigned long)usedBytes);
    json += ",\"clients\":" + String(WiFi.softAPgetStationNum());
    // Firmware version. Both device documents show it in the shared kit's
    // sidebar footer, so "what is this thing running" is answerable without a
    // serial cable; it is also what the companion's Your data segment reports.
    json += ",\"version\":\"" + String(getFirmwareVersionString()) + "\"";
    // Live caption link health: dropped counts make the contract's
    // backpressure behavior observable from a test or a curious user.
    json += ",\"live\":{\"connected\":" + String(liveClientId != 0 ? "true" : "false");
    json += ",\"sentFrames\":" + String((unsigned long)liveSentFrames);
    json += ",\"droppedFrames\":" + String((unsigned long)liveDroppedFrames) + "}";
    // Internal-heap health (the scarce resource under AP+STA + a live
    // stream): lets a soak test watch for leaks without a serial cable.
    json += ",\"heap\":{\"free\":" +
            String((unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    json += ",\"largest\":" +
            String((unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)) + "}";
    // Is the captions payload on the card? The shell itself is in flash, so this
    // is the only remaining card dependency, and it is the one thing the UI has
    // to be able to explain. It cannot be probed over HTTP: a missing path under
    // /web/ is a 404 with no way to distinguish "no card" from "no pack", and a
    // 200 would be indistinguishable from success. One bounded exists() lookup,
    // behind the same sdReady guard as the sizes above.
    bool captions = sdReady && SD.exists("/web/vendor");
    json += ",\"captions\":" + String(captions ? "true" : "false");
    json += "}";
    req->send(200, "application/json", json);
}

// ---------------------------------------------------------------------------
// Route: Voice notes list
//
// Drives off /recordings/index.csv (the recorder's source of truth, written by
// VoiceRecorderApp::appendIndexRow) and keeps only rows whose .wav still lives
// on the card. Each row's metadata is parsed by the shared RecNaming helper, so
// the portal and the on-device list agree byte-for-byte. The response is an
// object (not a bare array) so the client can tell "no card" apart from "no
// notes yet": {"sd":false} vs {"sd":true,"items":[...]}.
// ---------------------------------------------------------------------------
void WebPortalApp::handleRecordings(AsyncWebServerRequest* req) {
    if (!sdReady) {
        req->send(200, "application/json", "{\"sd\":false}");
        return;
    }

    char indexPath[40];
    snprintf(indexPath, sizeof(indexPath), "%s/index.csv", RECORDINGS_DIR);

    String json = "{\"sd\":true,\"items\":[";
    bool first = true;
    File f = SD.open(indexPath, FILE_READ);
    if (f) {
        char line[128];
        // name is generous: the recorder writes "REC_NNNN.wav" (12 chars), but a
        // portal rename can grow it up to REC_NAME_MAX, and parseIndexRow drops
        // any row whose name overflows nameSize — too small a buffer would make a
        // renamed note silently vanish from the list.
        char name[REC_NAME_MAX + 1], ts[24];
        uint32_t durationS;
        uint64_t bytes;
        while (wpReadLine(f, line, sizeof(line))) {
            if (!RecNaming::parseIndexRow(line, name, sizeof(name),
                                          ts, sizeof(ts), &durationS, &bytes)) {
                continue;  // header row, blank line, or a torn partial row
            }
            // Skip rows whose file is gone (deleted out-of-band) so the portal
            // never lists a note you can't play.
            char wavPath[16 + REC_NAME_MAX + 1];
            snprintf(wavPath, sizeof(wavPath), "%s/%s", RECORDINGS_DIR, name);
            if (!SD.exists(wavPath)) continue;

            if (!first) json += ",";
            first = false;
            json += "{\"name\":\"" + escJSON(name) + "\"";
            json += ",\"timestamp\":\"" + escJSON(ts) + "\"";
            json += ",\"duration\":" + String((unsigned long)durationS);
            json += ",\"bytes\":" + String((unsigned long)bytes) + "}";
        }
        f.close();
    }
    json += "]}";
    req->send(200, "application/json", json);
}

// ---------------------------------------------------------------------------
// Route: Set device clock
//
// The device has no battery-backed RTC, so recordings carry blank timestamps
// until something sets the clock. The portal page POSTs the browser's clock on
// load (?ms=<epoch>); settimeofday makes time(nullptr) report it, and any
// recording made afterwards lands a real timestamp in index.csv. The browser
// sends a tz-adjusted (local-naive) epoch so the recorder's localtime_r — which
// reads as UTC with TZ unset — renders the user's local wall-clock time.
// ---------------------------------------------------------------------------
void WebPortalApp::handleSetTime(AsyncWebServerRequest* req) {
    if (!req->hasParam("ms")) {
        req->send(400, "text/plain", "Missing 'ms'");
        return;
    }
    uint64_t ms = strtoull(req->getParam("ms")->value().c_str(), nullptr, 10);
    // Reject an obviously-unset clock (before 2020) so a confused client can't
    // wind the device back to 1970.
    if (ms < 1600000000000ULL) {
        req->send(400, "text/plain", "Bad time");
        return;
    }
    struct timeval tv;
    tv.tv_sec  = (time_t)(ms / 1000ULL);
    tv.tv_usec = (suseconds_t)((ms % 1000ULL) * 1000ULL);
    settimeofday(&tv, nullptr);
    WP_LOGF("clock set: epoch %lu", (unsigned long)tv.tv_sec);
    req->send(200, "text/plain", "OK");
}

// ---------------------------------------------------------------------------
// Voice-note index.csv maintenance
//
// Both rewrite index.csv via an index.tmp swap (crash-safe): the header and
// every non-matching row pass through verbatim. Matching is exact and
// prefix-collision safe via RecNaming::indexRowMatchesFilename.
// ---------------------------------------------------------------------------
bool WebPortalApp::deleteRecordingIndexRow(const char* filename) {
    char indexPath[40], tmpPath[44];
    snprintf(indexPath, sizeof(indexPath), "%s/index.csv", RECORDINGS_DIR);
    snprintf(tmpPath, sizeof(tmpPath), "%s/index.tmp", RECORDINGS_DIR);
    if (!SD.exists(indexPath)) return false;

    File in = SD.open(indexPath, FILE_READ);
    if (!in) return false;
    if (SD.exists(tmpPath)) SD.remove(tmpPath);
    File out = SD.open(tmpPath, FILE_WRITE);
    if (!out) { in.close(); return false; }

    char line[128];
    while (wpReadLine(in, line, sizeof(line))) {
        if (RecNaming::indexRowMatchesFilename(line, filename)) continue;  // drop
        out.print(line);
        out.write('\n');
    }
    in.close();
    out.close();

    SD.remove(indexPath);
    return SD.rename(tmpPath, indexPath);
}

bool WebPortalApp::renameRecordingIndexRow(const char* oldName, const char* newName) {
    char indexPath[40], tmpPath[44];
    snprintf(indexPath, sizeof(indexPath), "%s/index.csv", RECORDINGS_DIR);
    snprintf(tmpPath, sizeof(tmpPath), "%s/index.tmp", RECORDINGS_DIR);
    if (!SD.exists(indexPath)) return false;

    File in = SD.open(indexPath, FILE_READ);
    if (!in) return false;
    if (SD.exists(tmpPath)) SD.remove(tmpPath);
    File out = SD.open(tmpPath, FILE_WRITE);
    if (!out) { in.close(); return false; }

    char line[128];
    while (wpReadLine(in, line, sizeof(line))) {
        if (RecNaming::indexRowMatchesFilename(line, oldName)) {
            // Swap the filename field; keep timestamp,duration,bytes verbatim.
            const char* comma = strchr(line, ',');
            out.print(newName);
            out.print(comma ? comma : ",,0,0");
        } else {
            out.print(line);
        }
        out.write('\n');
    }
    in.close();
    out.close();

    SD.remove(indexPath);
    return SD.rename(tmpPath, indexPath);
}

// ---------------------------------------------------------------------------
// Route: Playlist list
// ---------------------------------------------------------------------------
void WebPortalApp::handlePlaylistList(AsyncWebServerRequest* req) {
    if (!sdReady || !SD.exists(PLAYLIST_DIR)) {
        req->send(200, "application/json", "[]");
        return;
    }

    File dir = SD.open(PLAYLIST_DIR);
    if (!dir || !dir.isDirectory()) {
        req->send(200, "application/json", "[]");
        return;
    }

    String json = "[";
    bool first = true;
    File entry;
    while ((entry = dir.openNextFile())) {
        if (!entry.isDirectory()) {
            String name = entry.name();
            String lower = name;
            lower.toLowerCase();
            if (lower.endsWith(".m3u")) {
                if (!first) json += ",";
                first = false;

                // Count tracks in the M3U file
                int trackCount = 0;
                while (entry.available()) {
                    String line = entry.readStringUntil('\n');
                    line.trim();
                    if (line.length() && !line.startsWith("#")) trackCount++;
                }

                // Get display name (strip extension)
                String displayName = name;
                int lastSlash = displayName.lastIndexOf('/');
                if (lastSlash >= 0) displayName = displayName.substring(lastSlash + 1);
                int dot = displayName.lastIndexOf('.');
                if (dot >= 0) displayName = displayName.substring(0, dot);

                json += "{\"name\":\"" + displayName + "\",\"tracks\":" + String(trackCount) + "}";
            }
        }
        entry.close();
    }
    dir.close();
    json += "]";
    req->send(200, "application/json", json);
}

// ---------------------------------------------------------------------------
// Route: Get playlist
// ---------------------------------------------------------------------------
void WebPortalApp::handlePlaylistGet(AsyncWebServerRequest* req) {
    if (!req->hasParam("name")) {
        req->send(400, "text/plain", "Missing 'name'");
        return;
    }
    String name = req->getParam("name")->value();
    String path = String(PLAYLIST_DIR) + "/" + name + ".m3u";

    File f = SD.open(path, FILE_READ);
    if (!f) {
        req->send(404, "application/json", "{\"name\":\"" + name + "\",\"tracks\":[]}");
        return;
    }

    String json = "{\"name\":\"" + name + "\",\"tracks\":[";
    bool first = true;
    while (f.available()) {
        String line = f.readStringUntil('\n');
        line.trim();
        if (line.length() && !line.startsWith("#")) {
            if (!first) json += ",";
            first = false;
            json += "\"" + line + "\"";
        }
    }
    f.close();
    json += "]}";
    req->send(200, "application/json", json);
}

// ---------------------------------------------------------------------------
// Route: Save playlist (POST body = JSON)
// ---------------------------------------------------------------------------
void WebPortalApp::handlePlaylistSave(AsyncWebServerRequest* req, uint8_t* data,
                                       size_t len, size_t index, size_t total) {
    // Accumulate body (small JSON, fits in one or two chunks)
    static String body;
    if (index == 0) body = "";
    body += String((char*)data, len);

    if (index + len >= total) {
        // Body complete — parse and save
        if (!req->hasParam("name")) {
            req->send(400, "text/plain", "Missing 'name'");
            return;
        }
        String name = req->getParam("name")->value();
        String path = String(PLAYLIST_DIR) + "/" + name + ".m3u";

        // Simple JSON parse: extract track paths from {"tracks":["/media/...","/media/..."]}
        File f = SD.open(path, FILE_WRITE);
        if (!f) {
            req->send(500, "text/plain", "Failed to write playlist");
            return;
        }

        f.println("#EXTM3U");

        // Extract tracks array from JSON
        int arrStart = body.indexOf('[');
        int arrEnd = body.lastIndexOf(']');
        if (arrStart >= 0 && arrEnd > arrStart) {
            String arr = body.substring(arrStart + 1, arrEnd);
            // Parse each quoted string
            int pos = 0;
            while (pos < (int)arr.length()) {
                int qStart = arr.indexOf('"', pos);
                if (qStart < 0) break;
                int qEnd = arr.indexOf('"', qStart + 1);
                if (qEnd < 0) break;
                String track = arr.substring(qStart + 1, qEnd);
                f.println(track);
                pos = qEnd + 1;
            }
        }

        f.close();
        WP_LOGF("saved playlist: %s", path.c_str());
        req->send(200, "text/plain", "OK");
    }
}

// ---------------------------------------------------------------------------
// Route: Delete playlist
// ---------------------------------------------------------------------------
void WebPortalApp::handlePlaylistDelete(AsyncWebServerRequest* req) {
    if (!req->hasParam("name")) {
        req->send(400, "text/plain", "Missing 'name'");
        return;
    }
    String name = req->getParam("name")->value();
    String path = String(PLAYLIST_DIR) + "/" + name + ".m3u";

    if (SD.exists(path) && SD.remove(path)) {
        WP_LOGF("deleted playlist: %s", path.c_str());
        req->send(200, "text/plain", "OK");
    } else {
        req->send(404, "text/plain", "Playlist not found");
    }
}

// ---------------------------------------------------------------------------
// WiFi route handlers
// ---------------------------------------------------------------------------
void WebPortalApp::handleWifiScan(AsyncWebServerRequest* req) {
    int result = WiFi.scanComplete();

    if (result == WIFI_SCAN_FAILED) {
        // No scan in progress — start async scan
        WP_LOG("WiFi scan starting (async)");
        WiFi.scanNetworks(true);  // true = async, non-blocking
        req->send(200, "application/json", "{\"status\":\"scanning\"}");
        return;
    }

    if (result == WIFI_SCAN_RUNNING) {
        // Still scanning — tell client to poll again
        req->send(200, "application/json", "{\"status\":\"scanning\"}");
        return;
    }

    // result >= 0: scan complete
    WP_LOGF("WiFi scan found %d networks", result);
    String json = "[";
    bool first = true;
    // Deduplicate by SSID (keep strongest signal — scan results sorted by RSSI)
    for (int i = 0; i < result; i++) {
        String ssid = WiFi.SSID(i);
        if (!ssid.length()) continue;

        // Check for duplicate SSID already emitted
        bool dup = false;
        for (int j = 0; j < i; j++) {
            if (WiFi.SSID(j) == ssid) { dup = true; break; }
        }
        if (dup) continue;

        if (!first) json += ",";
        first = false;
        json += "{\"ssid\":\"" + escJSON(ssid) + "\"";
        json += ",\"rssi\":" + String(WiFi.RSSI(i));
        json += ",\"secure\":" + String(WiFi.encryptionType(i) != WIFI_AUTH_OPEN ? "true" : "false");
        json += "}";
    }
    WiFi.scanDelete();
    json += "]";
    req->send(200, "application/json", json);
}

void WebPortalApp::handleWifiConnect(AsyncWebServerRequest* req, uint8_t* data,
                                      size_t len, size_t index, size_t total) {
    static String body;
    if (index == 0) body = "";
    body += String((char*)data, len);

    if (index + len >= total) {
        // Parse JSON: {"ssid":"...","pass":"..."}
        String ssid, pass;
        int ssidStart = body.indexOf("\"ssid\"");
        if (ssidStart >= 0) {
            int valStart = body.indexOf('"', body.indexOf(':', ssidStart) + 1);
            int valEnd = body.indexOf('"', valStart + 1);
            if (valStart >= 0 && valEnd > valStart) ssid = body.substring(valStart + 1, valEnd);
        }
        int passStart = body.indexOf("\"pass\"");
        if (passStart >= 0) {
            int valStart = body.indexOf('"', body.indexOf(':', passStart) + 1);
            int valEnd = body.indexOf('"', valStart + 1);
            if (valStart >= 0 && valEnd > valStart) pass = body.substring(valStart + 1, valEnd);
        }

        if (!ssid.length()) {
            req->send(400, "application/json", "{\"error\":\"Missing ssid\"}");
            return;
        }

        connectSTA(ssid, pass, true);
        req->send(200, "application/json", "{\"status\":\"connecting\"}");
    }
}

void WebPortalApp::handleWifiStatus(AsyncWebServerRequest* req) {
    String json = "{";
    json += "\"connected\":" + String(staConnected ? "true" : "false");
    if (staConnected) {
        json += ",\"ssid\":\"" + escJSON(staSSID) + "\"";
        json += ",\"ip\":\"" + WiFi.localIP().toString() + "\"";
        if (mdnsStarted) {
            json += ",\"mdns\":\"cyberfidget.local\"";
        }
    } else if (staSSID.length()) {
        json += ",\"ssid\":\"" + escJSON(staSSID) + "\"";
        json += ",\"status\":\"connecting\"";
    }
    json += ",\"ap_ip\":\"" + WiFi.softAPIP().toString() + "\"";
    json += "}";
    req->send(200, "application/json", json);
}

void WebPortalApp::handleWifiForget(AsyncWebServerRequest* req) {
    disconnectSTA();
    req->send(200, "application/json", "{\"status\":\"ok\"}");
}

// ---------------------------------------------------------------------------
// Live caption link (/ws/live)
// ---------------------------------------------------------------------------

// AsyncTCP-task context: claim/release the single-client slot and fill the
// mailbox; never touch SD, the display, or MicCapture from here.
void WebPortalApp::onWsEvent(AsyncWebSocketClient* client, AwsEventType type,
                             void* arg, uint8_t* data, size_t len) {
    switch (type) {
        case WS_EVT_CONNECT: {
            bool busy;
            portENTER_CRITICAL(&liveMux);
            busy = (wsClaimedId != 0);
            if (!busy) {
                wsClaimedId = client->id();
                rxConnectId = client->id();
            }
            portEXIT_CRITICAL(&liveMux);
            if (busy) {
                // Single client enforced: tell the second one why, then close.
                char err[80];
                if (LiveLink::buildError(err, sizeof(err),
                                         "busy: another listener is connected")) {
                    client->text(err);
                }
                client->close();
                return;
            }
            // Don't let a saturated queue kill the connection — the audio
            // pump checks queue depth itself and drops frames instead.
            client->setCloseClientOnQueueFull(false);
            break;
        }

        case WS_EVT_DISCONNECT: {
            portENTER_CRITICAL(&liveMux);
            if (client->id() == wsClaimedId) {
                wsClaimedId = 0;
                rxDisconnect = true;
            }
            portEXIT_CRITICAL(&liveMux);
            break;
        }

        case WS_EVT_DATA: {
            AwsFrameInfo* info = (AwsFrameInfo*)arg;
            // The contract's inbound frames are small single-fragment text;
            // anything else (binary, fragmented, oversized) is dropped.
            if (!info->final || info->index != 0 || info->opcode != WS_TEXT) break;
            char buf[LiveLink::kMaxCaptionText * 2 + 96];
            if (len >= sizeof(buf)) break;
            memcpy(buf, data, len);
            buf[len] = '\0';
            LiveLink::Msg msg;
            if (!LiveLink::parseMessage(buf, msg)) break;

            portENTER_CRITICAL(&liveMux);
            if (client->id() == wsClaimedId) {
                if (msg.type == LiveLink::MsgType::Time) {
                    rxEpochMs = msg.epochMs;
                    rxTimeNew = true;
                } else if (msg.type == LiveLink::MsgType::Caption &&
                           liveBufs != nullptr) {
                    if (msg.isFinal) {
                        if (rxFinalCount < RX_FINAL_QUEUE) {
                            strncpy(liveBufs->rxFinals[rxFinalCount], msg.text,
                                    LiveLink::kMaxCaptionText);
                            liveBufs->rxFinals[rxFinalCount]
                                               [LiveLink::kMaxCaptionText] = '\0';
                            rxFinalCount++;
                        }
                        // A final supersedes the in-flight partial.
                        liveBufs->rxPartial[0] = '\0';
                        rxPartialNew = true;
                    } else {
                        strncpy(liveBufs->rxPartial, msg.text,
                                LiveLink::kMaxCaptionText);
                        liveBufs->rxPartial[LiveLink::kMaxCaptionText] = '\0';
                        rxPartialNew = true;
                    }
                }
            }
            portEXIT_CRITICAL(&liveMux);
            break;
        }

        default:
            break;
    }
}

// Main loop: drain the mailbox and act on it. Disconnect is processed
// before connect so a quick reconnect can't be torn down by its
// predecessor's leftover disconnect flag.
void WebPortalApp::processLiveMailbox() {
    uint32_t connectId;
    bool disconnect, timeNew, partialNew;
    uint64_t epochMs;
    char partial[LiveLink::kMaxCaptionText + 1];
    char finals[RX_FINAL_QUEUE][LiveLink::kMaxCaptionText + 1];
    int finalCount;

    portENTER_CRITICAL(&liveMux);
    connectId  = rxConnectId;   rxConnectId = 0;
    disconnect = rxDisconnect;  rxDisconnect = false;
    timeNew    = rxTimeNew;     rxTimeNew = false;
    epochMs    = rxEpochMs;
    partialNew = rxPartialNew;  rxPartialNew = false;
    if (liveBufs != nullptr) {
        memcpy(partial, liveBufs->rxPartial, sizeof(partial));
        finalCount = rxFinalCount;
        for (int i = 0; i < finalCount; ++i) {
            memcpy(finals[i], liveBufs->rxFinals[i], sizeof(finals[i]));
        }
    } else {
        partial[0] = '\0';
        partialNew = false;
        finalCount = 0;
    }
    rxFinalCount = 0;
    portEXIT_CRITICAL(&liveMux);

    if (disconnect && liveClientId != 0) {
        WP_LOG("live: client disconnected");
        stopLiveSession(nullptr, /*notifyClient=*/false);
    }
    if (connectId != 0) {
        startLiveSession(connectId);
    }
    if (liveClientId != 0) {
        if (timeNew && epochMs >= 1600000000000ULL) {
            // Same clock-set semantics as POST /api/time (tz-adjusted
            // local-naive epoch), piggybacked on the live link.
            struct timeval tv;
            tv.tv_sec  = (time_t)(epochMs / 1000ULL);
            tv.tv_usec = (suseconds_t)((epochMs % 1000ULL) * 1000ULL);
            settimeofday(&tv, nullptr);
            WP_LOGF("live: clock set, epoch %lu", (unsigned long)tv.tv_sec);
        }
        for (int i = 0; i < finalCount; ++i) {
            applyCaption(finals[i], true);
        }
        if (partialNew) {
            applyCaption(partial, false);
        }
    }
}

void WebPortalApp::startLiveSession(uint32_t clientId) {
    AsyncWebSocketClient* client = (ws != nullptr) ? ws->client(clientId) : nullptr;
    if (client == nullptr || client->status() != WS_CONNECTED) {
        // Connected and vanished within one tick — give back the claim.
        portENTER_CRITICAL(&liveMux);
        if (wsClaimedId == clientId) wsClaimedId = 0;
        portEXIT_CRITICAL(&liveMux);
        return;
    }

    // Internal heap is the scarce resource here: WiFi AP+STA + the browser's
    // HTTP connection pool already burn most of it, and the mic acquire wants
    // ~12KB more. If we let acquire run when there isn't a safe margin, the
    // I2S/WiFi allocations can tip the system into an OOM abort (a hard
    // reboot that kills the portal) instead of failing cleanly. So gate the
    // whole session on a heap floor and refuse gracefully below it — the
    // device must stay reachable no matter how tight memory gets.
    size_t freeHeap = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (freeHeap < LIVE_HEAP_FLOOR) {
        WP_LOGF("live: refused, low heap (%u < %u)",
                (unsigned)freeHeap, (unsigned)LIVE_HEAP_FLOOR);
        char buf[120];
        if (LiveLink::buildError(buf, sizeof(buf),
                "device is low on memory - close other browser tabs and reconnect")) {
            client->text(buf);
        }
        client->close();
        portENTER_CRITICAL(&liveMux);
        if (wsClaimedId == clientId) wsClaimedId = 0;
        portEXIT_CRITICAL(&liveMux);
        return;
    }

    LiveSessionBufs* sessionBufs = static_cast<LiveSessionBufs*>(
        heap_caps_malloc(sizeof(LiveSessionBufs),
                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (sessionBufs == nullptr) {
        sessionBufs = static_cast<LiveSessionBufs*>(
            malloc(sizeof(LiveSessionBufs)));
    }
    if (sessionBufs == nullptr) {
        WP_LOG("live: refused, session buffer allocation failed");
        char buf[120];
        if (LiveLink::buildError(buf, sizeof(buf),
                "device is low on memory - close other browser tabs and reconnect")) {
            client->text(buf);
        }
        client->close();
        portENTER_CRITICAL(&liveMux);
        if (wsClaimedId == clientId) wsClaimedId = 0;
        portEXIT_CRITICAL(&liveMux);
        return;
    }
    memset(sessionBufs, 0, sizeof(*sessionBufs));
    portENTER_CRITICAL(&liveMux);
    liveBufs = sessionBufs;
    rxPartialNew = false;
    rxFinalCount = 0;
    portEXIT_CRITICAL(&liveMux);

    const char* err = nullptr;
    // Smallest viable DMA cushion (2 x 512B = ~32ms, still >> the capture
    // task's 5ms poll) to leave the most internal heap for WiFi + the WS
    // frame queue.
    if (!MicCapture::instance().acquire("Live captions",
                                        LiveLink::kPcmSampleRate, &err, 2)) {
        WP_LOGF("live: mic acquire failed (%s)", err ? err : "?");
        char buf[80];
        if (LiveLink::buildError(buf, sizeof(buf),
                                 err != nullptr ? err : "mic unavailable")) {
            client->text(buf);
        }
        client->close();
        portENTER_CRITICAL(&liveMux);
        liveBufs = nullptr;
        if (wsClaimedId == clientId) wsClaimedId = 0;
        portEXIT_CRITICAL(&liveMux);
        free(sessionBufs);
        return;
    }

    liveMicHeld = true;
    liveClientId = clientId;
    liveSentFrames = 0;
    liveDroppedFrames = 0;
    liveFrameFill = 0;
    liveStartMs = millis_NOW;
    captionFinalCount = 0;
    liveBufs->captionPartial[0] = '\0';
    captionLastRxMs = 0;

    char hello[128];
    if (LiveLink::buildHello(hello, sizeof(hello), getFirmwareVersionString())) {
        client->text(hello);
    }
    MicCapture::instance().startStreaming(0);  // no press-click to skip here
    WP_LOGF("live: session started (client %u)", (unsigned)clientId);
}

void WebPortalApp::stopLiveSession(const char* reason, bool notifyClient) {
    if (liveClientId == 0) return;
    uint32_t oldId = liveClientId;

    if (notifyClient && ws != nullptr) {
        AsyncWebSocketClient* client = ws->client(oldId);
        if (client != nullptr && client->status() == WS_CONNECTED) {
            char buf[80];
            if (LiveLink::buildStop(buf, sizeof(buf),
                                    reason != nullptr ? reason : "stopped")) {
                client->text(buf);
            }
            client->close();
        }
    }

    if (liveMicHeld) {
        MicCapture::instance().stopStreaming();
        MicCapture::instance().release();
        liveMicHeld = false;
    }
    liveClientId = 0;
    liveFrameFill = 0;

    portENTER_CRITICAL(&liveMux);
    LiveSessionBufs* sessionBufs = liveBufs;
    liveBufs = nullptr;
    if (wsClaimedId == oldId) wsClaimedId = 0;
    portEXIT_CRITICAL(&liveMux);
    free(sessionBufs);
    WP_LOG("live: session stopped");
}

// Main loop: move captured PCM toward the client in fixed kPcmFrameBytes
// frames. Backpressure per the link contract: never block, never buffer
// unboundedly — when the client's send queue saturates and the ring is
// half full, the OLDEST staged frame is dropped (and counted) so the
// freshest audio survives.
void WebPortalApp::pumpLiveAudio() {
    if (liveClientId == 0 || liveBufs == nullptr ||
        !liveMicHeld || ws == nullptr) return;
    AsyncWebSocketClient* client = ws->client(liveClientId);
    if (client == nullptr || client->status() != WS_CONNECTED) {
        stopLiveSession(nullptr, false);
        return;
    }

    RecRingBuffer& ring = MicCapture::instance().ring();
    // Real time is ~0.4 frames per 20ms tick; allow a few for catch-up
    // after a WiFi power-save burst without monopolizing the loop.
    int sends = 0;
    while (sends < 4) {
        if (liveFrameFill < LiveLink::kPcmFrameBytes) {
            liveFrameFill += ring.pop(liveBufs->liveFrameBuf + liveFrameFill,
                                      LiveLink::kPcmFrameBytes - liveFrameFill);
            if (liveFrameFill < LiveLink::kPcmFrameBytes) break;  // partial frame
        }
        // Cap in-flight frames well below WS_MAX_QUEUED_MESSAGES: each
        // queued frame is a 5KB DRAM copy inside AsyncWebSocket, and with
        // WiFi AP+STA up the portal runs lean after the mic acquire — two
        // in flight keeps real-time cadence with headroom.
        if (client->queueLen() >= 2) {
            if (ring.available() >= ring.capacity() / 2) {
                liveFrameFill = 0;          // drop-oldest
                liveDroppedFrames++;
                continue;
            }
            break;                          // ring has headroom; just wait
        }
        // Heap floor before the 5KB enqueue malloc: if internal heap has
        // fallen near empty (a new HTTP client landed, WiFi burst), DON'T
        // hand AsyncWebSocket a 5KB allocation it might satisfy at the
        // expense of the WiFi/lwIP stack — drop the frame and let heap
        // recover. Keeps a busy stream from starving the radio into a
        // hard fault; audio degrades, the device stays up.
        if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
                < LIVE_FRAME_HEAP_FLOOR) {
            liveFrameFill = 0;
            liveDroppedFrames++;
            break;
        }
        if (client->binary(liveBufs->liveFrameBuf, LiveLink::kPcmFrameBytes)) {
            liveSentFrames++;
        } else {
            liveDroppedFrames++;            // enqueue refused = frame lost
        }
        liveFrameFill = 0;
        sends++;
    }
}

// ---------------------------------------------------------------------------
// OLED caption screen
// ---------------------------------------------------------------------------

// Width measurer for CaptionWrap — uses the display's CURRENT font, so the
// caller must select the caption font before wrapping.
static int wpMeasureWidth(const char* s, void* /*ctx*/) {
    return (int)display.getStringWidth(String(s));
}

void WebPortalApp::applyCaption(const char* text, bool isFinal) {
    if (liveBufs == nullptr) return;
    captionLastRxMs = millis_NOW;
    if (!isFinal) {
        // Partial: replaces the in-progress line (re-wrapped every render).
        strncpy(liveBufs->captionPartial, text, LiveLink::kMaxCaptionText);
        liveBufs->captionPartial[LiveLink::kMaxCaptionText] = '\0';
        return;
    }
    if (text[0] == '\0') {
        liveBufs->captionPartial[0] = '\0';   // empty final just clears the partial
        return;
    }
    // Final: commit to the scrollback (oldest falls off), clear the partial.
    if (captionFinalCount == CAPTION_SCROLLBACK) {
        for (int i = 1; i < CAPTION_SCROLLBACK; ++i) {
            memcpy(liveBufs->captionFinals[i - 1],
                   liveBufs->captionFinals[i],
                   sizeof(liveBufs->captionFinals[i]));
        }
        captionFinalCount = CAPTION_SCROLLBACK - 1;
    }
    strncpy(liveBufs->captionFinals[captionFinalCount], text,
            LiveLink::kMaxCaptionText);
    liveBufs->captionFinals[captionFinalCount]
                           [LiveLink::kMaxCaptionText] = '\0';
    captionFinalCount++;
    liveBufs->captionPartial[0] = '\0';
}

void WebPortalApp::renderCaptionScreen() {
    if (liveBufs == nullptr) return;
    // Header strip: a live dot + elapsed time, kept compact so the caption
    // rows get the screen.
    display.setColor(WHITE);
    display.fillRect(0, 0, 128, 13);
    display.setColor(BLACK);
    display.setFont(ArialMT_Plain_10);
    display.setTextAlignment(TEXT_ALIGN_LEFT);
    unsigned long upS = (millis_NOW - liveStartMs) / 1000;
    char hdr[24];
    snprintf(hdr, sizeof(hdr), "LIVE  %lu:%02lu", upS / 60, upS % 60);
    display.drawString(3, 1, hdr);
    if ((millis_NOW / 600) & 1) display.fillCircle(121, 6, 3);
    display.setColor(WHITE);

    const uint8_t* font = captionLargeFont ? ArialMT_Plain_16 : ArialMT_Plain_10;
    const int lineH    = captionLargeFont ? 16 : 12;
    const int topY     = 15;
    const int maxRows  = captionLargeFont ? 3 : 4;
    display.setFont(font);
    display.setTextAlignment(TEXT_ALIGN_LEFT);

    if (captionFinalCount == 0 && liveBufs->captionPartial[0] == '\0') {
        display.setTextAlignment(TEXT_ALIGN_CENTER);
        display.setFont(ArialMT_Plain_10);
        display.drawString(64, 24, "Listening...");
        display.drawString(64, 38, "Captions appear here");
        display.display();
        return;
    }

    // Wrap finals + partial into display rows; keep the newest rows.
    char rows[12][CaptionWrap::kMaxLineChars + 1];
    int rowCount = 0;
    auto appendWrapped = [&](const char* text) {
        char wrapped[6][CaptionWrap::kMaxLineChars + 1];
        int n = CaptionWrap::wrap(text, 124, wpMeasureWidth, nullptr, wrapped, 6);
        for (int i = 0; i < n; ++i) {
            if (rowCount == 12) {
                for (int j = 1; j < 12; ++j) memcpy(rows[j - 1], rows[j], sizeof(rows[j]));
                rowCount = 11;
            }
            memcpy(rows[rowCount], wrapped[i], sizeof(rows[rowCount]));
            rowCount++;
        }
    };
    for (int i = 0; i < captionFinalCount; ++i) {
        appendWrapped(liveBufs->captionFinals[i]);
    }
    if (liveBufs->captionPartial[0] != '\0') {
        appendWrapped(liveBufs->captionPartial);
    }

    int first = (rowCount > maxRows) ? rowCount - maxRows : 0;
    int y = topY;
    for (int i = first; i < rowCount; ++i) {
        display.drawString(2, y, rows[i]);
        y += lineH;
    }
    display.display();
}

// ---------------------------------------------------------------------------
// OLED rendering
// ---------------------------------------------------------------------------
void WebPortalApp::render() {
    display.clear();

    if (exitConfirmPending) {
        display.setColor(WHITE);
        display.setFont(ArialMT_Plain_16);
        display.setTextAlignment(TEXT_ALIGN_CENTER);
        display.drawString(64, 4, "Exit portal?");
        display.setFont(ArialMT_Plain_10);
        display.drawString(64, 27, "Your Cyber Fidget");
        display.drawString(64, 39, "will restart.");
        display.drawString(64, 52, "ENTER = yes   BACK = no");
        display.display();
        return;
    }

    // A live caption session takes over the screen — it IS the product
    // surface while a listener is connected.
    if (liveClientId != 0) {
        renderCaptionScreen();
        return;
    }

    // Header bar
    display.setColor(WHITE);
    display.fillRect(0, 0, 128, 14);
    display.setColor(BLACK);
    display.setFont(ArialMT_Plain_10);
    display.setTextAlignment(TEXT_ALIGN_CENTER);
    display.drawString(64, 1, "CyberFidget Web");
    display.setColor(WHITE);

    if (!sdReady) {
        display.setTextAlignment(TEXT_ALIGN_CENTER);
        display.drawString(64, 30, "No SD Card");
        display.display();
        return;
    }

    display.setFont(ArialMT_Plain_10);
    display.setTextAlignment(TEXT_ALIGN_LEFT);

    // AP info
    if (apReady) {
        display.drawString(4, 16, String("AP: ") + WiFi.softAPIP().toString());
    } else {
        display.drawString(4, 16, "AP failed (low mem)");
    }

    // STA info
    if (staConnected) {
        display.drawString(4, 28, staSSID + " " + WiFi.localIP().toString());
        if (mdnsStarted) {
            display.drawString(4, 40, "cyberfidget.local");
        } else {
            display.drawString(4, 40, String("Files: ") + String(fileCount));
        }
    } else if (staSSID.length()) {
        display.drawString(4, 28, "Connecting: " + staSSID);
        display.drawString(4, 40, String("Files: ") + String(fileCount));
    } else {
        display.drawString(4, 28, "WiFi: not connected");
        display.drawString(4, 40, String("Files: ") + String(fileCount));
    }

    if (uploadInProgress && uploadBytesTotal > 0) {
        // Upload progress (overwrites bottom line)
        int pct = (int)((uint64_t)uploadBytesReceived * 100 / uploadBytesTotal);
        if (pct > 100) pct = 100;
        display.drawProgressBar(4, 54, 100, 8, pct);
        display.setTextAlignment(TEXT_ALIGN_RIGHT);
        display.drawString(124, 52, String(pct) + "%");
    } else {
        // File count + clients
        int clients = WiFi.softAPgetStationNum();
        String info = String(fileCount) + " files";
        if (clients > 0) info += " | " + String(clients) + " client" + (clients > 1 ? "s" : "");
        display.drawString(4, 52, info);
    }

    display.display();
}
