// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#ifndef SERIAL_CLI_H
#define SERIAL_CLI_H

#include <stddef.h>
#include <stdint.h>

// Line-buffered Serial command processor for USB UART. The always-on verbs
// are identification (`version`, `info`, `help`), bench observation/control
// (`mark <id>`, `reboot`, `battery`, `diary`), display observation (`menutree`,
// `screencap`, `screenstream`), and the sync-transport family (below). Builds
// with -DCF_TEST_CLI=1 (the `local_test` env) add device-control verbs for
// test automation: `apps`, `app`, `launch`, `net`, `mic`, `wifi`, `wasmstat`,
// `btn`, `sleep`, `rail`, `gauge`, and `uvlo`. Release and remote builds never
// compile those device-control verbs in.
//
// Sync-transport family (always compiled - the browser drives these over
// USB to install or recover apps/assets and edit the loadout; every file
// access is confined to the app/asset area and transferred data is
// checksummed):
//   fwrite <path> <size> <crc32>   open a chunked, restartable file write
//   fwdata <off> <len> <crc32>     write one chunk (raw bytes follow the line)
//   fwcommit                       verify whole-file crc + atomically rename
//   fwabort                        discard the in-progress write
//   fdelete <path>                 delete a confined file
//   flist <dir>                    list one confined directory
//   fstat <path>                   report confined-file size + whole crc
//   fread <path> <off> <len>       return one checksummed file chunk
//   lget                           report the loadout manifest (framed)
//   lapply <len> <crc32>           apply staged manifest ops (JSON follows)
//   syncinfo                       report storage, manifest, firmware version
//
// WiFi setup family (always compiled; announced by `syncinfo.setup=1`):
//   wifi scan                      list nearby networks (reported when done)
//   wifi add <len> <crc32>         save a network (raw `ssid\0pass` follows)
//   wifi try                       join the first saved network (reported when done)
//   wifi saved                     list the saved networks, names only
//
// All output uses a stable line-prefix so a test harness can parse without
// regex acrobatics:
//   [boot] - one-shot boot banner (printed by HAL, not here)
//   [cmd]  - CLI command responses
//   [err]  - CLI errors (overflow, unknown command)
//   [evt]  - reserved for future event streaming
//
// Call SerialCli::instance().poll() once per main loop. Buffer overflow,
// case-insensitive matching, and unknown-verb handling are all internal.
class SerialCli {
public:
    static SerialCli& instance();

    void poll();
    // A network pull must not overlap a partially written serial blob.
    bool ferryActive() const;
    // True while a USB sync session is under way: a serial transfer is open,
    // or a sync verb arrived within SyncProtocol::kUsbSessionHoldMs. A cloud
    // check-in does not start while this holds.
    bool holdsCheckins() const;
    // True (once) if a verb that moves data (SyncProtocol::isIdleActivityVerb)
    // arrived since the last call; AppManager treats it as use for the idle
    // sleep, like a button press.
    bool consumeUsbActivity();
    // Close an unfinished serial file transfer before LittleFS is formatted.
    void closeStorageForFactoryReset();
    // True while a `wifi scan` / `wifi try` (or a test-build radio probe)
    // owns WiFi. A check-in or link does not start while this holds.
    bool radioBusy() const;

#ifdef CF_TEST_CLI
    // Consumed by AppManager::loop so the display teardown never runs inside
    // command dispatch.
    bool consumeSleepRequest();
    bool soakActive() const { return soaking; }
#endif

    // Buffer size is exposed for testing and for callers that want to reason
    // about the maximum acceptable command length. Anything longer triggers
    // an `[err] line too long` and the buffer resets at the next newline.
    // Sized for the longest verb line: `fwrite <path> <size> <crc32>` with a
    // 96-char confined path (SyncProtocol::kMaxPathLen) plus the two numbers,
    // which overruns the old 112. (Previous driver: the test-mode
    // `wifi <ssid>|<pass>` line, 32-char SSID + 63-char passphrase.)
    static constexpr size_t kBufferSize = 160;

private:
    SerialCli() = default;
    SerialCli(const SerialCli&) = delete;
    SerialCli& operator=(const SerialCli&) = delete;

