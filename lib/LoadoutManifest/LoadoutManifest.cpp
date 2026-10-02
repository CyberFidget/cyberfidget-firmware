// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

// lib/LoadoutManifest/LoadoutManifest.cpp — see LoadoutManifest.h for the
// API contract and README.md for the schema. Pure C++17, no Arduino.
//
// The JSON parser below is hand-rolled on purpose: the manifest is a small
// closed schema and skipping ArduinoJson keeps this core buildable for
// native tests and the WASM emulator without any dependencies.

#include "LoadoutManifest.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <limits.h>
#include <map>
#include <stdlib.h>

namespace LoadoutManifest {

// ============================================================
// Minimal JSON parser (objects, arrays, strings, ints, bools,
// null). Unknown values are skipped recursively.
// ============================================================
namespace {

struct Cursor {
    const char* p;
};

void skipWs(Cursor& c) {
    while (*c.p == ' ' || *c.p == '\t' || *c.p == '\n' || *c.p == '\r') c.p++;
}

// Parse a JSON string (cursor on the opening quote) into `out`.
bool parseString(Cursor& c, std::string& out) {
    if (*c.p != '"') return false;
    c.p++;
    out.clear();
    while (*c.p && *c.p != '"') {
        if (*c.p == '\\') {
            c.p++;
            switch (*c.p) {
                case '"':  out += '"';  break;
                case '\\': out += '\\'; break;
                case '/':  out += '/';  break;
                case 'b':  out += '\b'; break;
                case 'f':  out += '\f'; break;
                case 'n':  out += '\n'; break;
                case 'r':  out += '\r'; break;
                case 't':  out += '\t'; break;
                case 'u': {
                    // \uXXXX — decode to UTF-8. Menu labels are effectively
                    // ASCII today; anything outside the BMP is out of scope.
                    unsigned int cp = 0;
                    for (int i = 0; i < 4; i++) {
                        c.p++;
                        char h = *c.p;
                        if      (h >= '0' && h <= '9') cp = (cp << 4) | (unsigned)(h - '0');
                        else if (h >= 'a' && h <= 'f') cp = (cp << 4) | (unsigned)(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') cp = (cp << 4) | (unsigned)(h - 'A' + 10);
                        else return false;
                    }
                    if (cp < 0x80) {
                        out += (char)cp;
                    } else if (cp < 0x800) {
                        out += (char)(0xC0 | (cp >> 6));
                        out += (char)(0x80 | (cp & 0x3F));
                    } else {
                        out += (char)(0xE0 | (cp >> 12));
                        out += (char)(0x80 | ((cp >> 6) & 0x3F));
                        out += (char)(0x80 | (cp & 0x3F));
                    }
                    break;
                }
                default: return false; // invalid escape
            }
            c.p++;
        } else {
            out += *c.p;
            c.p++;
        }
    }
    if (*c.p != '"') return false; // unterminated
    c.p++;
    return true;
}

// Parse an integer (the only number type the schema uses).
bool parseInt(Cursor& c, long& out) {
    const char* start = c.p;
    if (*c.p == '-') c.p++;
    if (*c.p < '0' || *c.p > '9') return false;
    long v = 0;
    bool neg = (*start == '-');
    while (*c.p >= '0' && *c.p <= '9') {
        v = v * 10 + (*c.p - '0');
        c.p++;
    }
    // Reject floats/exponents in known int fields.
    if (*c.p == '.' || *c.p == 'e' || *c.p == 'E') return false;
    out = neg ? -v : v;
    return true;
}

bool parseBool(Cursor& c, bool& out) {
    if (std::strncmp(c.p, "true", 4) == 0)  { out = true;  c.p += 4; return true; }
    if (std::strncmp(c.p, "false", 5) == 0) { out = false; c.p += 5; return true; }
    return false;
}

// Deepest object/array nesting skipValue will descend into before failing
// the parse. The manifest and ops schemas nest only a couple of levels, so
// 16 is generous headroom; the cap exists purely to keep a hostile document
// (thousands of nested {/[ in an unknown field, reachable from the wire via
// lapply/parseManifest) from overflowing the task stack via recursion.
constexpr int kMaxSkipDepth = 16;

// Skip any JSON value (for unknown fields — forward compatibility). `depth`
// counts nested containers; exceeding kMaxSkipDepth fails the parse (and the
// whole document rolls back) rather than recursing without bound.
bool skipValue(Cursor& c, int depth) {
    if (depth > kMaxSkipDepth) return false;
    skipWs(c);
    if (*c.p == '"') {
        std::string ignored;
        return parseString(c, ignored);
    }
    if (*c.p == '{' || *c.p == '[') {
        char open  = *c.p;
        char close = (open == '{') ? '}' : ']';
        c.p++;
        skipWs(c);
        if (*c.p == close) { c.p++; return true; }
        while (true) {
            if (open == '{') {
                std::string ignoredKey;
                skipWs(c);
                if (!parseString(c, ignoredKey)) return false;
                skipWs(c);
                if (*c.p != ':') return false;
                c.p++;
            }
            if (!skipValue(c, depth + 1)) return false;
            skipWs(c);
            if (*c.p == ',') { c.p++; continue; }
            if (*c.p == close) { c.p++; return true; }
            return false;
        }
    }
    if (std::strncmp(c.p, "true", 4) == 0)  { c.p += 4; return true; }
    if (std::strncmp(c.p, "false", 5) == 0) { c.p += 5; return true; }
    if (std::strncmp(c.p, "null", 4) == 0)  { c.p += 4; return true; }
    // number
    if (*c.p == '-' || (*c.p >= '0' && *c.p <= '9')) {
        c.p++;
        while ((*c.p >= '0' && *c.p <= '9') || *c.p == '.' ||
               *c.p == 'e' || *c.p == 'E' || *c.p == '+' || *c.p == '-') {
            c.p++;
        }
        return true;
    }
    return false;
}

// Parse one entry object into `entry`. `positionSeen` reports whether the
// document carried an explicit position (else the caller uses array order).
bool parseEntry(Cursor& c, LoadoutEntry& entry, bool& positionSeen) {
    positionSeen = false;
    skipWs(c);
    if (*c.p != '{') return false;
    c.p++;
    skipWs(c);
    if (*c.p == '}') { c.p++; return true; }
    while (true) {
        skipWs(c);
        std::string key;
        if (!parseString(c, key)) return false;
        skipWs(c);
        if (*c.p != ':') return false;
        c.p++;
        skipWs(c);

        if      (key == "id")        { if (!parseString(c, entry.id))        return false; }
        else if (key == "name")      { if (!parseString(c, entry.name))      return false; }
        else if (key == "category")  { if (!parseString(c, entry.category))  return false; }
        else if (key == "hidden")    { if (!parseBool(c, entry.hidden))      return false; }
        else if (key == "position")  {
            long v;
            if (!parseInt(c, v)) return false;
            entry.position = (int)v;
            positionSeen = true;
        }
        else if (key == "format")    { if (!parseString(c, entry.format))    return false; }
        else if (key == "blobPath")  { if (!parseString(c, entry.blobPath))  return false; }
        else if (key == "version")   { if (!parseString(c, entry.version))   return false; }
        else if (key == "abi")       { if (!parseString(c, entry.abi))       return false; }
        else if (key == "signature") { if (!parseString(c, entry.signature)) return false; }
        else                         { if (!skipValue(c, 0))                 return false; }

        skipWs(c);
        if (*c.p == ',') { c.p++; continue; }
        if (*c.p == '}') { c.p++; return true; }
        return false;
    }
}

// Renumber positions to match array order (canonical form).
void renumber(Loadout& loadout) {
    for (int i = 0; i < (int)loadout.entries.size(); i++) {
        loadout.entries[i].position = i;
    }
}

// Submenu levels the arrange normalization looks at. Deeper segments are
// ignored for ordering only (entries that share their first
// kMaxOrderDepth segments keep their relative order); the stored category
// string is never changed. Bounds the work a hostile path can cause.
constexpr size_t kMaxOrderDepth = 8;

// Split a category path on '/', skipping empty segments - the same rule
// the device menu uses to build submenus (MenuManager's CategoryPath.h) -
// keeping at most kMaxOrderDepth segments.
std::vector<std::string> splitPath(const std::string& path) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start < path.size() && out.size() < kMaxOrderDepth) {
        size_t slash = path.find('/', start);
        if (slash == std::string::npos) slash = path.size();
        if (slash > start) out.push_back(path.substr(start, slash - start));
        start = slash + 1;
    }
    return out;
}

// The merge's keep/drop rule, per manifest entry: the registry index a
// built-in merges to (>= 0), kMergedBlob for a delivered app that is kept,
// or kMergedDropped for a row the menu never shows (stale id, duplicate,
// non-menu slot, delivered app without a file). mergeWithRegistry and the
// arrange normalization both use it, so they agree on what is on screen.
constexpr int kMergedBlob    = -1;
constexpr int kMergedDropped = -2;
std::vector<int> mergeRows(const Loadout& loadout, const RegistryApp* apps, int count) {
    std::vector<int> rows(loadout.entries.size(), kMergedDropped);
    std::vector<bool> used(count > 0 ? (size_t)count : 0, false);
    for (size_t k = 0; k < loadout.entries.size(); k++) {
        const LoadoutEntry& entry = loadout.entries[k];
        // A ferried app has no compile-time registry row; it launches from
        // its file. The producer of truth is the website sync, which stamps
        // format "wasm" (the honest module format); "blob" is also accepted.
        // The load-bearing signal is a NON-builtin format with a blobPath -
        // that identifies an app sent from the website and launched from a file.
        // Guard: no path = unlaunchable, dropped like a stale id.
        if (entry.format != "builtin" && !entry.format.empty()) {
            if (!entry.blobPath.empty()) rows[k] = kMergedBlob;
            continue;
        }
        int idx = -1;
        for (int i = 0; i < count; i++) {
            if (apps[i].id == entry.id) { idx = i; break; }
        }
        if (idx < 0 || used[(size_t)idx]) continue;
        if (apps[idx].name.empty()) continue; // not a menu item
        used[(size_t)idx] = true;
        rows[k] = idx;
    }
    return rows;
}

// The compiled apps the merge appends because the manifest does not list
// them (no kept row in `rows`, from mergeRows), in compile order. Menu
// slots without a name are never apps.
std::vector<int> unlistedApps(const std::vector<int>& rows, const RegistryApp* apps, int count) {
    std::vector<bool> used(count > 0 ? (size_t)count : 0, false);
    for (int r : rows) if (r >= 0) used[(size_t)r] = true;
    std::vector<int> out;
    for (int i = 0; i < count; i++) {
        if (used[(size_t)i] || apps[i].name.empty()) continue;
        out.push_back(i);
    }
    return out;
}

// The first segment of a category path: the placement the merge gives a
// fallback ("Tools" for "Tools/LEDs").
std::string topSegment(const std::string& path) {
    return path.substr(0, path.find('/'));
}

// The category the merge renders for a kept row (`row` from mergeRows): a
// built-in with an empty category falls back to the first segment of its
// compiled path; otherwise the stored category, as is.
std::string renderedCategory(const LoadoutEntry& entry, int row, const RegistryApp* apps) {
    if (row >= 0 && entry.category.empty()) return topSegment(apps[row].category);
    return entry.category;
}

// Reorder entries into the order of the menu the device displays, so the
// flat list is a depth-first walk of that menu. Every ordering decision
// comes from the merge's view (mergeRows + renderedCategory):
//  - a row is SHOWN when the merge keeps it and it is not hidden;
//  - shown rows are placed by the category the merge renders for them;
//  - at every level (root included) a folder sits where its first shown
//    row is and an app keeps its own place, which is exactly how the
//    device builder (registerApp / findOrCreateCategory) orders a level;
//  - rows the device does not show (hidden, pruned) never anchor anything
//    while their folder has a shown row: each keeps its input position
//    among the rows of its own folder, so it rides along without moving
//    anything visible. A folder with no shown row sits at its first row.
// Without a registry, every non-hidden row counts as shown and is placed by
// its stored category (the caller has no merge view to offer).
// With single-level categories and every row shown this is "each category
// is one contiguous run, relative order kept" (the earlier rule), except
// that root entries ("") keep their own places among the folders instead of
// being gathered into one run - the device shows them that way.
// Only the first kMaxOrderDepth path levels are used; no recursion.
void normalizeSections(Loadout& loadout, const RegistryApp* apps, int count) {
    const size_t n = loadout.entries.size();
    const bool haveRegistry = apps && count > 0;
    std::vector<int> rows;
    if (haveRegistry) rows = mergeRows(loadout, apps, count);

    std::vector<std::vector<std::string>> segs(n);
    std::vector<bool> shown(n);
    for (size_t i = 0; i < n; i++) {
        const LoadoutEntry& e = loadout.entries[i];
        const bool kept = !haveRegistry || rows[i] != kMergedDropped;
        shown[i] = kept && !e.hidden;
        segs[i] = splitPath(haveRegistry && kept ? renderedCategory(e, rows[i], apps)
                                                 : e.category);
    }

    // A row's own place is its input index: shown rows keep their order,
    // and a row the device does not show stays after the shown row before
    // it within its own folder.

    // Folder anchors, keyed by the joined path ("Tools", "Tools/LEDs"): the
    // first shown row inside; for a folder with no shown row, the first
    // row inside.
    std::map<std::string, size_t> shownAnchor, otherAnchor;
    for (size_t i = 0; i < n; i++) {
        std::string prefix;
        for (size_t d = 0; d < segs[i].size(); d++) {
            if (d) prefix += '/';
            prefix += segs[i][d];
            (shown[i] ? shownAnchor : otherAnchor).emplace(prefix, i);
        }
    }

    // Sort key: the anchor of each enclosing folder, outermost first, then
    // the row's own place value.
    std::vector<std::vector<size_t>> key(n);
    for (size_t i = 0; i < n; i++) {
        std::string prefix;
        for (size_t d = 0; d < segs[i].size(); d++) {
            if (d) prefix += '/';
            prefix += segs[i][d];
            auto s = shownAnchor.find(prefix);
            key[i].push_back(s != shownAnchor.end() ? s->second : otherAnchor[prefix]);
        }
        key[i].push_back(i);
    }

    std::vector<size_t> order(n);
    for (size_t i = 0; i < n; i++) order[i] = i;
    std::stable_sort(order.begin(), order.end(),
                     [&](size_t a, size_t b) { return key[a] < key[b]; });

    std::vector<LoadoutEntry> result;
    result.reserve(n);
    for (size_t i : order) result.push_back(std::move(loadout.entries[i]));
    loadout.entries = std::move(result);
    renumber(loadout);
}

int findEntry(const Loadout& loadout, const char* id) {
    for (int i = 0; i < (int)loadout.entries.size(); i++) {
        if (loadout.entries[i].id == id) return i;
    }
    return -1;
}

// JSON string escaping for the serializer.
void appendEscaped(std::string& out, const std::string& s) {
    out += '"';
    for (char ch : s) {
        switch (ch) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if ((unsigned char)ch < 0x20) {
                    static const char hex[] = "0123456789abcdef";
                    out += "\\u00";
                    out += hex[((unsigned char)ch >> 4) & 0xF];
                    out += hex[(unsigned char)ch & 0xF];
                } else {
                    out += ch;
                }
        }
    }
    out += '"';
}

