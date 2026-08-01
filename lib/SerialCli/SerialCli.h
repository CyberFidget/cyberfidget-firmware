// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#ifndef SERIAL_CLI_H
#define SERIAL_CLI_H

#include <stddef.h>

// Line-buffered Serial command processor for USB UART. The always-on verbs
// are identification (`version`, `info`, `help`) and the sync-transport
// family (below); builds with -DCF_TEST_CLI=1 (the `local_test` env) add
// device-control verbs for test automation: `apps`, `launch <name|index>`,
// `app`, `net`, `wifi <ssid>|<pass>`. Release and remote builds never
// compile the device-control verbs in, so normal use can't trigger a
// surprise app switch.
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

    void dispatch(const char* line);
    void cmdVersion();
    void cmdInfo();
    void cmdHelp();

    // Sync-transport verbs (always compiled). Session state for an
    // in-progress `fwrite` lives in file-scope statics in the .cpp so this
    // header stays free of Arduino filesystem types.
    void cmdFwrite(const char* args);
    void cmdFwdata(const char* args);
    void cmdFwcommit();
    void cmdFwabort();
    void cmdFdelete(const char* args);
    void cmdFlist(const char* args);
    void cmdFstat(const char* args);
    void cmdFread(const char* args);
    void cmdLget();
    void cmdLapply(const char* args);
    void cmdSyncinfo();
    // T-191 serial tunnel: framebuffer capture + streaming (always compiled -
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
    void cmdApp();
    void cmdNet();
    void cmdWifi(const char* arg);
    void cmdMic();
    // Serial button injection (T-191). tap auto-releases after a delay.
    void cmdBtn(const char* args);
    void pollPendingTapReleases();
    static constexpr int kMaxInjectButtons = 6;
    static constexpr unsigned long kTapReleaseMs = 120;
    unsigned long tapReleaseDueMs[kMaxInjectButtons] = {0};
#endif

    char   buffer[kBufferSize] = {0};
    size_t bufferLen           = 0;
    bool   overflow            = false;
};

#endif  // SERIAL_CLI_H
