// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#ifndef HOME_REMOTE_LOGIC_H
#define HOME_REMOTE_LOGIC_H

// Home Remote's pure parts: no Arduino, no display, no clock (callers pass
// the time in). Host-tested in test/test_home_remote_logic.
//
// - The wire format. Home Assistant renders each list server-side (POST
//   /api/template) as one line per row, so the device never downloads or
//   parses the full state dump:
//     entity: entity_id|name|state|brightness|r,g,b|caps|effect_count|effect
//     area:   area_id|name|count
//     effect: one JSON string per line (decoded exactly, see decodeJsonString)
//   caps: bit 0 = can dim, bit 1 = can take a colour. r,g,b is empty when
//   the light reports no colour (off, or a white-only light).
//   speaker: entity_id|name|state|volume|title|features|muted|artist
//   volume is 0-255 (empty when unknown), features are HA feature bits,
//   muted is 0/1. Title/artist are display-only, sanitized like names.
// - Request bodies: the template bodies and the service call bodies.
// - Controls: soft takeover for the physical slider, the outgoing send
//   gate (latest wins, at most one send per interval), the poll guard (a
//   polled value does not overwrite a control changed locally in the last
//   1.5 s) and the hold-to-repeat acceleration for Left/Right.

#include <stddef.h>
#include <stdint.h>