void appendStringField(std::string& out, const char* key, const std::string& value) {
    out += "      \"";
    out += key;
    out += "\": ";
    appendEscaped(out, value);
}

} // namespace

// ============================================================
// Public API
// ============================================================

std::string slugifyBuiltinName(const char* name) {
    std::string slug;
    if (!name) return slug;
    slug.reserve(std::strlen(name));
    bool separatorPending = false;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(name); *p; ++p) {
        if (std::isalnum(*p)) {
            if (separatorPending && !slug.empty()) slug += '-';
            slug += static_cast<char>(std::tolower(*p));
            separatorPending = false;
        } else if (!slug.empty()) {
            separatorPending = true;
        }
    }
    return slug;
}

bool normalizeBuiltinIds(Loadout& loadout, const RegistryApp* apps, int count) {
    if (!apps || count <= 0) return false;
    bool changed = false;
    for (int app = 0; app < count; ++app) {
        if (apps[app].legacyId.empty() || apps[app].id.empty()) continue;
        while (true) {
            int legacy = -1, canonical = -1;
            for (int i = 0; i < (int)loadout.entries.size(); ++i) {
                if (legacy < 0 && loadout.entries[i].id == apps[app].legacyId) legacy = i;
                if (canonical < 0 && loadout.entries[i].id == apps[app].id) canonical = i;
            }
            if (legacy < 0) break;
            if (canonical >= 0) {
                if (legacy < canonical) {
                    LoadoutEntry existing = std::move(loadout.entries[(size_t)canonical]);
                    loadout.entries.erase(loadout.entries.begin() + canonical);
                    loadout.entries[(size_t)legacy] = std::move(existing);
                } else {
                    loadout.entries.erase(loadout.entries.begin() + legacy);
                }
            } else {
                loadout.entries[(size_t)legacy].id = apps[app].id;
                loadout.entries[(size_t)legacy].format = "builtin";
            }
            changed = true;
        }
    }
    if (changed) renumber(loadout);
    return changed;
}

