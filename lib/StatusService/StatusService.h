// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#ifndef STATUS_SERVICE_H
#define STATUS_SERVICE_H

#include <stdint.h>
#include <string.h>

/**
 * @brief Notification / status store behind the main-menu status bar.
 *
 * Any subsystem posts short status lines here; the menu's status bar shows
 * the most important one, the "Status" menu item carries a badge while
 * something needs attention, and the Status screen lists everything
 * pending. The service only carries events: it has no policy, no storage,
 * no networking and no display code, and callers pass the time in, so the
 * whole thing is native-tested (test_core_status).
 *
 * Entries
 *  - post(kind, text, priority, sticky, nowMs, flags) adds or refreshes one
 *    entry. Empty text uses the kind's default copy (kinds without default
 *    copy refuse empty text).
 *  - The four update states (Checking, UpdateReady, ChangesWaiting,
 *    Listening) hold at most one entry each: a new post of that kind
 *    replaces the old one. Info / Warning entries are keyed by kind + text,
 *    so different subsystems can post side by side.
 *  - Sticky entries stay until cleared. Non-sticky entries expire
 *    kTransientMs after their last post (expire(nowMs)).
 *  - Flags: Late (the result arrived after the moment it was meant for,
 *    e.g. after the boot window closed) and Cached (a stored earlier result,
 *    not a fresh one) travel with the entry for the Status screen.
 *
 * Bar line and pending list
 *  - current() is the entry with the highest priority; ties go to the most
 *    recent post. pending() lists every entry in that same order.
 *
 * Badge
 *  - An entry needs attention when it was posted at Priority::High or
 *    arrived through an ignored popup. badge() is true while any entry
 *    needs attention; markSeen() (the Status screen was viewed) clears
 *    that without removing entries. Re-posting an entry with identical
 *    kind and text keeps its seen state; a changed text asks again.
 *
 * Popups
 *  - resolvePopup(kind, text, accepted, ...) records the answer to a major
 *    event popup: accepted clears that entry (the caller acts on it);
 *    ignored or unanswered routes it to the bar as a sticky entry that
 *    needs attention - never dropped.
 *
 * Last check-in
 *  - setCheckIn(atSec, cached) / clearCheckIn() record when the device
 *    last checked in, in seconds on whatever clock the caller uses (pass
 *    the same clock to ageBucket / glyph). The WiFi glyph shows the AGE of
 *    that check-in, never a live connection: WiFi is normally off. The only
 *    live state is Listening (Dev mode), shown while that entry exists.
 */

enum class StatusKind : uint8_t {
    Info = 0,        // generic line from any subsystem
    Warning,         // e.g. a battery warning
    Checking,        // "Checking for updates..."
    UpdateReady,     // "Update ready"
    ChangesWaiting,  // "App changes waiting"
    Listening,       // Dev mode is on and listening
    Count
};

namespace StatusPriority {
constexpr uint8_t Low    = 0;
constexpr uint8_t Normal = 1;
constexpr uint8_t High   = 2;   // needs attention: sets the badge
}

namespace StatusFlag {
constexpr uint8_t Late   = 0x01;
constexpr uint8_t Cached = 0x02;
}

enum class CheckInAge : uint8_t {
    Never = 0,   // no check-in recorded
    Hour,        // less than 1 hour ago
    Today,       // less than 24 hours ago
    Days         // 24 hours or more
};

enum class StatusGlyph : uint8_t {
    Never = 0,   // WiFi glyph: never checked in
    Hour,        // checked in less than an hour ago
    Today,       // checked in within 24 hours
    Days,        // checked in a day or more ago
    Live         // Dev mode listening right now
};

struct StatusEntry {
    static constexpr int kMaxText = 48;   // bytes including the terminator

    StatusKind kind      = StatusKind::Info;
    uint8_t    priority  = StatusPriority::Normal;
    uint8_t    flags     = 0;
    bool       sticky    = false;
    bool       attention = false;
    uint32_t   postedMs  = 0;
    uint32_t   seq       = 0;
    char       text[kMaxText] = {0};

    bool late() const   { return (flags & StatusFlag::Late) != 0; }
    bool cached() const { return (flags & StatusFlag::Cached) != 0; }
};

