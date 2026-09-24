// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#include "PromptPolicy.h"

#include <stdio.h>
#include <string.h>

namespace PromptPolicy {

using CheckinPolicy::Policy;
using CheckinPolicy::Verdict;

namespace {
const char* orDefault(const char* text, const char* fallback) {
    return text && text[0] ? text : fallback;
}
} // namespace

namespace {
bool isDigit(char c) { return c >= '0' && c <= '9'; }
bool isIdentChar(char c) {
    return isDigit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-';
}

// A number without leading zeros; advances p.
bool parseNumber(const char*& p, uint32_t& out) {
    if (!isDigit(*p)) return false;
    if (*p == '0' && isDigit(p[1])) return false;
    uint32_t v = 0;
    while (isDigit(*p)) {
        if (v > 100000000u) return false;
        v = v * 10u + (uint32_t)(*p - '0');
        p++;
    }
    out = v;
    return true;
}

// Dot-separated non-empty identifiers; advances p to the first character
// after them. Numeric identifiers may not have leading zeros when
// `strictNumeric` (prerelease; build metadata allows them).
bool parseIdents(const char*& p, bool strictNumeric) {
    for (;;) {
        const char* start = p;
        bool numeric = true;
        while (isIdentChar(*p)) { numeric = numeric && isDigit(*p); p++; }
        if (p == start) return false;
        if (strictNumeric && numeric && *start == '0' && p - start > 1) return false;
        if (*p != '.') return true;
        p++;
    }
}

bool numericIdent(const char* s, size_t n) {
    for (size_t i = 0; i < n; i++) if (!isDigit(s[i])) return false;
    return n > 0;
}

int compareIdent(const char* a, size_t an, const char* b, size_t bn) {
    const bool na = numericIdent(a, an), nb = numericIdent(b, bn);
    if (na && nb) {
        if (an != bn) return an < bn ? -1 : 1;   // no leading zeros: longer is bigger
        const int c = strncmp(a, b, an);
        return c < 0 ? -1 : c > 0 ? 1 : 0;
    }
    if (na != nb) return na ? -1 : 1;           // numeric sorts below text
    const size_t n = an < bn ? an : bn;
    const int c = strncmp(a, b, n);
    if (c != 0) return c < 0 ? -1 : 1;
    return an == bn ? 0 : (an < bn ? -1 : 1);
}
} // namespace

bool parseVersion(const char* text, Version& out) {
    out = Version();
    if (!text || strlen(text) > kMaxVersionLen) return false;
    const char* p = text;
    for (int part = 0; part < 3; part++) {
        if (!parseNumber(p, out.core[part])) return false;
        if (part < 2) {
            if (*p != '.') return false;
            p++;
        }
    }
    if (*p == '-') {
        const char* start = ++p;
        if (!parseIdents(p, true)) return false;
        const size_t n = (size_t)(p - start);
        memcpy(out.pre, start, n);
        out.pre[n] = 0;
    }
    if (*p == '+') {
        p++;
        if (!parseIdents(p, false)) return false;
    }
    return *p == 0;
}

int compareVersions(const Version& a, const Version& b) {
    for (int i = 0; i < 3; i++) {
        if (a.core[i] != b.core[i]) return a.core[i] < b.core[i] ? -1 : 1;
    }
    if (!a.pre[0] || !b.pre[0]) {
        if (!a.pre[0] && !b.pre[0]) return 0;
        return a.pre[0] ? -1 : 1;   // a prerelease sorts below its final
    }
    const char* x = a.pre;
    const char* y = b.pre;
    for (;;) {
        const size_t xn = strcspn(x, ".");
        const size_t yn = strcspn(y, ".");
        const int c = compareIdent(x, xn, y, yn);
        if (c != 0) return c;
        const bool xMore = x[xn] == '.';
        const bool yMore = y[yn] == '.';
        if (!xMore || !yMore) return xMore == yMore ? 0 : (xMore ? 1 : -1);
        x += xn + 1;
        y += yn + 1;
    }
}

bool isNewer(const char* offered, const char* running) {
    Version o, r;
    if (!parseVersion(offered, o)) return false;
    if (!parseVersion(running, r)) return true;
    return compareVersions(o, r) > 0;
}

bool sameVersion(const char* a, const char* b) {
    return a && b && a[0] && strcmp(a, b) == 0;
}

bool offerEligible(const char* avail, const char* rej, const char* running) {
    if (!avail || !avail[0]) return false;   // nothing cached: no notification
    if (sameVersion(avail, rej)) return false;
    return isNewer(avail, running);
}

FwEffect firmwareChoice(int choice, bool updateSessionAvailable) {
    FwEffect e;
    switch ((FwChoice)choice) {
        case FwChoice::Install:
            if (updateSessionAvailable) e.handoff = true;
            else { e.comingSoon = true; e.keepInBar = true; }
            break;
        case FwChoice::Skip:
            e.writeRej = true;
            break;
        case FwChoice::Later:
        default:   // no answer (timeout, teardown) is the same as Later
            e.keepInBar = true;
            break;
    }
    return e;
}

AppEffect appChoice(int choice) {
    AppEffect e;
    if ((AppChoice)choice == AppChoice::GetNow) e.applyNow = true;
    else e.keepInBar = true;
    return e;
}

bool parseAutoapply(bool present, bool stored) { return present ? stored : true; }

AppBatch appBatch(bool autoapply, bool applyOnce) {
    return autoapply || applyOnce ? AppBatch::Apply : AppBatch::LeavePending;
}

RestartOneShot restartForCheck(bool applyWaiting) {
    RestartOneShot o;
    o.bootcloud = true;
    o.bootapply = applyWaiting;
    o.skipanim = true;
    return o;
}

CheckResume resumeAfterRestart(bool bootcloud, bool bootapply, bool otherAppFirst) {
    CheckResume r;
    if (!bootcloud || otherAppFirst) return r;
    r.openCheckScreen = true;
    r.runCheck = true;
    r.applyWaiting = bootapply;
    return r;
}

CheckEntry checkEntry(bool resumedSessionStarted, bool sessionBusy) {
    return resumedSessionStarted || sessionBusy ? CheckEntry::WatchSession
                                                : CheckEntry::StartNew;
}

PromptPlan bootPlan(Policy policy, bool fwEligible, bool appsWaiting) {
    PromptPlan p;
    if (policy == Policy::Never) return p;   // Auto-check Off: no popup
    p.firmware = fwEligible;
    p.apps = appsWaiting;
    return p;
}

PromptPlan manualPlan(Policy policy, bool fwEligible, bool appsWaiting) {
    PromptPlan p;
    p.firmware = fwEligible;
    p.apps = appsWaiting;
    p.explainOff = policy == Policy::Never;
    return p;
}

bool automaticAllowed(Policy policy) { return policy != Policy::Never; }

const char* manualCopy(Verdict verdict) {
    switch (verdict) {
        case Verdict::RebootFirst: return kRestartingToCheck;
        case Verdict::Busy:        return kAlreadyChecking;
        default:                   return kChecking;
    }
}

void firmwareTitle(char* out, size_t len, const char* version, const char* source) {
    snprintf(out, len, "Update %s ready (%s)", orDefault(version, "?"),
             orDefault(source, kDefaultSource));
}

void appsTitle(char* out, size_t len, uint32_t count) {
    if (count == 0) snprintf(out, len, "App changes waiting");
    else snprintf(out, len, "%lu app change%s waiting", (unsigned long)count,
                  count == 1 ? "" : "s");
}

DevMode parseDevMode(uint8_t stored) {
    return stored == 1 ? DevMode::On : stored == 2 ? DevMode::Always : DevMode::Off;
}

const char* devModeLabel(DevMode mode) {
    switch (mode) {
        case DevMode::On:     return "On";
        case DevMode::Always: return "Always on";
        default:              return "Off";
    }
}

uint32_t sanitizeDevIdleMin(uint32_t stored) {
    return stored == 0 || stored > 7u * 24u * 60u ? kDefaultDevIdleMin : stored;
}

bool devRestartNeeded(DevMode before, DevMode after) { return before != after; }

Row settingsRow(int index) {
    static const Row order[kSettingsRows] = {
        Row::CheckNow, Row::AutoCheck, Row::AutoApply, Row::Channel, Row::Source,
        Row::Skip, Row::Link, Row::DevMode, Row::Status,
    };
    return index >= 0 && index < kSettingsRows ? order[index] : Row::Status;
}

SkipAction skipAction(const SettingsState& s) {
    const bool skipped = s.rej && s.rej[0];
    // The offered version is the skipped one: undo that skip.
    if (skipped && sameVersion(s.rej, s.avail)) return SkipAction::Unskip;
    // A newer offer can be skipped while an older skip is stored; the new
    // skip replaces the old one.
    if (offerEligible(s.avail, s.rej, s.running)) return SkipAction::Skip;
    // Nothing offered: an old skip can still be undone.
    if (skipped) return SkipAction::Unskip;
    return SkipAction::Nothing;
}

void settingsLabel(Row row, const SettingsState& s, char* out, size_t len) {
    switch (row) {
        case Row::CheckNow:
            snprintf(out, len, "Check now");
            break;
        case Row::AutoCheck:
            snprintf(out, len, "Auto-check: %s", s.policy == Policy::Never ? "Off" : "On");
            break;
        case Row::AutoApply:
            snprintf(out, len, "Apply app changes automatically: %s", s.autoapply ? "On" : "Off");
            break;
        case Row::Channel:
            snprintf(out, len, "Channel: %s", orDefault(s.channel, kDefaultChannel));
            break;
        case Row::Source:
            snprintf(out, len, "Source: %s", orDefault(s.source, kDefaultSource));
            break;
        case Row::Skip:
            switch (skipAction(s)) {
                case SkipAction::Unskip: snprintf(out, len, "Unskip %s", s.rej); break;
                case SkipAction::Skip:   snprintf(out, len, "Skip %s", s.avail); break;
                default:                 snprintf(out, len, "Skip: no update waiting"); break;
            }
            break;
        case Row::Link:
            snprintf(out, len, s.linked ? "Unlink this Fidget" : "Link this Fidget");
            break;
        case Row::DevMode:
            snprintf(out, len, "Dev mode: %s", devModeLabel(s.dev));
            break;
        case Row::Status:
            if (s.policy == Policy::Never) snprintf(out, len, "%s", kOffExplanation);
            else if (s.status && s.status[0]) snprintf(out, len, "Status: %s", s.status);
            else snprintf(out, len, "Status: nothing waiting");
            break;
    }
}

} // namespace PromptPolicy