bool parseManifest(const char* json, Loadout& out) {
    if (!json) return false;
    Cursor c{json};
    Loadout parsed;
    parsed.schemaVersion = -1; // must be present in the document

    skipWs(c);
    if (*c.p != '{') return false;
    c.p++;
    skipWs(c);

    bool hasPositions = true; // all entries carried explicit positions
    if (*c.p != '}') {
        while (true) {
            skipWs(c);
            std::string key;
            if (!parseString(c, key)) return false;
            skipWs(c);
            if (*c.p != ':') return false;
            c.p++;
            skipWs(c);

            if (key == "schemaVersion") {
                long v;
                if (!parseInt(c, v)) return false;
                parsed.schemaVersion = (int)v;
            } else if (key == "entries") {
                if (*c.p != '[') return false;
                c.p++;
                skipWs(c);
                if (*c.p != ']') {
                    while (true) {
                        LoadoutEntry entry;
                        bool positionSeen;
                        if (!parseEntry(c, entry, positionSeen)) return false;
                        if (!positionSeen) {
                            entry.position = (int)parsed.entries.size();
                            hasPositions = false;
                        }
                        // Entries without an id can't be matched to an app:
                        // drop them (prune-not-fatal).
                        if (!entry.id.empty()) parsed.entries.push_back(entry);
                        skipWs(c);
                        if (*c.p == ',') { c.p++; continue; }
                        if (*c.p == ']') { c.p++; break; }
                        return false;
                    }
                } else {
                    c.p++;
                }
            } else {
                if (!skipValue(c, 0)) return false;
            }

            skipWs(c);
            if (*c.p == ',') { c.p++; continue; }
            if (*c.p == '}') { c.p++; break; }
            return false;
        }
    } else {
        c.p++;
    }
    skipWs(c);
    if (*c.p != '\0') return false; // trailing garbage

    // No legacy readers, no migration shims: refuse anything that isn't
    // exactly the schema version we write. Caller falls back to compile
    // order (today's behavior).
    if (parsed.schemaVersion != kSchemaVersion) return false;

    if (hasPositions) {
        std::stable_sort(parsed.entries.begin(), parsed.entries.end(),
                         [](const LoadoutEntry& a, const LoadoutEntry& b) {
                             return a.position < b.position;
                         });
    }
    renumber(parsed);
    out = std::move(parsed);
    return true;
}