class StatusService {
public:
    static constexpr int      kMaxEntries = 8;
    static constexpr uint32_t kTransientMs = 10000;
    static constexpr uint32_t kHourSec = 3600;
    static constexpr uint32_t kDaySec  = 86400;

    /** The device-wide instance the menu bar and every poster share. */
    static StatusService &instance() {
        static StatusService single;
        return single;
    }

    // ---- Kind metadata (plain copy, no jargon) ----

    /** Copy used when a post has no text; "" for kinds that need text. */
    static const char *defaultText(StatusKind kind) {
        switch (kind) {
            case StatusKind::Checking:       return "Checking for updates...";
            case StatusKind::UpdateReady:    return "Update ready";
            case StatusKind::ChangesWaiting: return "App changes waiting";
            case StatusKind::Listening:      return "Dev mode";
            default:                         return "";
        }
    }

    static uint8_t defaultPriority(StatusKind kind) {
        switch (kind) {
            case StatusKind::UpdateReady:
            case StatusKind::ChangesWaiting: return StatusPriority::High;
            case StatusKind::Info:           return StatusPriority::Low;
            default:                         return StatusPriority::Normal;
        }
    }

    /** One entry per kind for the update states; Info/Warning key on text. */
    static bool singleInstance(StatusKind kind) {
        return kind == StatusKind::Checking || kind == StatusKind::UpdateReady ||
               kind == StatusKind::ChangesWaiting || kind == StatusKind::Listening;
    }

    /** Stable lowercase name (serial read-back, logs). */
    static const char *kindName(StatusKind kind) {
        switch (kind) {
            case StatusKind::Info:           return "info";
            case StatusKind::Warning:        return "warning";
            case StatusKind::Checking:       return "checking";
            case StatusKind::UpdateReady:    return "ready";
            case StatusKind::ChangesWaiting: return "changes";
            case StatusKind::Listening:      return "listening";
            default:                         return "?";
        }
    }

    /** Inverse of kindName(); false for an unknown name. */
    static bool kindFromName(const char *name, StatusKind *out) {
        if (!name || !out) return false;
        for (uint8_t k = 0; k < (uint8_t)StatusKind::Count; k++) {
            if (strcmp(name, kindName((StatusKind)k)) == 0) {
                *out = (StatusKind)k;
                return true;
            }
        }
        return false;
    }

    // ---- Posting ----

    /**
     * @brief Add or refresh an entry. Text is copied (truncated to fit).
     * @return false when the text is empty and the kind has no default, or
     *         the store is full of entries that all outrank this one.
     */
    bool post(StatusKind kind, const char *text, uint8_t priority, bool sticky,
              uint32_t nowMs, uint8_t flags = 0) {
        if ((uint8_t)kind >= (uint8_t)StatusKind::Count) return false;
        const char *body = (text && text[0]) ? text : defaultText(kind);
        if (!body[0]) return false;

        int slot = find(kind, body);
        bool sameText = false;
        if (slot >= 0) {
            sameText = strncmp(entries_[slot].text, body, StatusEntry::kMaxText - 1) == 0;
        } else {
            slot = freeSlot(priority);
            if (slot < 0) return false;
        }

        StatusEntry &e = entries_[slot];
        const bool wasAttention = e.attention;
        const bool refresh = used_[slot] && sameText;
        e.kind     = kind;
        e.priority = priority;
        e.flags    = flags;
        e.sticky   = sticky;
        e.postedMs = nowMs;
        e.seq      = ++seq_;
        copyText(e.text, body);
        const bool wants = priority >= StatusPriority::High;
        // An identical repost keeps its seen state; new or changed text asks again.
        if (refresh) {
            e.attention = !seen_[slot] && (wasAttention || wants);
        } else {
            e.attention = wants;
            seen_[slot] = false;
        }
        used_[slot] = true;
        return true;
    }

    /** Remove every entry of this kind. Returns how many were removed. */
    int clear(StatusKind kind) {
        int n = 0;
        for (int i = 0; i < kMaxEntries; i++) {
            if (used_[i] && entries_[i].kind == kind) { drop(i); n++; }
        }
        return n;
    }

