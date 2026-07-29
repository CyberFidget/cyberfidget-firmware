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
#include "CaptionWrap.h"
#include "LiveLinkProtocol.h"

class WebPortalApp {
public:
    WebPortalApp(ButtonManager& btnMgr);

    void begin();
    void update();
    void end();

    static void onButtonBack(const ButtonEvent& event);
    static void onButtonUp(const ButtonEvent& event);   // caption font toggle

private:
    static WebPortalApp* instance;
    ButtonManager& buttonManager;

    static constexpr const char* AP_SSID = "CyberFidget";

    // Web server + DNS (heap-allocated server for clean lifecycle)
    AsyncWebServer* server = nullptr;
    DNSServer dnsServer;

    // AP bring-up state: false when WiFi.softAP() failed (surfaced on the OLED
    // and gates captive DNS instead of silently binding 0.0.0.0).
    bool apReady = false;

    // SD state
    bool sdReady = false;
    int fileCount = 0;

    // WiFi STA state
    bool staConnected = false;
    bool mdnsStarted = false;
    String staSSID;
    unsigned long staConnectStart = 0;
    static const unsigned long STA_TIMEOUT_MS = 15000;

    // Upload tracking (written by async handler, read by render loop)
    volatile bool uploadInProgress = false;
    volatile size_t uploadBytesReceived = 0;
    volatile size_t uploadBytesTotal = 0;
    File uploadFile;

    // --- Live caption link (/ws/live) ---
    //
    // The WS event callback fires on the AsyncTCP task; everything the main
    // loop consumes is handed across through rx* fields under liveMux, and
    // update() does the actual work (mic acquire, clock set, caption wrap)
    // in main-loop context. Owned by the server's handler list (freed with
    // the server in end()).
    AsyncWebSocket* ws = nullptr;
    portMUX_TYPE liveMux = portMUX_INITIALIZER_UNLOCKED;
    // Async-task -> main-loop mailbox (guarded by liveMux):
    uint32_t wsClaimedId = 0;        // single-client gate, claimed at CONNECT
    uint32_t rxConnectId = 0;        // nonzero: a client finished the WS handshake
    bool     rxDisconnect = false;   // the live client went away
    bool     rxTimeNew = false;
    uint64_t rxEpochMs = 0;
    bool     rxPartialNew = false;
    static const int RX_FINAL_QUEUE = 4;
    int      rxFinalCount = 0;       // finals must not be lost; small FIFO

    // Main-loop-only session state:
    uint32_t liveClientId = 0;       // 0 = no live session
    bool     liveMicHeld = false;
    uint32_t liveSentFrames = 0;
    uint32_t liveDroppedFrames = 0;
    uint32_t liveFrameFill = 0;
    unsigned long liveStartMs = 0;

    // OLED caption screen (main-loop only). Finals are kept as raw text and
    // wrapped at render time, so the font-size toggle re-wraps correctly.
    static const int CAPTION_SCROLLBACK = 4;
    int  captionFinalCount = 0;
    bool captionLargeFont = false;
    unsigned long captionLastRxMs = 0;

    struct LiveSessionBufs {
        // AsyncTCP task under liveMux:
        char rxPartial[LiveLink::kMaxCaptionText + 1];
        char rxFinals[RX_FINAL_QUEUE][LiveLink::kMaxCaptionText + 1];
        // Main loop only:
        uint8_t liveFrameBuf[LiveLink::kPcmFrameBytes];
        char captionFinals[CAPTION_SCROLLBACK][LiveLink::kMaxCaptionText + 1];
        char captionPartial[LiveLink::kMaxCaptionText + 1];
    };
    LiveSessionBufs* liveBufs = nullptr;

    // Live link helpers (main loop unless noted)
    void onWsEvent(AsyncWebSocketClient* client, AwsEventType type,
                   void* arg, uint8_t* data, size_t len);  // AsyncTCP task
    void processLiveMailbox();
    void startLiveSession(uint32_t clientId);
    void stopLiveSession(const char* reason, bool notifyClient);
    void pumpLiveAudio();
    void applyCaption(const char* text, bool isFinal);
    void renderCaptionScreen();

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