std::string serializeManifest(const Loadout& loadout) {
    std::string out;
    out.reserve(128 + loadout.entries.size() * 128);
    out += "{\n  \"schemaVersion\": ";
    out += std::to_string(loadout.schemaVersion);
    out += ",\n  \"entries\": [";
    for (size_t i = 0; i < loadout.entries.size(); i++) {
        const LoadoutEntry& e = loadout.entries[i];
        out += (i == 0) ? "\n" : ",\n";
        out += "    {\n";
        appendStringField(out, "id", e.id);         out += ",\n";
        appendStringField(out, "name", e.name);     out += ",\n";
        appendStringField(out, "category", e.category); out += ",\n";
        out += "      \"position\": ";
        out += std::to_string((int)i);
        out += ",\n      \"hidden\": ";
        out += e.hidden ? "true" : "false";
        // Reserved fields: only written when set, so a stock manifest
        // stays small and readable.
        if (!e.format.empty())    { out += ",\n"; appendStringField(out, "format",    e.format);    }
        if (!e.blobPath.empty())  { out += ",\n"; appendStringField(out, "blobPath",  e.blobPath);  }
        if (!e.version.empty())   { out += ",\n"; appendStringField(out, "version",   e.version);   }
        if (!e.abi.empty())       { out += ",\n"; appendStringField(out, "abi",       e.abi);       }
        if (!e.signature.empty()) { out += ",\n"; appendStringField(out, "signature", e.signature); }
        out += "\n    }";
    }
    out += "\n  ]\n}\n";
    return out;
}

Loadout buildFromRegistry(const RegistryApp* apps, int count) {
    Loadout loadout;
    if (!apps || count <= 0) return loadout;
    for (int i = 0; i < count; i++) {
        if (apps[i].name.empty()) continue; // not a menu item (e.g. the menu app)
        LoadoutEntry e;
        e.id       = apps[i].id;
        e.name     = apps[i].name;
        e.category = apps[i].category;
        e.format   = "builtin";
        loadout.entries.push_back(e);
    }
    renumber(loadout);
    return loadout;
}