    /** Remove every entry (the check-in record is kept). */
    void clearAll() {
        for (int i = 0; i < kMaxEntries; i++) drop(i);
    }

    /** Drop non-sticky entries older than kTransientMs. */
    void expire(uint32_t nowMs) {
        for (int i = 0; i < kMaxEntries; i++) {
            if (used_[i] && !entries_[i].sticky &&
                (uint32_t)(nowMs - entries_[i].postedMs) >= kTransientMs) {
                drop(i);
            }
        }
    }

    // ---- Popups ----

    /**
     * @brief Record a popup answer. Accepted clears the kind's entry (or the
     * matching Info/Warning line); ignored or unanswered routes the event to
     * the bar as a sticky entry that needs attention.
     */
    void resolvePopup(StatusKind kind, const char *text, bool accepted,
                      uint32_t nowMs, uint8_t flags = 0) {
        const char *body = (text && text[0]) ? text : defaultText(kind);
        if (accepted) {
            if (singleInstance(kind)) {
                clear(kind);
            } else {
                const int slot = find(kind, body);
                if (slot >= 0) drop(slot);
            }
            return;
        }
        uint8_t pri = defaultPriority(kind);
        if (pri < StatusPriority::High) pri = StatusPriority::High;
        if (!post(kind, body, pri, true, nowMs, flags)) return;
        const int slot = find(kind, body);
        if (slot >= 0) {
            entries_[slot].attention = true;
            seen_[slot] = false;
        }
    }

    // ---- Reading ----

    /** Highest priority, then most recent; nullptr when empty. */
    const StatusEntry *current() const {
        const StatusEntry *best = nullptr;
        for (int i = 0; i < kMaxEntries; i++) {
            if (!used_[i]) continue;
            if (!best || outranks(entries_[i], *best)) best = &entries_[i];
        }
        return best;
    }

    /** Fills out[] in bar order (see current()); returns the count written. */
    int pending(const StatusEntry **out, int max) const {
        int n = 0;
        for (int i = 0; i < kMaxEntries && n < max; i++) {
            if (!used_[i]) continue;
            int j = n++;
            while (j > 0 && outranks(entries_[i], *out[j - 1])) {
                out[j] = out[j - 1];
                j--;
            }
            out[j] = &entries_[i];
        }
        return n;
    }

    int count() const {
        int n = 0;
        for (int i = 0; i < kMaxEntries; i++) if (used_[i]) n++;
        return n;
    }

    bool has(StatusKind kind) const {
        for (int i = 0; i < kMaxEntries; i++) {
            if (used_[i] && entries_[i].kind == kind) return true;
        }
        return false;
    }

    /** True while anything needs attention (the Status item's badge). */
    bool badge() const {
        for (int i = 0; i < kMaxEntries; i++) {
            if (used_[i] && entries_[i].attention) return true;
        }
        return false;
    }

    /** The Status screen was viewed: clear attention, keep the entries. */
    void markSeen() {
        for (int i = 0; i < kMaxEntries; i++) {
            if (!used_[i]) continue;
            entries_[i].attention = false;
            seen_[i] = true;
        }
    }

    // ---- Last check-in ----

    void setCheckIn(uint32_t atSec, bool cached) {
        hasCheckIn_ = true;
        checkInSec_ = atSec;
        checkInCached_ = cached;
    }

    void clearCheckIn() {
        hasCheckIn_ = false;
        checkInSec_ = 0;
        checkInCached_ = false;
    }

    bool hasCheckIn() const       { return hasCheckIn_; }
    bool checkInCached() const    { return checkInCached_; }
    uint32_t checkInSec() const   { return checkInSec_; }

    /** Seconds since the check-in (0 if the clock reads earlier). */
    uint32_t checkInAgeSec(uint32_t nowSec) const {
        if (!hasCheckIn_ || nowSec < checkInSec_) return 0;
        return nowSec - checkInSec_;
    }

    CheckInAge ageBucket(uint32_t nowSec) const {
        if (!hasCheckIn_) return CheckInAge::Never;
        const uint32_t age = checkInAgeSec(nowSec);
        if (age < kHourSec) return CheckInAge::Hour;
        if (age < kDaySec)  return CheckInAge::Today;
        return CheckInAge::Days;
    }

