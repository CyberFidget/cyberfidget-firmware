// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#ifndef HOME_REMOTE_CLIENT_H
#define HOME_REMOTE_CLIENT_H

// Home Remote's network side. One worker task (started by the app's
// begin(), stopped by its end()) owns WiFi for the app's lifetime: it
// claims the radio (CloudSync::claimRadio, so check-ins and USB WiFi jobs
// wait and Bluetooth later needs a restart), joins a saved network
// (SavedWifi::join), then serves requests from the app one at a time over
// plain HTTP to the configured Home Assistant address, with a 2 s limit on
// each socket wait and 6 s on a whole request, and one kept connection.
// The access key goes only into the
// Authorization header; its buffers are wiped when the worker ends.
//
// The app never waits on the network: it posts a request (each slot is
// latest-wins: a newer one replaces one not yet started) and picks the
// result up on a later frame. Actions run before fetches.

#include <stddef.h>
#include <stdint.h>

#include "HomeRemoteLogic.h"

namespace HomeRemote {
namespace Client {

enum class StartError : uint8_t { None, NotConfigured, NoWifi, RadioBusy, NoMemory };

enum class Link : uint8_t {
    Off,          ///< no worker
    Joining,      ///< joining the saved network
    Ready,        ///< joined: requests are served
    JoinFailed,   ///< no saved network could be joined (a new request retries)
};

enum class Fetch : uint8_t { None, Favorites, Areas, AreaEntities, Entity, Effects };

struct FetchResult {
    uint32_t seq = 0;
    Fetch kind = Fetch::None;
    Outcome outcome = Outcome::Ok;
    int status = 0;
    int count = 0;     ///< rows, areas or effects kept
};

/// Loop task. Claims the radio and starts the worker.
StartError start();
/// Loop task. Asks the worker to end (WiFi off, radio released, key
/// wiped) and waits for it, then deletes the task. If it has not ended
/// after 3 s, WiFi is switched off from here (ending any socket wait) and it
/// gets 3 s more; failing that the device restarts. It never returns with
/// the worker still running.
void stop();
bool workerAlive();

Link link();
/// The network joined (a name, never a password); "" when not joined.
void networkName(char* out, size_t cap);

/// Each returns the request's sequence number (0 when refused).
uint32_t requestFetch(Fetch kind, const char* arg);
uint32_t requestAction(const Action& action);
/// A fetch or action is waiting or running.
bool fetchBusy();
bool actionBusy();

/// Copies a finished fetch out (once). Only the buffers for its kind are
/// filled; pass nullptr/0 for the others.
bool takeFetch(FetchResult& meta, Entity* rows, int maxRows, Area* areas, int maxAreas,
               char (*effects)[kEffectMax + 1], int maxEffects);
/// A finished action (once): its sequence number and outcome.
bool takeAction(uint32_t& seq, Outcome& outcome);

} // namespace Client
} // namespace HomeRemote

#endif // HOME_REMOTE_CLIENT_H