std::vector<int> compiledMenuRows(const RegistryApp* apps, int count) {
    std::vector<int> rows;
    if (!apps || count <= 0) return rows;
    for (int i = 0; i < count; i++) {
        if (apps[i].name.empty()) continue; // internal slot, not a menu item
        rows.push_back(i);
    }
    return rows;
}

Loadout buildBuiltinReport(const RegistryApp* apps,
                           const char* const* categoryPaths, int count) {
    Loadout report;
    for (int i : compiledMenuRows(apps, count)) {
        LoadoutEntry e;
        e.id       = slugifyBuiltinName(apps[i].name.c_str());
        e.name     = apps[i].name;
        const char* path = categoryPaths ? categoryPaths[i] : nullptr;
        e.category = path ? path : "";
        e.format   = "builtin";
        report.entries.push_back(e);
    }
    renumber(report);
    return report;
}

std::vector<MergedApp> mergeWithRegistry(const Loadout& loadout,
                                         const RegistryApp* apps, int count) {
    std::vector<MergedApp> out;
    if (!apps || count <= 0) return out;

    // Fallbacks (an entry with an empty category, an app the manifest does
    // not list) use the first segment of the compiled path, as released
    // firmware always has: "Tools" for an app compiled under "Tools/LEDs".
    // Nested paths come only from what the file stores (a fresh seed, an
    // arrange). A new nested app therefore first shows in its top-level
    // category until the user arranges it.
    // (renderedCategory / topSegment are shared with the arrange
    // normalization, so both agree on where a row is shown.)

    // 1) Manifest entries, in manifest order. Stale ids (no registry
    //    match), duplicates and unlaunchable delivered apps are pruned,
    //    not fatal (mergeRows).
    const std::vector<int> rows = mergeRows(loadout, apps, count);
    for (size_t k = 0; k < loadout.entries.size(); k++) {
        const LoadoutEntry& entry = loadout.entries[k];
        if (rows[k] == kMergedDropped) continue;
        if (rows[k] == kMergedBlob) {
            MergedApp m;
            m.appIndex = -1;
            m.category = entry.category;
            m.hidden   = entry.hidden;
            m.label    = entry.name.empty() ? entry.id : entry.name;
            m.blobPath = entry.blobPath;
            m.abi      = parseAbiVersion(entry.abi);
            m.id       = entry.id;
            out.push_back(m);
            continue;
        }
        const int idx = rows[k];
        MergedApp m;
        m.appIndex = idx;
        m.category = renderedCategory(entry, idx, apps);
        m.hidden   = entry.hidden;
        out.push_back(m);
    }

    // 2) Compiled-in apps absent from the manifest: append in compile
    //    order so new apps appear after a firmware update. (The menu
    //    tree groups by category name, so they still render inside
    //    their category; the flat order becomes contiguous again the
    //    next time the manifest is rewritten.)
    for (int i : unlistedApps(rows, apps, count)) {
        MergedApp m;
        m.appIndex = i;
        m.category = topSegment(apps[i].category);
        m.hidden   = false;
        out.push_back(m);
    }
    return out;
}

int parseAbiVersion(const std::string& abi) {
    if (abi.empty()) return 0;
    char* end = nullptr;
    long value = strtol(abi.c_str(), &end, 10);
    if (end == abi.c_str() || *end != '\0' || value < 0 || value > INT_MAX) return 0;
    return (int)value;
}

bool applyAdd(Loadout& loadout, const LoadoutEntry& entry) {
    if (entry.id.empty()) return false;
    if (findEntry(loadout, entry.id.c_str()) >= 0) return false; // duplicate
    // Insert at the end of the entry's category section so contiguity
    // holds by construction; unknown categories start a new section at
    // the end.
    int insertAt = (int)loadout.entries.size();
    for (int i = (int)loadout.entries.size() - 1; i >= 0; i--) {
        if (loadout.entries[i].category == entry.category) {
            insertAt = i + 1;
            break;
        }
    }
    loadout.entries.insert(loadout.entries.begin() + insertAt, entry);
    renumber(loadout);
    return true;
}

bool applyRemove(Loadout& loadout, const char* id) {
    if (!id) return false;
    int idx = findEntry(loadout, id);
    if (idx < 0) return false;
    loadout.entries.erase(loadout.entries.begin() + idx);
    renumber(loadout);
    return true;
}

std::vector<std::string> removeNonBuiltin(Loadout& loadout) {
    std::vector<std::string> paths;
    std::vector<LoadoutEntry> kept;
    for (const auto& entry : loadout.entries) {
        if (entry.format.empty() || entry.format == "builtin" || entry.blobPath.empty())
            kept.push_back(entry);
        else paths.push_back(entry.blobPath);
    }
    loadout.entries = std::move(kept);
    renumber(loadout);
    return paths;
}

bool applyHide(Loadout& loadout, const char* id, bool hidden) {
    if (!id) return false;
    int idx = findEntry(loadout, id);
    if (idx < 0) return false;
    loadout.entries[idx].hidden = hidden;
    return true;
}