namespace HomeRemote {

constexpr int kMaxRows = 32;        // rows kept from any list
constexpr int kMaxEffects = 40;
constexpr size_t kIdMax = 64;       // entity / area id
constexpr size_t kNameMax = 48;     // a shown name
constexpr size_t kEffectMax = 32;   // an effect name
constexpr size_t kLineMax = 256;    // a longer line is dropped
constexpr size_t kSpeakerLineMax = 768;  // room for the server's UTF-8 display text
constexpr size_t kUrlMax = 96;
constexpr size_t kTokenMax = 300;   // Home Assistant's long-lived keys are ~183

/// The label that marks an entity as a favorite in Home Assistant.
constexpr const char* kFavoriteLabel = "cyber_fidget";

enum class Kind : uint8_t { Light, Scene, Script, Speaker, Other };
enum class SpeakerState : uint8_t { Playing, Paused, Idle, Off };
enum SpeakerFeature : uint32_t {
    Pause = 1, VolumeSet = 4, VolumeMute = 8, PreviousTrack = 16,
    NextTrack = 32, Stop = 4096, Play = 16384
};
constexpr size_t kMediaTextMax = 32;

struct Entity {
    char id[kIdMax + 1] = {0};
    char name[kNameMax + 1] = {0};
    Kind kind = Kind::Other;
    bool on = false;
    bool available = true;     ///< false for "unavailable" / "unknown"
    uint8_t brightness = 0;    ///< 0-255 (0 when off or unknown)
    bool hasRgb = false;
    uint8_t r = 0, g = 0, b = 0;
    bool canDim = false;
    bool canColor = false;
    uint8_t effectCount = 0;
    char effect[kEffectMax + 1] = {0};
    SpeakerState speakerState = SpeakerState::Idle;
    uint32_t features = 0;
    bool hasVolume = false;
    uint8_t volume = 0;
    bool muted = false;
    char title[kMediaTextMax + 1] = {0};
    char artist[kMediaTextMax + 1] = {0};
};

struct Area {
    char id[kIdMax + 1] = {0};
    char name[kNameMax + 1] = {0};
    uint16_t count = 0;
};

Kind kindOf(const char* entityId);
const char* speakerStateText(SpeakerState state);
bool speakerSupports(uint32_t features, SpeakerFeature feature);
bool speakerCanPlayPause(uint32_t features, SpeakerState state);
uint8_t volumeByte(int percent);
int volumePercent(int level);
/// Whole percent steps, capped at five even during accelerated repeats.
uint8_t steppedVolume(uint8_t level, int direction, int amount);

/// One line (without its newline). False for a malformed line.
bool parseEntityLine(const char* line, size_t len, Entity& out);
bool parseAreaLine(const char* line, size_t len, Area& out);

/// Whole bodies: every well-formed line up to max; malformed lines are
/// skipped. Returns the count kept.
int parseEntities(const char* body, size_t len, Entity* out, int max);
int parseAreas(const char* body, size_t len, Area* out, int max);
int parseEffects(const char* body, size_t len, char (*out)[kEffectMax + 1], int max);

/// Entity ids are domain.object_id: lowercase letters, digits and '_',
/// exactly one '.'. Area ids: lowercase letters, digits and '_'. Only ids
/// that pass are ever placed inside a template.
bool validEntityId(const char* id);
bool validAreaId(const char* id);

/// JSON string escaping (no surrounding quotes). Returns the length
/// written, or 0 when it would not fit (out is then "").
size_t jsonEscape(const char* in, char* out, size_t cap);

/// "http://host[:port][/path]" with any trailing '/' removed. Only plain
/// http (Home Assistant's default on a home network); no spaces, no
/// credentials in the URL, no query or fragment. False when refused.
bool normalizeBaseUrl(const char* in, char* out, size_t cap);

/// A token can be saved: printable ASCII without spaces, 1..kTokenMax.
bool validTokenChars(const char* token, size_t len);

// ---- templates --------------------------------------------------------------

enum class Template : uint8_t { Favorites, Areas, AreaEntities, Entity, Effects };
constexpr size_t kTemplateMax = 2048;

/// The exact Jinja text sent for a list (arg: the area or entity id, where
/// one is needed). Every template lives in HomeRemoteLogic.cpp only. Returns
/// the length, or 0 when it would not fit or the id is refused.
size_t templateText(Template kind, const char* arg, char* out, size_t cap);

/// One JSON string literal (with its quotes) decoded to UTF-8. Returns the
/// length, or -1 when it is malformed, holds a NUL or does not fit in cap
/// (terminator included).
int decodeJsonString(const char* in, size_t len, char* out, size_t cap);

/// Text safe to draw: control characters become spaces (the display's width
/// lookup has no entry below the space). `in` may be `out`.
void displayText(const char* in, char* out, size_t cap);

/// Zeroes memory in a way the compiler keeps (secrets before a free/return).
void secureWipe(void* p, size_t n);

// ---- request bodies (JSON for POST /api/template) ---------------------------
// Each returns the body length, or 0 when it would not fit or an id is
// refused.
size_t favoritesBody(char* out, size_t cap);
size_t areasBody(char* out, size_t cap);
size_t areaEntitiesBody(const char* areaId, char* out, size_t cap);
size_t entityBody(const char* entityId, char* out, size_t cap);
size_t effectsBody(const char* entityId, char* out, size_t cap);

// ---- service calls ----------------------------------------------------------

struct Action {
    enum class Type : uint8_t {
        None, Toggle, TurnOn, TurnOff, Run, PlayPause, SetVolume, Mute,
        Previous, Next
    };
    Type type = Type::None;
    char entity[kIdMax + 1] = {0};
    bool setBrightness = false;
    uint8_t brightness = 0;
    bool setRgb = false;
    uint8_t r = 0, g = 0, b = 0;
    bool setEffect = false;
    char effect[kEffectMax + 1] = {0};
    int volumePercent = 0;   ///< clamped to 0..100, serialized as 0.00..1.00
    bool muted = false;
};

/// "/api/services/<domain>/<service>" for the action. False when refused.
bool servicePath(const Action& a, char* out, size_t cap);
/// The JSON body. Returns its length, or 0 when refused.
size_t serviceBody(const Action& a, char* out, size_t cap);

// ---- outcome of one request -------------------------------------------------

enum class Outcome : uint8_t { Ok, Unreachable, Rejected, BadReply };
/// transportOk: the request was sent and a status line came back.
Outcome classify(bool transportOk, int status);

// ---- display helpers --------------------------------------------------------

/// "#RRGGBB" (8 bytes with the terminator).
void formatHex(uint8_t r, uint8_t g, uint8_t b, char out[8]);
/// 0-255 to a whole percent, rounded.
int brightnessPercent(uint8_t level);

struct Preset {
    const char* name;
    uint8_t r, g, b;
};
constexpr int kPresetCount = 8;
const Preset& preset(int index);

// ---- mirroring the colour on the Fidget's own LEDs --------------------------

struct LedColor {
    uint8_t r = 0, g = 0, b = 0;
    bool operator==(const LedColor& o) const { return r == o.r && g == o.g && b == o.b; }
    bool operator!=(const LedColor& o) const { return !(*this == o); }
};

/// The brightest the mirror ever drives a channel (of 255): enough to see,
/// not blinding, light on the battery.
constexpr uint8_t kLedCap = 48;

/// The colour the front LEDs show for a light: off when the light is off;
/// its colour (warm white when it reports none) scaled by its brightness
/// and by kLedCap. A channel that is on never rounds down to dark.
LedColor mirrorColor(bool on, bool hasRgb, uint8_t r, uint8_t g, uint8_t b, uint8_t brightness);

// ---- list cursor ------------------------------------------------------------

/// Selection and scroll window for a list. Up/Down wrap. setCount keeps the
/// selection where it was when it still exists (a refreshed list).
class ListCursor {
public:
    void reset(int count, int visibleRows);
    void setCount(int count);
    void up();
    void down();
    int count() const { return count_; }
    int selected() const { return selected_; }
    int top() const { return top_; }
    int visible() const { return count_ < rows_ ? count_ : rows_; }
private:
    void follow();
    int count_ = 0;
    int rows_ = 1;
    int selected_ = 0;
    int top_ = 0;
};

// ---- soft takeover ----------------------------------------------------------

/// The physical slider drives a value only after it has "picked it up":
/// opening a control never changes it. On arm() the slider is somewhere;
/// it does nothing until it comes within kTolerance of the real value or
/// crosses it, and from then on each move of at least kMinStep is a new
/// value to send. A real value that changes while engaged by more than
/// kTolerance from the slider (someone else changed it) drops the pickup
/// until the slider meets it again. All values 0-255.
class SoftTakeover {
public:
    static constexpr int kTolerance = 6;
    static constexpr int kMinStep = 2;