    /** WiFi glyph state: Live only while Listening, else check-in age. */
    StatusGlyph glyph(uint32_t nowSec) const {
        if (has(StatusKind::Listening)) return StatusGlyph::Live;
        switch (ageBucket(nowSec)) {
            case CheckInAge::Hour:  return StatusGlyph::Hour;
            case CheckInAge::Today: return StatusGlyph::Today;
            case CheckInAge::Days:  return StatusGlyph::Days;
            default:                return StatusGlyph::Never;
        }
    }

    static const char *glyphName(StatusGlyph g) {
        switch (g) {
            case StatusGlyph::Hour:  return "hour";
            case StatusGlyph::Today: return "today";
            case StatusGlyph::Days:  return "days";
            case StatusGlyph::Live:  return "live";
            default:                 return "never";
        }
    }

    /**
     * @brief Short age label drawn beside the glyph: "--" never, "<1h",
     * "<N>h" within a day, "<N>d" after. Empty while Live (the bar line
     * says Dev mode). Writes at most len bytes including the terminator.
     */
    void ageLabel(uint32_t nowSec, char *buf, int len) const {
        if (!buf || len <= 0) return;
        buf[0] = '\0';
        const StatusGlyph g = glyph(nowSec);
        const uint32_t age = checkInAgeSec(nowSec);
        switch (g) {
            case StatusGlyph::Never: writeText(buf, len, "--"); break;
            case StatusGlyph::Hour:  writeText(buf, len, "<1h"); break;
            case StatusGlyph::Today: writeNumber(buf, len, age / kHourSec, 'h'); break;
            case StatusGlyph::Days:  writeNumber(buf, len, age / kDaySec, 'd'); break;
            default: break;
        }
    }

    /** Back to empty (tests; also a clean slate for bench runs). */
    void reset() {
        clearAll();
        clearCheckIn();
        seq_ = 0;
    }

private:
    static bool outranks(const StatusEntry &a, const StatusEntry &b) {
        if (a.priority != b.priority) return a.priority > b.priority;
        return (int32_t)(a.seq - b.seq) > 0;
    }

    static void copyText(char *dst, const char *src) {
        strncpy(dst, src, StatusEntry::kMaxText - 1);
        dst[StatusEntry::kMaxText - 1] = '\0';
    }

    static void writeText(char *buf, int len, const char *s) {
        strncpy(buf, s, (size_t)len - 1);
        buf[len - 1] = '\0';
    }

    static void writeNumber(char *buf, int len, uint32_t n, char unit) {
        char tmp[12];
        int i = 0;
        do { tmp[i++] = (char)('0' + n % 10); n /= 10; } while (n && i < 10);
        int o = 0;
        while (i > 0 && o < len - 2) buf[o++] = tmp[--i];
        if (o < len - 1) buf[o++] = unit;
        buf[o] = '\0';
    }

    int find(StatusKind kind, const char *body) const {
        for (int i = 0; i < kMaxEntries; i++) {
            if (!used_[i] || entries_[i].kind != kind) continue;
            if (singleInstance(kind)) return i;
            if (strncmp(entries_[i].text, body, StatusEntry::kMaxText - 1) == 0) return i;
        }
        return -1;
    }

    // A free slot, else evict the lowest-ranked entry that the new post
    // (at this priority, newest) outranks; -1 when nothing can go.
    int freeSlot(uint8_t priority) {
        int victim = -1;
        for (int i = 0; i < kMaxEntries; i++) {
            if (!used_[i]) return i;
            if (victim < 0 || outranks(entries_[victim], entries_[i])) victim = i;
        }
        if (victim >= 0 && entries_[victim].priority <= priority) {
            drop(victim);
            return victim;
        }
        return -1;
    }

    void drop(int i) {
        used_[i] = false;
        seen_[i] = false;
        entries_[i] = StatusEntry();
    }

    StatusEntry entries_[kMaxEntries];
    bool        used_[kMaxEntries] = {false};
    bool        seen_[kMaxEntries] = {false};
    uint32_t    seq_ = 0;
    bool        hasCheckIn_ = false;
    bool        checkInCached_ = false;
    uint32_t    checkInSec_ = 0;
};

#endif