// ============================================================
// Staged-ops document apply (serial sync write direction). Parses one op
// object at the cursor and applies it to `work`, reusing the same
// apply primitives the on-device reorder uses. Returns false on malformed
// JSON or a rejected op (kept in the anonymous namespace with the other
// parse helpers so it can reach Cursor / parseEntry / findEntry).
// ============================================================
namespace {

// Parse an `order` array ([{ "id":.., "category"?:.. }, ...]) at the cursor
// into `order`. Cursor must be on the opening '['.
bool parseArrangeArray(Cursor& c, std::vector<ArrangeItem>& order) {
    skipWs(c);
    if (*c.p != '[') return false;
    c.p++;
    skipWs(c);
    if (*c.p == ']') { c.p++; return true; }
    while (true) {
        skipWs(c);
        if (*c.p != '{') return false;
        c.p++;
        ArrangeItem item;
        skipWs(c);
        if (*c.p != '}') {
            while (true) {
                skipWs(c);
                std::string key;
                if (!parseString(c, key)) return false;
                skipWs(c);
                if (*c.p != ':') return false;
                c.p++;
                skipWs(c);
                if (key == "id") {
                    if (!parseString(c, item.id)) return false;
                } else if (key == "category") {
                    if (!parseString(c, item.category)) return false;
                    item.hasCategory = true;
                } else {
                    if (!skipValue(c, 0)) return false;
                }
                skipWs(c);
                if (*c.p == ',') { c.p++; continue; }
                if (*c.p == '}') { c.p++; break; }
                return false;
            }
        } else {
            c.p++;
        }
        if (!item.id.empty()) order.push_back(item);
        skipWs(c);
        if (*c.p == ',') { c.p++; continue; }
        if (*c.p == ']') { c.p++; break; }
        return false;
    }
    return true;
}

// A `batch` id: 1..kMaxBatchIdLen printable ASCII bytes (no space/control).
bool validBatchId(const std::string& s) {
    if (s.empty() || s.size() > kMaxBatchIdLen) return false;
    for (char ch : s) {
        unsigned char u = (unsigned char)ch;
        if (u < 0x21 || u > 0x7E) return false;
    }
    return true;
}

// A `base` value: 1..8 hex digits, either case.
bool parseBaseHex(const std::string& s, uint32_t& out) {
    if (s.empty() || s.size() > 8) return false;
    uint32_t v = 0;
    for (char h : s) {
        if      (h >= '0' && h <= '9') v = (v << 4) | (uint32_t)(h - '0');
        else if (h >= 'a' && h <= 'f') v = (v << 4) | (uint32_t)(h - 'a' + 10);
        else if (h >= 'A' && h <= 'F') v = (v << 4) | (uint32_t)(h - 'A' + 10);
        else return false;
    }
    out = v;
    return true;
}

// Parse the value of a top-level `batch` or `base` key (cursor on the value)
// into `meta`. An invalid value fails the whole document.
bool parseMetaField(Cursor& c, const std::string& key, OpsMeta& meta) {
    std::string value;
    if (!parseString(c, value)) return false;
    if (key == "batch") {
        if (!validBatchId(value)) return false;
        meta.batch = value;
        meta.hasBatch = true;
        return true;
    }
    if (!parseBaseHex(value, meta.base)) return false;
    meta.hasBase = true;
    return true;
}

// Parse and apply one op object (cursor on its opening '{') to `work`.
bool applyOneOp(Cursor& c, Loadout& work, int& applied,
                const RegistryApp* apps, int count) {
    skipWs(c);
    if (*c.p != '{') return false;
    c.p++;

    std::string opType;
    std::string id;
    LoadoutEntry entry;
    bool hasEntry = false;
    bool hidden = false;
    std::vector<ArrangeItem> order;
    bool hasOrder = false;

    skipWs(c);
    if (*c.p != '}') {
        while (true) {
            skipWs(c);
            std::string key;
            if (!parseString(c, key)) return false;
            skipWs(c);
            if (*c.p != ':') return false;
            c.p++;
            skipWs(c);
            if (key == "op") {
                if (!parseString(c, opType)) return false;
            } else if (key == "id") {
                if (!parseString(c, id)) return false;
            } else if (key == "hidden") {
                if (!parseBool(c, hidden)) return false;
            } else if (key == "entry") {
                bool positionSeen;
                if (!parseEntry(c, entry, positionSeen)) return false;
                hasEntry = true;
            } else if (key == "order") {
                if (!parseArrangeArray(c, order)) return false;
                hasOrder = true;
            } else {
                if (!skipValue(c, 0)) return false;
            }
            skipWs(c);
            if (*c.p == ',') { c.p++; continue; }
            if (*c.p == '}') { c.p++; break; }
            return false;
        }
    } else {
        c.p++;
    }

    // Dispatch on the op discriminator. A rejected op fails the whole
    // document (the caller left the real loadout untouched).
    if (opType == "add") {
        if (!hasEntry) return false;
        if (!applyAdd(work, entry)) return false;
    } else if (opType == "remove") {
        if (id.empty()) return false;
        if (!applyRemove(work, id.c_str())) return false;
    } else if (opType == "hide") {
        if (id.empty()) return false;
        if (!applyHide(work, id.c_str(), hidden)) return false;
    } else if (opType == "arrange") {
        if (!hasOrder) return false;
        if (!applyArrange(work, order, apps, count)) return false;
    } else if (opType == "replace") {
        if (!hasEntry) return false;
        if (!applyReplace(work, entry)) return false;
    } else {
        return false; // unknown / missing op discriminator
    }
    applied++;
    return true;
}

} // namespace

