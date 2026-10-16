// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#include "HomeRemoteLogic.h"

#include <stdio.h>
#include <string.h>

namespace HomeRemote {
namespace {

// ---- line splitting ---------------------------------------------------------

constexpr int kMaxFields = 8;

struct Field {
    const char* p;
    size_t n;
};

// Splits [line, line+len) on '|' into exactly `want` fields.
bool split(const char* line, size_t len, Field* f, int want) {
    int count = 0;
    size_t start = 0;
    for (size_t i = 0; i <= len; i++) {
        if (i == len || line[i] == '|') {
            if (count >= want) return false;
            f[count].p = line + start;
            f[count].n = i - start;
            count++;
            start = i + 1;
        }
    }
    return count == want;
}

void copyField(const Field& f, char* out, size_t cap) {
    const size_t n = f.n < cap - 1 ? f.n : cap - 1;
    memcpy(out, f.p, n);
    out[n] = '\0';
}

bool fieldIs(const Field& f, const char* s) {
    return f.n == strlen(s) && memcmp(f.p, s, f.n) == 0;
}

// A whole number of at most `digits` digits, no sign, no spaces.
bool parseUint(const char* p, size_t n, int digits, uint32_t max, uint32_t& out) {
    if (n == 0 || n > (size_t)digits) return false;
    uint32_t v = 0;
    for (size_t i = 0; i < n; i++) {
        if (p[i] < '0' || p[i] > '9') return false;
        v = v * 10 + (uint32_t)(p[i] - '0');
    }
    if (v > max) return false;
    out = v;
    return true;
}

bool parseByte(const char* p, size_t n, uint8_t& out) {
    uint32_t v = 0;
    if (!parseUint(p, n, 3, 255, v)) return false;
    out = (uint8_t)v;
    return true;
}

bool parseRgb(const Field& f, Entity& e) {
    if (f.n == 0) { e.hasRgb = false; return true; }
    Field parts[3];
    int count = 0;
    size_t start = 0;
    for (size_t i = 0; i <= f.n; i++) {
        if (i == f.n || f.p[i] == ',') {
            if (count >= 3) return false;
            parts[count].p = f.p + start;
            parts[count].n = i - start;
            count++;
            start = i + 1;
        }
    }
    if (count != 3) return false;
    if (!parseByte(parts[0].p, parts[0].n, e.r) || !parseByte(parts[1].p, parts[1].n, e.g) ||
        !parseByte(parts[2].p, parts[2].n, e.b)) return false;
    e.hasRgb = true;
    return true;
}

// Trims spaces and '\r' at both ends.
void trim(const char*& p, size_t& n) {
    while (n && (*p == ' ' || *p == '\r' || *p == '\t')) { p++; n--; }
    while (n && (p[n - 1] == ' ' || p[n - 1] == '\r' || p[n - 1] == '\t')) n--;
}

// Calls fn(line, len) for each line of the body; stops when fn says so.
template <typename Fn>
void forEachLine(const char* body, size_t len, Fn fn) {
    size_t start = 0;
    for (size_t i = 0; i <= len; i++) {
        if (i == len || body[i] == '\n') {
            const char* p = body + start;
            size_t n = i - start;
            start = i + 1;
            trim(p, n);
            if (n == 0 || n > kLineMax) continue;
            if (!fn(p, n)) return;
        }
    }
}

bool lowerDigitUnderscore(char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
}

// ---- templates --------------------------------------------------------------
// The shared row macro (see the line format in the header). Names are cut
// to 48 characters and lose any '|' and line breaks on the server, so a
// row is always one line with exactly eight fields.
const char kRowMacro[] =
    "{%- macro row(s) -%}"
    "{%- set m = s.attributes.supported_color_modes or [] -%}"
    "{%- set col = m|select('in',['hs','rgb','rgbw','rgbww','xy'])|list|count > 0 -%}"
    "{%- set dim = col or m|select('in',['brightness','color_temp','white'])|list|count > 0 -%}"
    "{%- set c = s.attributes.rgb_color -%}"
    "{{ s.entity_id }}|"
    "{{ (s.name|string|replace('|','/')|replace('\\n',' ')|replace('\\r',' '))[:48] }}|"
    "{{ s.state }}|"
    "{{ s.attributes.brightness|int(0) }}|"
    "{{ ((c[0]|int)~','~(c[1]|int)~','~(c[2]|int)) if c else '' }}|"
    "{{ (1 if dim else 0) + (2 if col else 0) }}|"
    "{{ (s.attributes.effect_list or [])|count }}|"
    "{{ ((s.attributes.effect or '')|string|replace('|','/')|replace('\\n',' '))[:32] }}\n"
    "{% endmacro -%}";

const char kDomains[] = "['light','scene','script']";

// Wraps a template as {"template":"..."}. Returns the length or 0.
size_t wrapTemplate(const char* tmpl, char* out, size_t cap) {
    static const char kHead[] = "{\"template\":\"";
    static const char kTail[] = "\"}";
    const size_t head = sizeof(kHead) - 1;
    const size_t tail = sizeof(kTail) - 1;
    if (!out || cap < head + tail + 2) {
        if (out && cap) out[0] = '\0';
        return 0;
    }
    memcpy(out, kHead, head);
    const size_t body = jsonEscape(tmpl, out + head, cap - head - tail);
    if (body == 0 && tmpl[0] != '\0') { out[0] = '\0'; return 0; }
    memcpy(out + head + body, kTail, tail + 1);
    return head + body + tail;
}

constexpr size_t kTemplateMax = 1600;

size_t rowsBody(const char* selection, char* out, size_t cap) {
    char tmpl[kTemplateMax];
    const int n = snprintf(tmpl, sizeof(tmpl),
        "%s{%%- for s in (%s|selectattr('domain','in',%s)|sort(attribute='name')|list)[:%d] -%%}"
        "{{ row(s) }}{%%- endfor -%%}",
        kRowMacro, selection, kDomains, kMaxRows);
    if (n <= 0 || (size_t)n >= sizeof(tmpl)) { if (out && cap) out[0] = '\0'; return 0; }
    return wrapTemplate(tmpl, out, cap);
}

} // namespace

Kind kindOf(const char* id) {
    if (!id) return Kind::Other;
    if (strncmp(id, "light.", 6) == 0) return Kind::Light;
    if (strncmp(id, "scene.", 6) == 0) return Kind::Scene;
    if (strncmp(id, "script.", 7) == 0) return Kind::Script;
    return Kind::Other;
}

bool validEntityId(const char* id) {
    if (!id) return false;
    const size_t n = strlen(id);
    if (n < 3 || n > kIdMax) return false;
    int dots = 0;
    for (size_t i = 0; i < n; i++) {
        if (id[i] == '.') {
            if (i == 0 || i == n - 1) return false;
            dots++;
        } else if (!lowerDigitUnderscore(id[i])) {
            return false;
        }
    }
    return dots == 1;
}

bool validAreaId(const char* id) {
    if (!id) return false;
    const size_t n = strlen(id);
    if (n < 1 || n > kIdMax) return false;
    for (size_t i = 0; i < n; i++)
        if (!lowerDigitUnderscore(id[i])) return false;
    return true;
}

bool parseEntityLine(const char* line, size_t len, Entity& out) {
    if (!line) return false;
    trim(line, len);
    if (len == 0 || len > kLineMax) return false;
    Field f[kMaxFields];
    if (!split(line, len, f, 8)) return false;
    Entity e;
    if (f[0].n == 0 || f[0].n > kIdMax) return false;
    copyField(f[0], e.id, sizeof(e.id));
    if (!validEntityId(e.id)) return false;
    e.kind = kindOf(e.id);
    if (e.kind == Kind::Other) return false;
    copyField(f[1], e.name, sizeof(e.name));
    if (e.name[0] == '\0') copyField(f[0], e.name, sizeof(e.name));
    e.on = fieldIs(f[2], "on");
    e.available = !(fieldIs(f[2], "unavailable") || fieldIs(f[2], "unknown") || f[2].n == 0);
    if (!parseByte(f[3].p, f[3].n, e.brightness)) return false;
    if (!parseRgb(f[4], e)) return false;
    uint32_t caps = 0;
    if (!parseUint(f[5].p, f[5].n, 1, 3, caps)) return false;
    e.canDim = (caps & 1) != 0;
    e.canColor = (caps & 2) != 0;
    uint32_t effects = 0;
    if (!parseUint(f[6].p, f[6].n, 5, 99999, effects)) return false;
    e.effectCount = effects > 255 ? 255 : (uint8_t)effects;
    copyField(f[7], e.effect, sizeof(e.effect));
    if (!e.on) e.brightness = 0;
    out = e;
    return true;
}

bool parseAreaLine(const char* line, size_t len, Area& out) {
    if (!line) return false;
    trim(line, len);
    if (len == 0 || len > kLineMax) return false;
    Field f[kMaxFields];
    if (!split(line, len, f, 3)) return false;
    Area a;
    if (f[0].n == 0 || f[0].n > kIdMax) return false;
    copyField(f[0], a.id, sizeof(a.id));
    if (!validAreaId(a.id)) return false;
    copyField(f[1], a.name, sizeof(a.name));
    if (a.name[0] == '\0') copyField(f[0], a.name, sizeof(a.name));
    uint32_t count = 0;
    if (!parseUint(f[2].p, f[2].n, 5, 65535, count)) return false;
    a.count = (uint16_t)count;
    out = a;
    return true;
}

int parseEntities(const char* body, size_t len, Entity* out, int max) {
    int n = 0;
    if (!body || !out || max <= 0) return 0;
    forEachLine(body, len, [&](const char* p, size_t l) {
        if (parseEntityLine(p, l, out[n])) n++;
        return n < max;
    });
    return n;
}

int parseAreas(const char* body, size_t len, Area* out, int max) {
    int n = 0;
    if (!body || !out || max <= 0) return 0;
    forEachLine(body, len, [&](const char* p, size_t l) {
        if (parseAreaLine(p, l, out[n])) n++;
        return n < max;
    });
    return n;
}

int parseEffects(const char* body, size_t len, char (*out)[kEffectMax + 1], int max) {
    int n = 0;
    if (!body || !out || max <= 0) return 0;
    forEachLine(body, len, [&](const char* p, size_t l) {
        // A name too long to keep whole could not be sent back: skip it.
        if (l > kEffectMax) return true;
        memcpy(out[n], p, l);
        out[n][l] = '\0';
        n++;
        return n < max;
    });
    return n;
}

size_t jsonEscape(const char* in, char* out, size_t cap) {
    if (!out || cap == 0) return 0;
    out[0] = '\0';
    if (!in) return 0;
    size_t w = 0;
    for (const unsigned char* p = (const unsigned char*)in; *p; p++) {
        char esc[7];
        size_t n = 0;
        switch (*p) {
            case '"':  esc[0] = '\\'; esc[1] = '"';  n = 2; break;
            case '\\': esc[0] = '\\'; esc[1] = '\\'; n = 2; break;
            case '\n': esc[0] = '\\'; esc[1] = 'n';  n = 2; break;
            case '\r': esc[0] = '\\'; esc[1] = 'r';  n = 2; break;
            case '\t': esc[0] = '\\'; esc[1] = 't';  n = 2; break;
            default:
                if (*p < 0x20) {
                    snprintf(esc, sizeof(esc), "\\u%04x", (unsigned)*p);
                    n = 6;
                } else {
                    esc[0] = (char)*p;
                    n = 1;
                }
        }
        if (w + n >= cap) { out[0] = '\0'; return 0; }
        memcpy(out + w, esc, n);
        w += n;
    }
    out[w] = '\0';
    return w;
}

bool normalizeBaseUrl(const char* in, char* out, size_t cap) {
    if (!in || !out || cap == 0) return false;
    out[0] = '\0';
    size_t n = strlen(in);
    while (n && in[n - 1] == '/') n--;
    if (n > kUrlMax || n + 1 > cap) return false;
    static const char kScheme[] = "http://";
    const size_t schemeLen = sizeof(kScheme) - 1;
    if (n <= schemeLen) return false;
    for (size_t i = 0; i < schemeLen; i++) {
        char c = in[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c != kScheme[i]) return false;
    }
    if (in[schemeLen] == '/' || in[schemeLen] == ':') return false;   // no host
    for (size_t i = schemeLen; i < n; i++) {
        const unsigned char c = (unsigned char)in[i];
        if (c <= 0x20 || c >= 0x7f || c == '@' || c == '?' || c == '#' || c == '"' ||
            c == '\\' || c == '\'' || c == '<' || c == '>') return false;
    }
    memcpy(out, kScheme, schemeLen);
    memcpy(out + schemeLen, in + schemeLen, n - schemeLen);
    out[n] = '\0';
    return true;
}

bool validTokenChars(const char* token, size_t len) {
    if (!token || len == 0 || len > kTokenMax) return false;
    for (size_t i = 0; i < len; i++) {
        const unsigned char c = (unsigned char)token[i];
        if (c <= 0x20 || c >= 0x7f) return false;
    }
    return true;
}

size_t favoritesBody(char* out, size_t cap) {
    char selection[96];
    snprintf(selection, sizeof(selection),
             "states|selectattr('entity_id','in',label_entities('%s'))", kFavoriteLabel);
    return rowsBody(selection, out, cap);
}

size_t areasBody(char* out, size_t cap) {
    char tmpl[kTemplateMax];
    const int n = snprintf(tmpl, sizeof(tmpl),
        "{%%- for a in areas() -%%}"
        "{%%- set n = states|selectattr('entity_id','in',area_entities(a))"
        "|selectattr('domain','in',%s)|list|count -%%}"
        "{%%- if n > 0 -%%}"
        "{{ a }}|{{ (area_name(a)|string|replace('|','/')|replace('\\n',' '))[:48] }}|{{ n }}\n"
        "{%% endif -%%}"
        "{%%- endfor -%%}",
        kDomains);
    if (n <= 0 || (size_t)n >= sizeof(tmpl)) { if (out && cap) out[0] = '\0'; return 0; }
    return wrapTemplate(tmpl, out, cap);
}

size_t areaEntitiesBody(const char* areaId, char* out, size_t cap) {
    if (!validAreaId(areaId)) { if (out && cap) out[0] = '\0'; return 0; }
    char selection[kIdMax + 96];
    snprintf(selection, sizeof(selection),
             "states|selectattr('entity_id','in',area_entities('%s'))", areaId);
    return rowsBody(selection, out, cap);
}

size_t entityBody(const char* entityId, char* out, size_t cap) {
    if (!validEntityId(entityId)) { if (out && cap) out[0] = '\0'; return 0; }
    char selection[kIdMax + 64];
    snprintf(selection, sizeof(selection), "states|selectattr('entity_id','eq','%s')", entityId);
    return rowsBody(selection, out, cap);
}

size_t effectsBody(const char* entityId, char* out, size_t cap) {
    if (!validEntityId(entityId)) { if (out && cap) out[0] = '\0'; return 0; }
    char tmpl[512];
    const int n = snprintf(tmpl, sizeof(tmpl),
        "{%%- for e in (state_attr('%s','effect_list') or [])[:%d] -%%}"
        "{{ e|string|replace('|','/')|replace('\\n',' ') }}\n"
        "{%% endfor -%%}",
        entityId, kMaxEffects);
    if (n <= 0 || (size_t)n >= sizeof(tmpl)) { if (out && cap) out[0] = '\0'; return 0; }
    return wrapTemplate(tmpl, out, cap);
}

bool servicePath(const Action& a, char* out, size_t cap) {
    if (!out || cap == 0) return false;
    out[0] = '\0';
    if (!validEntityId(a.entity)) return false;
    const Kind kind = kindOf(a.entity);
    const char* domain = nullptr;
    const char* service = nullptr;
    if (kind == Kind::Light) {
        domain = "light";
        switch (a.type) {
            case Action::Type::Toggle:  service = "toggle"; break;
            case Action::Type::TurnOn:  service = "turn_on"; break;
            case Action::Type::TurnOff: service = "turn_off"; break;
            default: return false;
        }
    } else if (kind == Kind::Scene || kind == Kind::Script) {
        // A scene or script only runs (a long hold "toggles" it by running it).
        if (a.type != Action::Type::Run && a.type != Action::Type::Toggle &&
            a.type != Action::Type::TurnOn) return false;
        domain = kind == Kind::Scene ? "scene" : "script";
        service = "turn_on";
    } else {
        return false;
    }
    const int n = snprintf(out, cap, "/api/services/%s/%s", domain, service);
    if (n <= 0 || (size_t)n >= cap) { out[0] = '\0'; return false; }
    return true;
}

size_t serviceBody(const Action& a, char* out, size_t cap) {
    char path[48];
    if (!out || cap == 0) return 0;
    out[0] = '\0';
    if (!servicePath(a, path, sizeof(path))) return 0;
    int n = snprintf(out, cap, "{\"entity_id\":\"%s\"", a.entity);
    if (n <= 0 || (size_t)n >= cap) { out[0] = '\0'; return 0; }
    size_t w = (size_t)n;
    if (kindOf(a.entity) == Kind::Light && a.type == Action::Type::TurnOn) {
        if (a.setBrightness) {
            n = snprintf(out + w, cap - w, ",\"brightness\":%u", (unsigned)a.brightness);
            if (n <= 0 || (size_t)n >= cap - w) { out[0] = '\0'; return 0; }
            w += (size_t)n;
        }
        if (a.setRgb) {
            n = snprintf(out + w, cap - w, ",\"rgb_color\":[%u,%u,%u]",
                         (unsigned)a.r, (unsigned)a.g, (unsigned)a.b);
            if (n <= 0 || (size_t)n >= cap - w) { out[0] = '\0'; return 0; }
            w += (size_t)n;
        }
        if (a.setEffect) {
            char esc[kEffectMax * 6 + 1];
            if (a.effect[0] == '\0' || jsonEscape(a.effect, esc, sizeof(esc)) == 0) {
                out[0] = '\0';
                return 0;
            }
            n = snprintf(out + w, cap - w, ",\"effect\":\"%s\"", esc);
            if (n <= 0 || (size_t)n >= cap - w) { out[0] = '\0'; return 0; }
            w += (size_t)n;
        }
    }
    if (w + 2 > cap) { out[0] = '\0'; return 0; }
    out[w++] = '}';
    out[w] = '\0';
    return w;
}

Outcome classify(bool transportOk, int status) {
    if (!transportOk) return Outcome::Unreachable;
    if (status == 401 || status == 403) return Outcome::Rejected;
    if (status >= 200 && status < 300) return Outcome::Ok;
    return Outcome::BadReply;
}

void formatHex(uint8_t r, uint8_t g, uint8_t b, char out[8]) {
    static const char kHex[] = "0123456789ABCDEF";
    out[0] = '#';
    out[1] = kHex[r >> 4]; out[2] = kHex[r & 15];
    out[3] = kHex[g >> 4]; out[4] = kHex[g & 15];
    out[5] = kHex[b >> 4]; out[6] = kHex[b & 15];
    out[7] = '\0';
}

int brightnessPercent(uint8_t level) {
    return ((int)level * 100 + 127) / 255;
}

const Preset& preset(int index) {
    static const Preset kPresets[kPresetCount] = {
        {"Warm white", 255, 180, 107},
        {"Red", 255, 0, 0},
        {"Orange", 255, 110, 0},
        {"Yellow", 255, 220, 0},
        {"Green", 0, 255, 0},
        {"Cyan", 0, 255, 255},
        {"Blue", 0, 0, 255},
        {"Magenta", 255, 0, 255},
    };
    if (index < 0) index = 0;
    if (index >= kPresetCount) index = kPresetCount - 1;
    return kPresets[index];
}

// ---- ListCursor -------------------------------------------------------------

void ListCursor::reset(int count, int visibleRows) {
    count_ = count < 0 ? 0 : count;
    rows_ = visibleRows < 1 ? 1 : visibleRows;
    selected_ = 0;
    top_ = 0;
}

void ListCursor::setCount(int count) {
    count_ = count < 0 ? 0 : count;
    if (selected_ >= count_) selected_ = count_ > 0 ? count_ - 1 : 0;
    follow();
}

void ListCursor::up() {
    if (count_ <= 0) return;
    selected_ = selected_ > 0 ? selected_ - 1 : count_ - 1;
    follow();
}

void ListCursor::down() {
    if (count_ <= 0) return;
    selected_ = selected_ < count_ - 1 ? selected_ + 1 : 0;
    follow();
}

void ListCursor::follow() {
    if (selected_ < top_) top_ = selected_;
    if (selected_ >= top_ + rows_) top_ = selected_ - rows_ + 1;
    const int maxTop = count_ > rows_ ? count_ - rows_ : 0;
    if (top_ > maxTop) top_ = maxTop;
    if (top_ < 0) top_ = 0;
}

// ---- SoftTakeover -----------------------------------------------------------

namespace {
int sign(int v) { return v > 0 ? 1 : (v < 0 ? -1 : 0); }
int absi(int v) { return v < 0 ? -v : v; }
int clamp255(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }
}

void SoftTakeover::arm(int real, int slider) {
    real_ = clamp255(real);
    slider = clamp255(slider);
    lastSent_ = slider;
    side_ = sign(slider - real_);
    engaged_ = absi(slider - real_) <= kTolerance;
}

void SoftTakeover::setReal(int real, int slider) {
    real = clamp255(real);
    slider = clamp255(slider);
    if (engaged_ && absi(slider - real) <= kTolerance) {
        real_ = real;
        return;
    }
    // Not picked up (or someone else moved the value away from the
    // slider): the slider has to meet the new value first.
    arm(real, slider);
}

bool SoftTakeover::update(int slider, int* out) {
    slider = clamp255(slider);
    if (!engaged_) {
        const bool near = absi(slider - real_) <= kTolerance;
        const bool crossed = side_ != 0 && sign(slider - real_) != side_;
        if (!near && !crossed) return false;
        engaged_ = true;
        lastSent_ = real_;
    }
    const int moved = absi(slider - lastSent_);
    const bool atEnd = (slider == 0 || slider == 255) && slider != lastSent_;
    if (moved < kMinStep && !atEnd) return false;
    lastSent_ = slider;
    real_ = slider;
    if (out) *out = slider;
    return true;
}

// ---- SendGate ---------------------------------------------------------------

bool SendGate::ready(uint32_t nowMs, bool inFlight) const {
    if (!dirty_ || inFlight) return false;
    return !everSent_ || nowMs - lastMs_ >= kIntervalMs;
}

void SendGate::sent(uint32_t nowMs) {
    dirty_ = false;
    everSent_ = true;
    lastMs_ = nowMs;
}

// ---- PollGuard --------------------------------------------------------------

void PollGuard::reset() {
    for (int i = 0; i < kControls; i++) { touched_[i] = false; at_[i] = 0; }
}

void PollGuard::touched(Control c, uint32_t nowMs) {
    if (c >= kControls) return;
    touched_[c] = true;
    at_[c] = nowMs;
}

bool PollGuard::accepts(Control c, uint32_t nowMs) const {
    if (c >= kControls) return true;
    return !touched_[c] || nowMs - at_[c] >= kHoldMs;
}

// ---- RepeatAccel ------------------------------------------------------------

void RepeatAccel::press(uint32_t nowMs) {
    active_ = true;
    startMs_ = nowMs;
    lastMs_ = nowMs;
}

int RepeatAccel::stepSize(uint32_t heldMs) {
    if (heldMs < 1000) return 1;
    if (heldMs < 2000) return 5;
    return 15;
}

int RepeatAccel::steps(uint32_t nowMs) {
    if (!active_) return 0;
    const uint32_t held = nowMs - startMs_;
    if (held < kDelayMs) return 0;
    // The first repeat is due as soon as the delay has passed.
    if (lastMs_ != startMs_ && nowMs - lastMs_ < kEveryMs) return 0;
    lastMs_ = nowMs;
    return stepSize(held);
}

} // namespace HomeRemote
