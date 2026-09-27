// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#ifndef CLOUD_PLANNER_H
#define CLOUD_PLANNER_H

#include <stdint.h>

namespace CloudSync {

enum class Step : uint8_t {
    Idle, Reboot, Checkin, Loadout, Download, Apply, Ack,
    Waiting, Done, Backoff, Deferred, Error
};

/// How a loadout offer was judged before any blob byte is written.
enum class OfferVerdict : uint8_t {
    Ok,              ///< download and apply it
    Retryable,       ///< transport-like failure; try again next session
    Permanent,       ///< a defect that will never succeed: answer rejected
    AlreadyApplied,  ///< the applied record holds this batch + doc CRC
};

/// Outcome of fetching and committing every blob of an offer.
enum class BlobVerdict : uint8_t { Ok, Retryable, Permanent };

// Pure routing decisions; transport, time, files and UI stay with the driver.
// The worker acts on every returned step, so these transitions are the
// device's behaviour.
class CloudPlanner {
public:
    Step start(bool bluetoothIdle);
    Step checkin(int http, bool hasBatch, bool firmwareOffer,
                 bool autoapply, uint32_t nextMs, uint32_t retryMs,
                 bool waiting = false);
    Step loadout(int http, OfferVerdict verdict);
    Step blob(BlobVerdict verdict);
    Step applied(bool success);
    /// Before the follow-up check-in that carries the answer: the server's
    /// floor must pass first. Without budget for it, the answer is deferred
    /// to the next session (an applied batch is reported from its record).
    Step report(bool budgetCovers);
    Step ack(int http, uint32_t nextMs, uint32_t retryMs);
    /// From Backoff: retry the refused request once when the budget covers
    /// its Retry-After, otherwise defer to the next session.
    Step retry(bool budgetCovers);
    /// End of session. Only a radio that failed to switch off reboots.
    Step failure(bool radioOff);

    Step step() const { return step_; }
    bool firmwareOffered() const { return firmwareOffered_; }
    bool rejected() const { return rejected_; }
    /// True only when this session's apply changed the menu (not for an
    /// already-applied batch or a refusal).
    bool appliedNow() const { return appliedNow_; }
    uint32_t nextMs() const { return nextMs_; }
private:
    Step step_ = Step::Idle;
    Step retryStep_ = Step::Idle;
    bool retried_ = false;
    bool firmwareOffered_ = false;
    bool rejected_ = false;
    bool appliedNow_ = false;
    uint32_t nextMs_ = 0;
};

} // namespace CloudSync
#endif
