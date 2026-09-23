// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#include "CloudPlanner.h"

namespace CloudSync {

Step CloudPlanner::start(bool bluetoothIdle) {
    *this = CloudPlanner();
    step_ = bluetoothIdle ? Step::Checkin : Step::Reboot;
    return step_;
}

Step CloudPlanner::checkin(int http, bool hasBatch, bool firmwareOffer,
                           bool autoapply, uint32_t nextMs, uint32_t retryMs) {
    if (step_ != Step::Checkin) return step_ = Step::Error;
    firmwareOffered_ = firmwareOffer;
    nextMs_ = nextMs;
    if (http == 429) {
        nextMs_ = retryMs > nextMs_ ? retryMs : nextMs_;
        retryStep_ = Step::Checkin;
        return step_ = Step::Backoff;
    }
    if (http == 204) return step_ = Step::Done;
    if (http != 200) return step_ = Step::Error;
    if (!hasBatch) return step_ = Step::Done;
    return step_ = autoapply ? Step::Loadout : Step::Waiting;
}

Step CloudPlanner::loadout(int http, OfferVerdict verdict) {
    if (step_ != Step::Loadout) return step_ = Step::Error;
    if (http == 204) return step_ = Step::Done;
    if (http == 429) { retryStep_ = Step::Loadout; return step_ = Step::Backoff; }
    if (http != 200) return step_ = Step::Error;
    switch (verdict) {
        case OfferVerdict::Ok:             return step_ = Step::Download;
        case OfferVerdict::AlreadyApplied: return step_ = Step::Ack;
        case OfferVerdict::Permanent:      rejected_ = true; return step_ = Step::Ack;
        default:                           return step_ = Step::Error;
    }
}

Step CloudPlanner::blob(BlobVerdict verdict) {
    if (step_ != Step::Download) return step_ = Step::Error;
    if (verdict == BlobVerdict::Ok) return step_ = Step::Apply;
    if (verdict == BlobVerdict::Permanent) { rejected_ = true; return step_ = Step::Ack; }
    return step_ = Step::Error;
}

Step CloudPlanner::applied(bool success) {
    if (step_ != Step::Apply) return step_ = Step::Error;
    // A refusal is answered too; the result string carries which.
    if (!success) rejected_ = true;
    appliedNow_ = success;
    return step_ = Step::Ack;
}

Step CloudPlanner::report(bool budgetCovers) {
    if (step_ != Step::Ack) return step_ = Step::Error;
    return step_ = budgetCovers ? Step::Ack : Step::Deferred;
}

Step CloudPlanner::ack(int http, uint32_t nextMs, uint32_t retryMs) {
    if (step_ != Step::Ack) return step_ = Step::Error;
    nextMs_ = nextMs;
    if (http == 429) {
        nextMs_ = retryMs > nextMs_ ? retryMs : nextMs_;
        retryStep_ = Step::Ack;
        return step_ = Step::Backoff;
    }
    return step_ = (http == 200 || http == 204) ? Step::Done : Step::Error;
}

Step CloudPlanner::retry(bool budgetCovers) {
    if (step_ != Step::Backoff) return step_ = Step::Error;
    if (retried_ || !budgetCovers) return step_ = Step::Deferred;
    retried_ = true;
    return step_ = retryStep_;
}

Step CloudPlanner::failure(bool radioOff) {
    return step_ = radioOff ? Step::Error : Step::Reboot;
}

} // namespace CloudSync