bool applyArrange(Loadout& loadout, const std::vector<ArrangeItem>& order,
                  const RegistryApp* apps, int count) {
    // With a registry, first write down the compiled apps the file does not
    // list, exactly as the merge appends them (compile order, first-segment
    // category, shown). The device menu already shows them there, so this
    // changes nothing visible, and the ordering below then sees the same
    // menu the device builds.
    if (apps && count > 0) {
        for (int i : unlistedApps(mergeRows(loadout, apps, count), apps, count)) {
            LoadoutEntry e;
            e.id       = apps[i].id;
            e.name     = apps[i].name;
            e.category = topSegment(apps[i].category);
            e.format   = "builtin";
            loadout.entries.push_back(e);
        }
    }

    // Drop later built-in rows that repeat an earlier built-in id. The
    // merge only ever uses the first one (mergeRows), so this is invisible
    // on the device, and it stops the normalization below from moving a
    // duplicate ahead of the row the merge currently uses (which would
    // change which one wins, e.g. bring a hidden copy to the front).
    // Delivered apps are left alone: the merge keeps every one with a file.
    {
        std::vector<LoadoutEntry> unique;
        unique.reserve(loadout.entries.size());
        std::vector<std::string> seen;
        for (auto& e : loadout.entries) {
            const bool builtin = e.format == "builtin" || e.format.empty();
            if (builtin) {
                if (std::find(seen.begin(), seen.end(), e.id) != seen.end()) continue;
                seen.push_back(e.id);
            }
            unique.push_back(std::move(e));
        }
        loadout.entries = std::move(unique);
    }
    // Put the list in displayed-menu order first, so the entries this
    // arrange does not name keep their displayed relative order (a stored
    // order that is not normalized could otherwise regroup them).
    normalizeSections(loadout, apps, count);

    std::vector<LoadoutEntry> arranged;
    arranged.reserve(loadout.entries.size());
    std::vector<bool> taken(loadout.entries.size(), false);

    // Pull entries into the requested order; unknown ids are ignored.
    for (const auto& item : order) {
        int idx = findEntry(loadout, item.id.c_str());
        if (idx < 0 || taken[(size_t)idx]) continue;
        taken[(size_t)idx] = true;
        LoadoutEntry e = loadout.entries[(size_t)idx];
        if (item.hasCategory) e.category = item.category;
        arranged.push_back(std::move(e));
    }
    // Entries missing from the order keep their relative order, appended
    // after (prune-not-fatal philosophy: an incomplete order never loses
    // entries).
    for (size_t i = 0; i < loadout.entries.size(); i++) {
        if (!taken[i]) arranged.push_back(loadout.entries[i]);
    }
    loadout.entries = std::move(arranged);
    normalizeSections(loadout, apps, count); // displayed-menu order
    return true;
}

bool applyOps(Loadout& loadout, const char* opsJson, int* appliedOut,
              const RegistryApp* apps, int count) {
    if (appliedOut) *appliedOut = 0;
    if (!opsJson) return false;

    Cursor c{opsJson};
    Loadout work = loadout; // apply to a copy so a failure leaves the original
    int applied = 0;
    OpsMeta meta; // validated here; acted on by the transport session

    skipWs(c);
    if (*c.p != '{') return false;
    c.p++;
    skipWs(c);
    if (*c.p != '}') {
        while (true) {
            skipWs(c);
            std::string key;
            if (!parseString(c, key)) return false;
            skipWs(c);
            if (*c.p != ':') return false;
            c.p++;
            skipWs(c);
            if (key == "ops") {
                if (*c.p != '[') return false;
                c.p++;
                skipWs(c);
                if (*c.p != ']') {
                    while (true) {
                        if (!applyOneOp(c, work, applied, apps, count)) return false;
                        skipWs(c);
                        if (*c.p == ',') { c.p++; continue; }
                        if (*c.p == ']') { c.p++; break; }
                        return false;
                    }
                } else {
                    c.p++;
                }
            } else if (key == "batch" || key == "base") {
                if (!parseMetaField(c, key, meta)) return false;
            } else {
                if (!skipValue(c, 0)) return false; // unknown top-level field
            }
            skipWs(c);
            if (*c.p == ',') { c.p++; continue; }
            if (*c.p == '}') { c.p++; break; }
            return false;
        }
    } else {
        c.p++;
    }
    skipWs(c);
    if (*c.p != '\0') return false; // trailing garbage

    loadout = std::move(work);
    if (appliedOut) *appliedOut = applied;
    return true;
}

bool applyReplace(Loadout& loadout, const LoadoutEntry& entry) {
    if (entry.id.empty() || entry.blobPath.empty()) return false;
    int idx = findEntry(loadout, entry.id.c_str());
    if (idx < 0) return false; // replace never creates an entry
    LoadoutEntry& e = loadout.entries[(size_t)idx];
    // Only a delivered blob app has a blob to swap; builtin (and any other
    // kind, e.g. sprite packs) entries are refused.
    if (e.format != "wasm" && e.format != "blob") return false;
    e.blobPath = entry.blobPath;
    e.version  = entry.version;
    e.abi      = entry.abi;
    e.name     = entry.name;
    // A signature belongs to the blob it signed: the new blob's (or none).
    e.signature = entry.signature;
    return true;
}