    /// Strict pickup requires crossing, consumes that movement without a
    /// send, and never engages merely because the slider is near the level.
    void arm(int real, int slider, bool strict = false);
    /// A new real value (a poll that is allowed to update the control).
    void setReal(int real, int slider);
    /// The slider's position now. True when *out is a new value to send.
    bool update(int slider, int* out);
    bool engaged() const { return engaged_; }
    int real() const { return real_; }
private:
    int real_ = 0;
    int side_ = 0;        // sign of (slider - real) when armed
    int lastSent_ = 0;    // the slider position last turned into a value
    bool engaged_ = false;
    bool strict_ = false;
};

// ---- outgoing send gate -----------------------------------------------------

/// Latest wins: changes mark the control dirty, and at most one send goes
/// out per kIntervalMs (5 per second), only when the previous one is done.
class SendGate {
public:
    static constexpr uint32_t kIntervalMs = 200;
    void markDirty() { dirty_ = true; }
    bool dirty() const { return dirty_; }
    /// True when a send may start now (the caller then calls sent()).
    bool ready(uint32_t nowMs, bool inFlight) const;
    void sent(uint32_t nowMs);
    void clear() { dirty_ = false; }
private:
    bool dirty_ = false;
    bool everSent_ = false;
    uint32_t lastMs_ = 0;
};

// ---- poll guard -------------------------------------------------------------

/// A polled value must not overwrite a control changed here in the last
/// kHoldMs (no "snap back" while adjusting).
class PollGuard {
public:
    enum Control : uint8_t { Power, Brightness, Color, Effect, kControls };
    static constexpr uint32_t kHoldMs = 1500;
    void reset();
    void touched(Control c, uint32_t nowMs);
    bool accepts(Control c, uint32_t nowMs) const;
private:
    bool touched_[kControls] = {false, false, false, false};
    uint32_t at_[kControls] = {0, 0, 0, 0};
};

// ---- hold-to-repeat ---------------------------------------------------------

/// Left/Right on a value row: one step on the press, then after
/// kDelayMs a step every kEveryMs, larger the longer it is held.
class RepeatAccel {
public:
    static constexpr uint32_t kDelayMs = 400;
    static constexpr uint32_t kEveryMs = 60;
    void press(uint32_t nowMs);
    void release() { active_ = false; }
    bool active() const { return active_; }
    /// Units to move since the last call while held (0 when none due).
    int steps(uint32_t nowMs);
    /// The size of one repeat step after holding for heldMs.
    static int stepSize(uint32_t heldMs);
private:
    bool active_ = false;
    uint32_t startMs_ = 0;
    uint32_t lastMs_ = 0;
};

} // namespace HomeRemote

#endif // HOME_REMOTE_LOGIC_H
