// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#ifndef WEB_PORTAL_APP_H
#define WEB_PORTAL_APP_H

#include <Arduino.h>
#include <DNSServer.h>
#include <ESPAsyncWebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>

#include "HAL.h"
#include "AppDefs.h"
#include "ButtonManager.h"

class WebPortalApp {
public:
    WebPortalApp(ButtonManager& btnMgr);

    void begin();
    void update();
    void end();

    static void onButtonBack(const ButtonEvent& event);

private:
    static WebPortalApp* instance;
    ButtonManager& buttonManager;

    static constexpr const char* AP_SSID = "CyberFidget";

    // Web server + DNS (heap-allocated server for clean lifecycle)
    AsyncWebServer* server = nullptr;
    DNSServer dnsServer;

    // SD state
    bool sdReady = false;
    int fileCount = 0;

    // WiFi STA state
    bool staConnected = false;
    String staSSID;
    unsigned long staConnectStart = 0;
    static const unsigned long STA_TIMEOUT_MS = 15000;

    // Upload tracking (written by async handler, read by render loop)
    volatile bool uploadInProgress = false;
    volatile size_t uploadBytesReceived = 0;
    volatile size_t uploadBytesTotal = 0;
    File uploadFile;

    // SD helpers
    void initSD();
    void countFilesRecursive(const char* dir);
    void invalidateMusicCache();

    // Voice-note index.csv maintenance (keeps a recording's metadata row in
    // step when the portal renames/deletes the .wav). Crash-safe via index.tmp
    // swap, mirroring VoiceRecorderApp::deleteIndexRow.
    bool deleteRecordingIndexRow(const char* filename);
    bool renameRecordingIndexRow(const char* oldName, const char* newName);

    // WiFi STA helpers
    void loadWifiCreds();
    void connectSTA(const String& ssid, const String& pass, bool save);
    void disconnectSTA();

    // Web server setup
    void setupRoutes();

    // Route handlers
    void handleFileList(AsyncWebServerRequest* req);
    void handleBrowse(AsyncWebServerRequest* req);
    void handleDownload(AsyncWebServerRequest* req);
    void handleUpload(AsyncWebServerRequest* req, const String& filename,
                      size_t index, uint8_t* data, size_t len, bool final);
    void handleDelete(AsyncWebServerRequest* req);
    void handleMkdir(AsyncWebServerRequest* req);
    void handleMove(AsyncWebServerRequest* req);
    void handleTrackList(AsyncWebServerRequest* req);
    void handleStatus(AsyncWebServerRequest* req);
    void handleRecordings(AsyncWebServerRequest* req);
    void handleSetTime(AsyncWebServerRequest* req);
    void handlePlaylistList(AsyncWebServerRequest* req);
    void handlePlaylistGet(AsyncWebServerRequest* req);
    void handlePlaylistSave(AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t index, size_t total);
    void handlePlaylistDelete(AsyncWebServerRequest* req);

    // WiFi route handlers
    void handleWifiScan(AsyncWebServerRequest* req);
    void handleWifiConnect(AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t index, size_t total);
    void handleWifiStatus(AsyncWebServerRequest* req);
    void handleWifiForget(AsyncWebServerRequest* req);

    // OLED rendering
    void render();
};

extern WebPortalApp webPortalApp;

#endif // WEB_PORTAL_APP_H
