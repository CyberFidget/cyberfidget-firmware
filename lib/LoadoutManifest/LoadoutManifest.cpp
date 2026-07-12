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
#include <cstring>

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

// Regroup entries so each category is one contiguous run, keeping the
// relative order within a category and ordering sections by first
// appearance. This is what makes "sections = contiguous category runs"
// hold by construction after an arrange op.
void normalizeSections(Loadout& loadout) {
    std::vector<LoadoutEntry> result;
    result.reserve(loadout.entries.size());
    std::vector<bool> placed(loadout.entries.size(), false);
    for (size_t i = 0; i < loadout.entries.size(); i++) {
        if (placed[i]) continue;
        const std::string cat = loadout.entries[i].category; // copy: entries get moved below
        for (size_t j = i; j < loadout.entries.size(); j++) {
            if (!placed[j] && loadout.entries[j].category == cat) {
                placed[j] = true;
                result.push_back(std::move(loadout.entries[j]));
            }
        }
    }
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

std::string flattenCategory(const char* path) {
    if (!path) return std::string();
    const char* slash = std::strchr(path, '/');
    if (!slash) return std::string(path);
    return std::string(path, (size_t)(slash - path));
}

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

std::vector<MergedApp> mergeWithRegistry(const Loadout& loadout,
                                         const RegistryApp* apps, int count) {
    std::vector<MergedApp> out;
    if (!apps || count <= 0) return out;
    std::vector<bool> used((size_t)count, false);

    // 1) Manifest entries, in manifest order. Stale ids (no registry
    //    match) and duplicates are pruned, not fatal.
    for (const auto& entry : loadout.entries) {
        // A ferried app has no compile-time registry row; it launches from
        // its file. The producer of truth is the website sync, which stamps
        // format "wasm" (the honest module format); "blob" is also accepted.
        // The load-bearing signal is a NON-builtin format with a blobPath -
        // that is what makes an entry a ferried, file-launched app (T-183).
        // Guard: no path = unlaunchable, dropped like a stale id.
        if (entry.format != "builtin" && !entry.format.empty()) {
            if (entry.blobPath.empty()) continue;
            MergedApp m;
            m.appIndex = -1;
            m.category = entry.category;
            m.hidden   = entry.hidden;
            m.label    = entry.name.empty() ? entry.id : entry.name;
            m.blobPath = entry.blobPath;
            out.push_back(m);
            continue;
        }
        int idx = -1;
        for (int i = 0; i < count; i++) {
            if (apps[i].id == entry.id) { idx = i; break; }
        }
        if (idx < 0 || used[(size_t)idx]) continue;
        if (apps[idx].name.empty()) continue; // not a menu item
        used[(size_t)idx] = true;
        MergedApp m;
        m.appIndex = idx;
        m.category = entry.category.empty() ? apps[idx].category : entry.category;
        m.hidden   = entry.hidden;
        out.push_back(m);
    }

    // 2) Compiled-in apps absent from the manifest: append in compile
    //    order so new apps appear after a firmware update. (The menu
    //    tree groups by category name, so they still render inside
    //    their category; the flat order becomes contiguous again the
    //    next time the manifest is rewritten.)
    for (int i = 0; i < count; i++) {
        if (used[(size_t)i] || apps[i].name.empty()) continue;
        MergedApp m;
        m.appIndex = i;
        m.category = apps[i].category;
        m.hidden   = false;
        out.push_back(m);
    }
    return out;
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

// Parse and apply one op object (cursor on its opening '{') to `work`.
bool applyOneOp(Cursor& c, Loadout& work, int& applied) {
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
        if (!applyArrange(work, order)) return false;
    } else {
        return false; // unknown / missing op discriminator
    }
    applied++;
    return true;
}

} // namespace

bool applyArrange(Loadout& loadout, const std::vector<ArrangeItem>& order) {
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
    normalizeSections(loadout); // contiguity by construction
    return true;
}

bool applyOps(Loadout& loadout, const char* opsJson, int* appliedOut) {
    if (appliedOut) *appliedOut = 0;
    if (!opsJson) return false;

    Cursor c{opsJson};
    Loadout work = loadout; // apply to a copy so a failure leaves the original
    int applied = 0;

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
                        if (!applyOneOp(c, work, applied)) return false;
                        skipWs(c);
                        if (*c.p == ',') { c.p++; continue; }
                        if (*c.p == ']') { c.p++; break; }
                        return false;
                    }
                } else {
                    c.p++;
                }
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

} // namespace LoadoutManifest