    // `retry`: the parked verb run again from pollDeferred(). A store-writing
    // verb that finds the store busy is parked until SyncProtocol::kBusyWaitMs
    // after its first arrival, and refused only once that has passed.
    void dispatch(const char* line, bool retry = false);
    // Runs a deferred store-writing verb once the store is free, or refuses
    // it when the wait runs out. True while it is still waiting.
    bool pollDeferred();
    void cmdVersion();
    void cmdInfo();
    void cmdHelp();
    void cmdMark(const char* arg);
    void cmdReboot();
    void cmdBattery();
    void cmdDiary(const char* arg);

    // Sync-transport verbs (always compiled). Session state for an
    // in-progress `fwrite` lives in a SyncProtocol::FerrySession owned by the
    // .cpp (with its LittleFS/UART adapters) so this header stays free of
    // Arduino filesystem types.
    void cmdFwrite(const char* args);
    void cmdFwdata(const char* args);
    void cmdFwcommit();
    void cmdFwabort();
    // Drops an open write session idle for SyncProtocol::kTransferIdleMs.
    void expireIdleTransfer();
    void cmdFdelete(const char* args);
    void cmdFlist(const char* args);
    void cmdFstat(const char* args);
    void cmdFread(const char* args);
    void cmdLget();
    void cmdLapply(const char* args);
    void cmdSyncinfo();
    // WiFi setup verbs (always compiled). Scan and try run on their own task
    // and report from pollUsbWifi().
    void cmdWifiScan();
    void cmdWifiTry();
    void cmdWifiSaved();
    void cmdWifiAdd(const char* args);
    void pollUsbWifi();
    // Serial framebuffer capture + streaming (always compiled -
    // these are observation/remote-display verbs, valuable in any build).
    void cmdScreencap();
    void cmdScreenstream(const char* arg);
    void emitScreencap();
    void pollScreenStream();
    unsigned long streamIntervalMs = 0;  // 0 = off
    unsigned long lastStreamMs     = 0;
#ifdef CF_TEST_CLI
    void cmdApps();
    void cmdLaunch(const char* arg);
    void cmdSoak(const char* arg);
    bool launchResolved(const char* arg, const char* replyVerb, int* appIndex);
    void cmdApp();
    void cmdNet();
    void cmdHeapstat();
    void cmdBtstat();
    void cmdTlsalloc(const char* arg);
    void cmdTlsprobe(const char* url);
    void pollTlsprobeResult();
    void cmdWifi(const char* arg);
    void cmdMic();
    void cmdSleep();
    void cmdRail(const char* args);
    void cmdGauge(const char* args);
    void cmdUvlo(const char* args);
    // Opens a sample ModalPrompt for bench screenshots; result arrives later.
    void cmdPrompt(const char* args);
    // Menu status bar bench states (post / popup / clear / checkin / read).
    void cmdStatus(const char* args);
    // Serial button injection. tap auto-releases after a delay.
    void cmdBtn(const char* args);
    void pollPendingTapReleases();
    static constexpr int kMaxInjectButtons = 6;
    static constexpr unsigned long kTapReleaseMs = 120;
    unsigned long tapReleaseDueMs[kMaxInjectButtons] = {0};
    bool sleepRequested = false;
    bool soaking = false;
#endif

    char   buffer[kBufferSize] = {0};
    size_t bufferLen           = 0;
    bool   overflow            = false;
    // A store-writing verb that arrived while a cloud check-in owned the
    // store waits here (its payload stays in the UART buffer) instead of
    // being refused at once.
    char     deferred[kBufferSize] = {0};
    bool     deferredPending       = false;
    uint32_t deferredAtMs          = 0;
    // When the open write session last saw fwrite/fwdata.
    uint32_t ferryAtMs             = 0;
    bool     usbActivity           = false;
    // Set by `wifi add`: the next line is not echoed if unknown, and is
    // wiped from the buffer (it could be the end of an over-long payload).
    bool     hideNextEcho          = false;
};

#endif  // SERIAL_CLI_H
