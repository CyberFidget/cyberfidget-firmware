// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#include "WebPortalApp.h"
#include "portal_page.h"
#include "MenuManager.h"
#include "globals.h"
#include "RecNaming.h"   // shared index.csv row parser (lib/VoiceRecorderApp)

#include <SD.h>
#include <SPI.h>
#include <WiFi.h>
#include <esp_bt.h>
#include <sys/time.h>    // settimeofday for POST /api/time

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
WebPortalApp webPortalApp(HAL::buttonManager());

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------
WebPortalApp::WebPortalApp(ButtonManager& btnMgr)
    : buttonManager(btnMgr) {
    instance = this;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------
void WebPortalApp::begin() {
    WP_LOG("begin: enter");
    instance = this;

    // Register Back button
    buttonManager.registerCallback(button_SelectIndex, onButtonBack);

    // Reset upload state
    uploadInProgress = false;
    uploadBytesReceived = 0;
    uploadBytesTotal = 0;

    // Init SD
    initSD();

    // Count files
    fileCount = 0;
    if (sdReady) countFilesRecursive(MEDIA_DIR);

    // Stop Bluetooth to free the radio for WiFi
    WP_LOG("begin: stopping BT controller");
    btStop();
    delay(100);

    // Start WiFi in AP+STA dual mode
    WP_LOG("begin: starting WiFi AP+STA");
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(AP_SSID);
    delay(100);
    WP_LOGF("begin: AP started, IP=%s", WiFi.softAPIP().toString().c_str());

    // Auto-connect to saved WiFi network (STA)
    staConnected = false;
    loadWifiCreds();

    // mDNS: cyberfidget.local
    if (MDNS.begin("cyberfidget")) {
        MDNS.addService("http", "tcp", 80);
        WP_LOG("begin: mDNS started (cyberfidget.local)");
    }

    // Start captive portal DNS (binds to AP interface only)
    dnsServer.start(53, "*", WiFi.softAPIP());

    // Create web server
    server = new AsyncWebServer(80);
    setupRoutes();
    server->begin();
    WP_LOG("begin: web server started");

    // Keep device awake
    millis_APP_LASTINTERACTION = millis_NOW;
}

void WebPortalApp::end() {
    WP_LOG("end: enter");

    // Close any in-progress upload
    if (uploadFile) {
        uploadFile.close();
    }
    uploadInProgress = false;

    // Stop web server
    if (server) {
        server->end();
        delete server;
        server = nullptr;
    }

    // Stop DNS + mDNS
    dnsServer.stop();
    MDNS.end();

    // Stop WiFi (STA + AP)
    WiFi.disconnect(false);
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
    staConnected = false;
    delay(100);
    WP_LOG("end: WiFi stopped");

    // Unregister callbacks
    buttonManager.unregisterCallback(button_SelectIndex);

    WP_LOG("end: done");
}

void WebPortalApp::update() {
    // Keep device awake
    millis_APP_LASTINTERACTION = millis_NOW;

    // Track STA connection state
    if (staSSID.length() && !staConnected) {
        if (WiFi.status() == WL_CONNECTED) {
            staConnected = true;
            WP_LOGF("STA connected, IP=%s", WiFi.localIP().toString().c_str());
        } else if (millis() - staConnectStart > STA_TIMEOUT_MS) {
            WP_LOG("STA connect timeout");
            staSSID = "";
        }
    }
    if (staConnected && WiFi.status() != WL_CONNECTED) {
        staConnected = false;
        WP_LOG("STA connection lost");
    }

    // Process DNS requests for captive portal
    dnsServer.processNextRequest();

    // Render OLED
    render();
}

// ---------------------------------------------------------------------------
// Button callback
// ---------------------------------------------------------------------------
void WebPortalApp::onButtonBack(const ButtonEvent& event) {
    if (event.eventType == ButtonEvent_Released) {
        WP_LOG("back pressed");
        MenuManager::instance().returnToMenu();
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
    WiFi.disconnect(false);
    staSSID = "";
    staConnected = false;
    Preferences prefs;
    prefs.begin("wificfg", false);
    prefs.clear();
    prefs.end();
    WP_LOG("WiFi credentials cleared");
}

// ---------------------------------------------------------------------------
// Web server routes
// ---------------------------------------------------------------------------
void WebPortalApp::setupRoutes() {
    // Main portal page
    server->on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
        req->send(200, "text/html", PORTAL_PAGE_HTML);
    });

    // Serve media files for audio playback
    server->serveStatic("/media/", SD, "/media/");

    // Serve voice notes for in-browser playback + download
    server->serveStatic("/recordings/", SD, "/recordings/");

    // API: File list (recursive JSON — for folder tree view)
    server->on("/api/files", HTTP_GET, [this](AsyncWebServerRequest* req) {
        handleFileList(req);
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

    // Captive portal catch-all: redirect everything else to our page
    server->onNotFound([](AsyncWebServerRequest* req) {
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
        // Determine target directory from query param or default to /media/
        String dir = MEDIA_DIR;
        if (req->hasParam("dir")) {
            dir = req->getParam("dir")->value();
        }
        String path = dir + "/" + filename;
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

    // Security: must be under /media/ (music) or /recordings/ (voice notes)
    bool isRec = path.startsWith("/recordings/");
    if (!path.startsWith("/media/") && !isRec) {
        req->send(403, "text/plain", "Forbidden");
        return;
    }

    if (!SD.exists(path)) {
        req->send(404, "text/plain", "Not found");
        return;
    }

    // --- Voice note: drop the .wav, its index.csv row, and any transcript
    // sidecar together so the metadata never outlives the file. A voice note
    // is always a single file (no subdirs under /recordings/).
    if (isRec) {
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

    if (SD.remove(path)) {
        WP_LOGF("deleted: %s", path.c_str());
        fileCount = 0;
        countFilesRecursive(MEDIA_DIR);
        invalidateMusicCache();
        req->send(200, "text/plain", "OK");
    } else {
        // Might be a directory — try rmdir
        if (SD.rmdir(path)) {
            WP_LOGF("removed dir: %s", path.c_str());
            fileCount = 0;
            countFilesRecursive(MEDIA_DIR);
            req->send(200, "text/plain", "OK");
        } else {
            req->send(500, "text/plain", "Delete failed");
        }
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

    if (!path.startsWith("/media/")) {
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

    // Security: a move must stay within one root — both under /media/ (music)
    // or both under /recordings/ (voice notes). Cross-root moves are refused so
    // the two stores never bleed into each other.
    bool mediaMove = from.startsWith("/media/") && to.startsWith("/media/");
    bool recMove   = from.startsWith("/recordings/") && to.startsWith("/recordings/");
    if (!mediaMove && !recMove) {
        req->send(403, "text/plain", "Forbidden");
        return;
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
    } else {
        invalidateMusicCache();
    }
    req->send(200, "text/plain", "OK");
}

// ---------------------------------------------------------------------------
// Route: Status
// ---------------------------------------------------------------------------
void WebPortalApp::handleStatus(AsyncWebServerRequest* req) {
    uint64_t totalBytes = SD.totalBytes();
    uint64_t usedBytes = SD.usedBytes();

    String json = "{";
    json += "\"files\":" + String(fileCount);
    json += ",\"totalBytes\":" + String((unsigned long)totalBytes);
    json += ",\"usedBytes\":" + String((unsigned long)usedBytes);
    json += ",\"clients\":" + String(WiFi.softAPgetStationNum());
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
        json += ",\"mdns\":\"cyberfidget.local\"";
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
// OLED rendering
// ---------------------------------------------------------------------------
void WebPortalApp::render() {
    display.clear();

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
    display.drawString(4, 16, String("AP: ") + WiFi.softAPIP().toString());

    // STA info
    if (staConnected) {
        display.drawString(4, 28, staSSID + " " + WiFi.localIP().toString());
        display.drawString(4, 40, "cyberfidget.local");
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