bool parseOpsMeta(const char* opsJson, OpsMeta& out) {
    if (!opsJson) return false;
    Cursor c{opsJson};
    OpsMeta meta;
    skipWs(c);
    if (*c.p != '{') return false;
    c.p++;
    skipWs(c);
    if (*c.p != '}') {
        while (true) {
            skipWs(c);
            std::string key;
            if (!parseString(c, key)) return false;
            skipWs(c);
            if (*c.p != ':') return false;
            c.p++;
            skipWs(c);
            if (key == "batch" || key == "base") {
                if (!parseMetaField(c, key, meta)) return false;
            } else {
                if (!skipValue(c, 0)) return false;
            }
            skipWs(c);
            if (*c.p == ',') { c.p++; continue; }
            if (*c.p == '}') { c.p++; break; }
            return false;
        }
    } else {
        c.p++;
    }
    skipWs(c);
    if (*c.p != '\0') return false; // trailing garbage
    out = std::move(meta);
    return true;
}

bool collectOpBlobPaths(const char* opsJson, std::vector<std::string>& out) {
    if (!opsJson) return false;
    Cursor c{opsJson};
    std::vector<std::string> paths;
    skipWs(c);
    if (*c.p != '{') return false;
    c.p++;
    skipWs(c);
    if (*c.p == '}') { out.clear(); return true; }
    while (true) {
        skipWs(c);
        std::string key;
        if (!parseString(c, key)) return false;
        skipWs(c);
        if (*c.p != ':') return false;
        c.p++;
        skipWs(c);
        if (key != "ops") {
            if (!skipValue(c, 0)) return false;
        } else {
            if (*c.p != '[') return false;
            c.p++;
            skipWs(c);
            if (*c.p == ']') {
                c.p++;
            } else {
                while (true) {
                    // One op object: only its `entry` matters here.
                    skipWs(c);
                    if (*c.p != '{') return false;
                    c.p++;
                    skipWs(c);
                    if (*c.p == '}') {
                        c.p++;
                    } else {
                        while (true) {
                            skipWs(c);
                            std::string opKey;
                            if (!parseString(c, opKey)) return false;
                            skipWs(c);
                            if (*c.p != ':') return false;
                            c.p++;
                            skipWs(c);
                            if (opKey == "entry") {
                                LoadoutEntry entry;
                                bool positionSeen;
                                if (!parseEntry(c, entry, positionSeen)) return false;
                                if (!entry.blobPath.empty()) paths.push_back(entry.blobPath);
                            } else {
                                if (!skipValue(c, 0)) return false;
                            }
                            skipWs(c);
                            if (*c.p == ',') { c.p++; continue; }
                            if (*c.p == '}') { c.p++; break; }
                            return false;
                        }
                    }
                    skipWs(c);
                    if (*c.p == ',') { c.p++; continue; }
                    if (*c.p == ']') { c.p++; break; }
                    return false;
                }
            }
        }
        skipWs(c);
        if (*c.p == ',') { c.p++; continue; }
        if (*c.p == '}') { c.p++; break; }
        return false;
    }
    out = std::move(paths);
    return true;
}

std::string serializeAppliedRecord(const AppliedRecord& rec) {
    char crc[9];
    std::snprintf(crc, sizeof(crc), "%08x", (unsigned)rec.crcAfter);
    std::string out = "{\"batch\":";
    appendEscaped(out, rec.batch);
    out += ",\"result\":";
    appendEscaped(out, rec.result);
    out += ",\"crc_after\":\"";
    out += crc;
    out += "\",\"at\":";
    out += std::to_string((unsigned long)rec.at);
    out += ",\"ops\":";
    out += std::to_string(rec.ops);
    out += ",\"entries\":";
    out += std::to_string(rec.entries);
    std::snprintf(crc, sizeof(crc), "%08x", (unsigned)rec.docCrc);
    out += ",\"doc_crc\":\"";
    out += crc;
    out += "\"}\n";
    return out;
}

bool parseAppliedRecord(const char* json, AppliedRecord& out) {
    if (!json) return false;
    Cursor c{json};
    AppliedRecord rec;
    bool hasBatch = false, hasResult = false;
    skipWs(c);
    if (*c.p != '{') return false;
    c.p++;
    skipWs(c);
    if (*c.p != '}') {
        while (true) {
            skipWs(c);
            std::string key;
            if (!parseString(c, key)) return false;
            skipWs(c);
            if (*c.p != ':') return false;
            c.p++;
            skipWs(c);
            long v = 0;
            if (key == "batch") {
                if (!parseString(c, rec.batch)) return false;
                hasBatch = true;
            } else if (key == "result") {
                if (!parseString(c, rec.result)) return false;
                hasResult = true;
            } else if (key == "crc_after") {
                std::string hex;
                if (!parseString(c, hex) || !parseBaseHex(hex, rec.crcAfter)) return false;
            } else if (key == "doc_crc") {
                std::string hex;
                if (!parseString(c, hex) || !parseBaseHex(hex, rec.docCrc)) return false;
            } else if (key == "at") {
                if (!parseInt(c, v) || v < 0) return false;
                rec.at = (uint32_t)v;
            } else if (key == "ops") {
                if (!parseInt(c, v) || v < 0 || v > INT_MAX) return false;
                rec.ops = (int)v;
            } else if (key == "entries") {
                if (!parseInt(c, v) || v < 0 || v > INT_MAX) return false;
                rec.entries = (int)v;
            } else {
                if (!skipValue(c, 0)) return false;
            }
            skipWs(c);
            if (*c.p == ',') { c.p++; continue; }
            if (*c.p == '}') { c.p++; break; }
            return false;
        }
    } else {
        c.p++;
    }
    skipWs(c);
    if (*c.p != '\0') return false; // trailing garbage
    if (!hasBatch || !hasResult || rec.batch.empty()) return false;
    out = std::move(rec);
    return true;
}

} // namespace LoadoutManifest
