// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

// Home Remote's pure logic (lib/HomeRemote/HomeRemoteLogic): the line
// format Home Assistant renders, the request bodies, soft takeover, the
// send gate, the poll guard and hold-to-repeat.

#include <unity.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>

#include "HomeRemoteLogic.h"

using namespace HomeRemote;

void setUp(void) {}
void tearDown(void) {}

static bool parseLine(const char* s, Entity& e) { return parseEntityLine(s, strlen(s), e); }

// ---- entity lines -----------------------------------------------------------

void test_entity_line_full_light(void) {
    Entity e;
    TEST_ASSERT_TRUE(parseLine("light.table_lamp|Table Lamp|on|184|255,40,120|3|12|sunrise", e));
    TEST_ASSERT_EQUAL_STRING("light.table_lamp", e.id);
    TEST_ASSERT_EQUAL_STRING("Table Lamp", e.name);
    TEST_ASSERT_TRUE(e.kind == Kind::Light);
    TEST_ASSERT_TRUE(e.on);
    TEST_ASSERT_TRUE(e.available);
    TEST_ASSERT_EQUAL_UINT8(184, e.brightness);
    TEST_ASSERT_TRUE(e.hasRgb);
    TEST_ASSERT_EQUAL_UINT8(255, e.r);
    TEST_ASSERT_EQUAL_UINT8(40, e.g);
    TEST_ASSERT_EQUAL_UINT8(120, e.b);
    TEST_ASSERT_TRUE(e.canDim);
    TEST_ASSERT_TRUE(e.canColor);
    TEST_ASSERT_EQUAL_UINT8(12, e.effectCount);
    TEST_ASSERT_EQUAL_STRING("sunrise", e.effect);
}

void test_entity_line_off_light_has_no_colour(void) {
    Entity e;
    TEST_ASSERT_TRUE(parseLine("light.cob_strip|COB Strip|off|0||3|0|", e));
    TEST_ASSERT_FALSE(e.on);
    TEST_ASSERT_FALSE(e.hasRgb);
    TEST_ASSERT_EQUAL_UINT8(0, e.brightness);
    TEST_ASSERT_EQUAL_UINT8(0, e.effectCount);
    TEST_ASSERT_EQUAL_STRING("", e.effect);
}

void test_entity_line_scene_and_script(void) {
    Entity e;
    TEST_ASSERT_TRUE(parseLine("scene.movie_night|Movie night|2026-10-16T10:00:00+00:00|0||0|0|", e));
    TEST_ASSERT_TRUE(e.kind == Kind::Scene);
    TEST_ASSERT_TRUE(parseLine("script.party|Party|off|0||0|0|", e));
    TEST_ASSERT_TRUE(e.kind == Kind::Script);
}

void test_entity_line_unavailable(void) {
    Entity e;
    TEST_ASSERT_TRUE(parseLine("light.spot_lights|Spots|unavailable|0||3|0|", e));
    TEST_ASSERT_FALSE(e.available);
    TEST_ASSERT_FALSE(e.on);
}

void test_entity_line_trims_carriage_return_and_spaces(void) {
    Entity e;
    TEST_ASSERT_TRUE(parseLine("  light.a|A|on|10||1|0|\r", e));
    TEST_ASSERT_EQUAL_STRING("light.a", e.id);
    TEST_ASSERT_TRUE(e.canDim);
    TEST_ASSERT_FALSE(e.canColor);
}

void test_entity_line_rejects_malformed(void) {
    Entity e;
    TEST_ASSERT_FALSE(parseLine("", e));
    TEST_ASSERT_FALSE(parseLine("light.a|A|on|10||1|0", e));          // 7 fields
    TEST_ASSERT_FALSE(parseLine("light.a|A|on|10||1|0|x|y", e));      // 9 fields
    TEST_ASSERT_FALSE(parseLine("light.a|A|on|256||1|0|", e));        // brightness > 255
    TEST_ASSERT_FALSE(parseLine("light.a|A|on|-1||1|0|", e));
    TEST_ASSERT_FALSE(parseLine("light.a|A|on|10|1,2|1|0|", e));      // two colour parts
    TEST_ASSERT_FALSE(parseLine("light.a|A|on|10|1,2,300|1|0|", e));
    TEST_ASSERT_FALSE(parseLine("light.a|A|on|10||4|0|", e));         // caps > 3
    TEST_ASSERT_FALSE(parseLine("Light.A|A|on|10||1|0|", e));         // id case
    TEST_ASSERT_FALSE(parseLine("switch.fan|Fan|on|0||0|0|", e));      // not a light/scene/script
    TEST_ASSERT_FALSE(parseLine("light.a'b|A|on|10||1|0|", e));
}

void test_entity_name_is_cut_to_fit(void) {
    std::string line = "light.long|";
    line += std::string(80, 'N');
    line += "|on|1||1|0|";
    Entity e;
    TEST_ASSERT_TRUE(parseEntityLine(line.c_str(), line.size(), e));
    TEST_ASSERT_EQUAL_size_t(kNameMax, strlen(e.name));
}

void test_empty_name_falls_back_to_id(void) {
    Entity e;
    TEST_ASSERT_TRUE(parseLine("light.a||on|10||1|0|", e));
    TEST_ASSERT_EQUAL_STRING("light.a", e.name);
}

void test_entities_body_skips_bad_lines_and_caps(void) {
    const char body[] =
        "light.a|A|on|10||1|0|\n"
        "garbage line\n"
        "\n"
        "scene.b|B|x|0||0|0|\r\n"
        "light.c|C|off|0||3|2|\n";
    Entity rows[kMaxRows];
    TEST_ASSERT_EQUAL_INT(3, parseEntities(body, strlen(body), rows, kMaxRows));
    TEST_ASSERT_EQUAL_STRING("scene.b", rows[1].id);
    TEST_ASSERT_EQUAL_INT(2, parseEntities(body, strlen(body), rows, 2));

    std::string many;
    for (int i = 0; i < 50; i++) many += "light.x" + std::to_string(i) + "|X|on|1||1|0|\n";
    TEST_ASSERT_EQUAL_INT(kMaxRows, parseEntities(many.c_str(), many.size(), rows, kMaxRows));
}

void test_overlong_line_is_dropped(void) {
    std::string body = "light.a|A|on|10||1|0|" + std::string(300, 'e') + "\nlight.b|B|on|1||1|0|\n";
    Entity rows[4];
    TEST_ASSERT_EQUAL_INT(1, parseEntities(body.c_str(), body.size(), rows, 4));
    TEST_ASSERT_EQUAL_STRING("light.b", rows[0].id);
}

// ---- areas and effects ------------------------------------------------------

void test_area_lines(void) {
    const char body[] = "booth|Booth|3\nliving_room|Living Room|2\nBad Id|X|1\nshop||4\n";
    Area areas[8];
    const int n = parseAreas(body, strlen(body), areas, 8);
    TEST_ASSERT_EQUAL_INT(3, n);
    TEST_ASSERT_EQUAL_STRING("booth", areas[0].id);
    TEST_ASSERT_EQUAL_STRING("Booth", areas[0].name);
    TEST_ASSERT_EQUAL_UINT16(3, areas[0].count);
    TEST_ASSERT_EQUAL_STRING("shop", areas[2].name);
}

void test_effect_lines_skip_names_too_long_to_send_back(void) {
    std::string body = "\"sunrise\"\n\"sunset\"\n\"" + std::string(kEffectMax + 1, 'z') +
                       "\"\nnot json\n\"movie\"\n";
    char effects[kMaxEffects][kEffectMax + 1];
    const int n = parseEffects(body.c_str(), body.size(), effects, kMaxEffects);
    TEST_ASSERT_EQUAL_INT(3, n);
    TEST_ASSERT_EQUAL_STRING("movie", effects[2]);
    // Exactly kEffectMax bytes still fits.
    body = "\"" + std::string(kEffectMax, 'y') + "\"\n";
    TEST_ASSERT_EQUAL_INT(1, parseEffects(body.c_str(), body.size(), effects, kMaxEffects));
}

// The name the device sends back is byte-for-byte the one Home Assistant
// listed: '|', quotes, backslashes, line breaks, edge spaces, non-ASCII.
void test_effect_names_round_trip_exactly(void) {
    const char body[] =
        "\"A|B\"\n"
        "\"say \\\"hi\\\"\"\n"
        "\"back\\\\slash\"\n"
        "\"two\\nlines\"\n"
        "\"  edge  \"\n"
        "\"caf\\u00e9\"\n"
        "\"caf\xc3\xa9 raw\"\n"
        "\"\\ud83c\\udf05 dawn\"\n";
    char effects[kMaxEffects][kEffectMax + 1];
    const int n = parseEffects(body, strlen(body), effects, kMaxEffects);
    TEST_ASSERT_EQUAL_INT(8, n);
    TEST_ASSERT_EQUAL_STRING("A|B", effects[0]);
    TEST_ASSERT_EQUAL_STRING("say \"hi\"", effects[1]);
    TEST_ASSERT_EQUAL_STRING("back\\slash", effects[2]);
    TEST_ASSERT_EQUAL_STRING("two\nlines", effects[3]);
    TEST_ASSERT_EQUAL_STRING("  edge  ", effects[4]);
    TEST_ASSERT_EQUAL_STRING("caf\xc3\xa9", effects[5]);
    TEST_ASSERT_EQUAL_STRING("caf\xc3\xa9 raw", effects[6]);
    TEST_ASSERT_EQUAL_STRING("\xf0\x9f\x8c\x85 dawn", effects[7]);

    // And it goes back out as the same JSON string value.
    Action a;
    strcpy(a.entity, "light.x");
    a.type = Action::Type::TurnOn;
    a.setEffect = true;
    char out[256];
    for (int i = 0; i < n; i++) {
        strcpy(a.effect, effects[i]);
        TEST_ASSERT_TRUE(serviceBody(a, out, sizeof(out)) > 0);
        const char* v = strstr(out, "\"effect\":");
        TEST_ASSERT_NOT_NULL(v);
        v += 9;
        char back[kEffectMax + 1];
        TEST_ASSERT_EQUAL_INT((int)strlen(effects[i]),
                              decodeJsonString(v, strlen(v) - 1, back, sizeof(back)));
        TEST_ASSERT_EQUAL_STRING(effects[i], back);
    }

    // Drawn without the line break.
    char shown[kEffectMax + 1];
    displayText(effects[3], shown, sizeof(shown));
    TEST_ASSERT_EQUAL_STRING("two lines", shown);
}

void test_json_string_decode_rejects(void) {
    char out[16];
    TEST_ASSERT_EQUAL_INT(-1, decodeJsonString("abc", 3, out, sizeof(out)));          // no quotes
    TEST_ASSERT_EQUAL_INT(-1, decodeJsonString("\"a\\x\"", 5, out, sizeof(out)));     // bad escape
    TEST_ASSERT_EQUAL_INT(-1, decodeJsonString("\"\\u0000\"", 8, out, sizeof(out)));  // NUL
    TEST_ASSERT_EQUAL_INT(-1, decodeJsonString("\"\\ud800\"", 8, out, sizeof(out)));  // lone surrogate
    TEST_ASSERT_EQUAL_INT(-1, decodeJsonString("\"a\"b\"", 5, out, sizeof(out)));     // bare quote
    TEST_ASSERT_EQUAL_INT(-1, decodeJsonString("\"0123456789abcdef\"", 18, out, sizeof(out)));  // too long
    TEST_ASSERT_EQUAL_INT(0, decodeJsonString("\"\"", 2, out, sizeof(out)));
}

static int decodeRaw(const std::string& inner) {
    const std::string s = "\"" + inner + "\"";
    char out[32];
    return decodeJsonString(s.data(), s.size(), out, sizeof(out));
}

void test_json_string_decode_checks_raw_utf8(void) {
    // Well-formed: 2, 3 and 4 byte forms, and the edges of each range.
    TEST_ASSERT_EQUAL_INT(2, decodeRaw("\xc3\xa9"));             // U+00E9
    TEST_ASSERT_EQUAL_INT(2, decodeRaw("\xc2\x80"));             // U+0080
    TEST_ASSERT_EQUAL_INT(3, decodeRaw("\xe0\xa0\x80"));         // U+0800
    TEST_ASSERT_EQUAL_INT(3, decodeRaw("\xed\x9f\xbf"));         // U+D7FF
    TEST_ASSERT_EQUAL_INT(3, decodeRaw("\xee\x80\x80"));         // U+E000
    TEST_ASSERT_EQUAL_INT(4, decodeRaw("\xf0\x90\x80\x80"));     // U+10000
    TEST_ASSERT_EQUAL_INT(4, decodeRaw("\xf4\x8f\xbf\xbf"));     // U+10FFFF
    // Overlong forms.
    TEST_ASSERT_EQUAL_INT(-1, decodeRaw("\xc0\xaf"));
    TEST_ASSERT_EQUAL_INT(-1, decodeRaw("\xc1\xbf"));
    TEST_ASSERT_EQUAL_INT(-1, decodeRaw("\xe0\x9f\xbf"));
    TEST_ASSERT_EQUAL_INT(-1, decodeRaw("\xf0\x8f\xbf\xbf"));
    // Surrogates encoded raw.
    TEST_ASSERT_EQUAL_INT(-1, decodeRaw("\xed\xa0\x80"));
    TEST_ASSERT_EQUAL_INT(-1, decodeRaw("\xed\xbf\xbf"));
    // Above U+10FFFF, and lead bytes that never start a character.
    TEST_ASSERT_EQUAL_INT(-1, decodeRaw("\xf4\x90\x80\x80"));
    TEST_ASSERT_EQUAL_INT(-1, decodeRaw("\xf5\x80\x80\x80"));
    TEST_ASSERT_EQUAL_INT(-1, decodeRaw("\xff"));
    // Bad or missing continuation bytes.
    TEST_ASSERT_EQUAL_INT(-1, decodeRaw("\x80"));
    TEST_ASSERT_EQUAL_INT(-1, decodeRaw("\xc3" "A"));
    TEST_ASSERT_EQUAL_INT(-1, decodeRaw("\xe2\x82"));            // cut short by the closing quote
    TEST_ASSERT_EQUAL_INT(-1, decodeRaw("\xe2\x28\xa1"));
    // A bad one inside an effect list line drops only that line.
    const char body[] = "\"ok\"\n\"bad\xc0\xaf\"\n\"fine\xc3\xa9\"\n";
    char effects[kMaxEffects][kEffectMax + 1];
    TEST_ASSERT_EQUAL_INT(2, parseEffects(body, strlen(body), effects, kMaxEffects));
    TEST_ASSERT_EQUAL_STRING("fine\xc3\xa9", effects[1]);
}

void test_secure_wipe(void) {
    char buf[8] = "secret";
    secureWipe(buf, sizeof(buf));
    for (char c : buf) TEST_ASSERT_EQUAL_CHAR(0, c);
    secureWipe(nullptr, 4);   // harmless
}

// ---- ids, urls, tokens ------------------------------------------------------

void test_ids(void) {
    TEST_ASSERT_TRUE(validEntityId("light.table_lamp"));
    TEST_ASSERT_FALSE(validEntityId("light"));
    TEST_ASSERT_FALSE(validEntityId("light.a.b"));
    TEST_ASSERT_FALSE(validEntityId(".abc"));
    TEST_ASSERT_FALSE(validEntityId("light.a')"));
    TEST_ASSERT_TRUE(validAreaId("booth"));
    TEST_ASSERT_FALSE(validAreaId("booth'"));
    TEST_ASSERT_FALSE(validAreaId(""));
}

void test_base_url(void) {
    char out[kUrlMax + 1];
    TEST_ASSERT_TRUE(normalizeBaseUrl("http://192.168.1.156/", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("http://192.168.1.156", out);
    TEST_ASSERT_TRUE(normalizeBaseUrl("HTTP://homeassistant.local:8123", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("http://homeassistant.local:8123", out);
    TEST_ASSERT_FALSE(normalizeBaseUrl("https://ha.example", out, sizeof(out)));
    TEST_ASSERT_FALSE(normalizeBaseUrl("http://", out, sizeof(out)));
    TEST_ASSERT_FALSE(normalizeBaseUrl("http:///x", out, sizeof(out)));
    TEST_ASSERT_FALSE(normalizeBaseUrl("http://user:pw@host", out, sizeof(out)));
    TEST_ASSERT_FALSE(normalizeBaseUrl("http://host/a b", out, sizeof(out)));
    TEST_ASSERT_FALSE(normalizeBaseUrl("http://host?x=1", out, sizeof(out)));
    TEST_ASSERT_FALSE(normalizeBaseUrl("192.168.1.156", out, sizeof(out)));
}

void test_token_chars(void) {
    const char jwt[] = "eyJhbGciOiJIUzI1NiJ9.eyJpc3MiOiJ4In0.abc-_DEF";
    TEST_ASSERT_TRUE(validTokenChars(jwt, strlen(jwt)));
    TEST_ASSERT_FALSE(validTokenChars("has space", 9));
    TEST_ASSERT_FALSE(validTokenChars("", 0));
    std::string longTok(kTokenMax + 1, 'a');
    TEST_ASSERT_FALSE(validTokenChars(longTok.c_str(), longTok.size()));
}

// ---- JSON and request bodies ------------------------------------------------

void test_json_escape(void) {
    char out[64];
    TEST_ASSERT_EQUAL_size_t(13, jsonEscape("a\"b\\c\nd\te", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("a\\\"b\\\\c\\nd\\te", out);
    TEST_ASSERT_EQUAL_size_t(0, jsonEscape("abcdef", out, 4));
    TEST_ASSERT_EQUAL_STRING("", out);
    char ctl[2] = {0x01, 0};
    TEST_ASSERT_EQUAL_size_t(6, jsonEscape(ctl, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("\\u0001", out);
}

void test_template_bodies(void) {
    char body[4096];
    size_t n = favoritesBody(body, sizeof(body));
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_EQUAL_size_t(strlen(body), n);
    TEST_ASSERT_EQUAL_INT(0, strncmp(body, "{\"template\":\"", 13));
    TEST_ASSERT_NOT_NULL(strstr(body, "label_entities('cyber_fidget')"));
    TEST_ASSERT_NOT_NULL(strstr(body, "macro row(s)"));
    // A real newline in the template travels as \n; the Jinja "\n" literal
    // inside replace() travels as \\n.
    TEST_ASSERT_NOT_NULL(strstr(body, "replace('\\\\n',' ')"));
    TEST_ASSERT_NOT_NULL(strstr(body, "\\n{% endmacro"));
    TEST_ASSERT_NULL(strchr(body, '\n'));
    TEST_ASSERT_EQUAL_STRING("\"}", body + n - 2);

    TEST_ASSERT_TRUE(areasBody(body, sizeof(body)) > 0);
    TEST_ASSERT_NOT_NULL(strstr(body, "areas()"));

    TEST_ASSERT_TRUE(areaEntitiesBody("booth", body, sizeof(body)) > 0);
    TEST_ASSERT_NOT_NULL(strstr(body, "area_entities('booth')"));
    TEST_ASSERT_EQUAL_size_t(0, areaEntitiesBody("booth')", body, sizeof(body)));

    TEST_ASSERT_TRUE(entityBody("light.table_lamp", body, sizeof(body)) > 0);
    TEST_ASSERT_NOT_NULL(strstr(body, "'eq','light.table_lamp'"));
    TEST_ASSERT_EQUAL_size_t(0, entityBody("light.x'", body, sizeof(body)));

    TEST_ASSERT_TRUE(effectsBody("light.table_lamp", body, sizeof(body)) > 0);
    TEST_ASSERT_NOT_NULL(strstr(body, "state_attr('light.table_lamp','effect_list')"));
    TEST_ASSERT_NOT_NULL(strstr(body, "to_json"));

    // Too small a buffer is refused, never cut.
    TEST_ASSERT_EQUAL_size_t(0, favoritesBody(body, 100));
    TEST_ASSERT_EQUAL_STRING("", body);
}

// Entities without brightness / colour / effects (scenes, scripts, on/off
// lights, other domains) must not fail the list: every attribute is read
// with .get(), never as a bare attribute that is undefined when missing.
void test_row_template_tolerates_missing_attributes(void) {
    char t[kTemplateMax];
    TEST_ASSERT_TRUE(templateText(Template::Favorites, nullptr, t, sizeof(t)) > 0);
    TEST_ASSERT_NOT_NULL(strstr(t, "a.get('brightness')|int(0)"));
    TEST_ASSERT_NOT_NULL(strstr(t, "a.get('rgb_color')"));
    TEST_ASSERT_NOT_NULL(strstr(t, "a.get('effect_list') or []"));
    TEST_ASSERT_NOT_NULL(strstr(t, "a.get('supported_color_modes') or []"));
    TEST_ASSERT_NOT_NULL(strstr(t, "a.get('effect') or ''"));
    TEST_ASSERT_NULL(strstr(t, "s.attributes."));
    // The template text and the body agree (the body is just its JSON form).
    char body[4096];
    TEST_ASSERT_TRUE(favoritesBody(body, sizeof(body)) > strlen(t));
    TEST_ASSERT_EQUAL_size_t(0, templateText(Template::Entity, "bad id", t, sizeof(t)));
    TEST_ASSERT_EQUAL_size_t(0, templateText(Template::Favorites, nullptr, t, 64));
}

// HR_DUMP_TEMPLATES=<dir> pio test -e test_home_remote writes every
// template exactly as sent (raw Jinja and the JSON request body) so they
// can be checked against a real Home Assistant (POST the .json file to
// /api/template). Without the variable it writes nothing.
void test_dump_templates(void) {
    const char* dir = getenv("HR_DUMP_TEMPLATES");
    if (!dir || !dir[0]) { TEST_PASS_MESSAGE("HR_DUMP_TEMPLATES not set; nothing written"); return; }
    struct Item { const char* name; Template kind; const char* arg; };
    const Item items[] = {
        {"favorites", Template::Favorites, nullptr},
        {"areas", Template::Areas, nullptr},
        {"area_booth", Template::AreaEntities, "booth"},
        {"entity_table_lamp", Template::Entity, "light.table_lamp"},
        {"effects_table_lamp", Template::Effects, "light.table_lamp"},
    };
    for (const Item& it : items) {
        char t[kTemplateMax];
        TEST_ASSERT_TRUE(templateText(it.kind, it.arg, t, sizeof(t)) > 0);
        char json[4096];
        size_t n = 0;
        switch (it.kind) {
            case Template::Favorites:    n = favoritesBody(json, sizeof(json)); break;
            case Template::Areas:        n = areasBody(json, sizeof(json)); break;
            case Template::AreaEntities: n = areaEntitiesBody(it.arg, json, sizeof(json)); break;
            case Template::Entity:       n = entityBody(it.arg, json, sizeof(json)); break;
            case Template::Effects:      n = effectsBody(it.arg, json, sizeof(json)); break;
        }
        TEST_ASSERT_TRUE(n > 0);
        const std::string base = std::string(dir) + "/" + it.name;
        FILE* f = fopen((base + ".j2").c_str(), "wb");
        TEST_ASSERT_NOT_NULL(f);
        fwrite(t, 1, strlen(t), f);
        fclose(f);
        f = fopen((base + ".json").c_str(), "wb");
        TEST_ASSERT_NOT_NULL(f);
        fwrite(json, 1, n, f);
        fclose(f);
    }
}

void test_service_calls(void) {
    char path[64];
    char body[256];
    Action a;
    strcpy(a.entity, "light.table_lamp");
    a.type = Action::Type::Toggle;
    TEST_ASSERT_TRUE(servicePath(a, path, sizeof(path)));
    TEST_ASSERT_EQUAL_STRING("/api/services/light/toggle", path);
    TEST_ASSERT_TRUE(serviceBody(a, body, sizeof(body)) > 0);
    TEST_ASSERT_EQUAL_STRING("{\"entity_id\":\"light.table_lamp\"}", body);

    a.type = Action::Type::TurnOn;
    a.setBrightness = true;
    a.brightness = 128;
    a.setRgb = true;
    a.r = 255; a.g = 40; a.b = 120;
    TEST_ASSERT_TRUE(servicePath(a, path, sizeof(path)));
    TEST_ASSERT_EQUAL_STRING("/api/services/light/turn_on", path);
    serviceBody(a, body, sizeof(body));
    TEST_ASSERT_EQUAL_STRING(
        "{\"entity_id\":\"light.table_lamp\",\"brightness\":128,\"rgb_color\":[255,40,120]}", body);

    Action fx;
    strcpy(fx.entity, "light.table_lamp");
    fx.type = Action::Type::TurnOn;
    fx.setEffect = true;
    strcpy(fx.effect, "say \"hi\"");
    serviceBody(fx, body, sizeof(body));
    TEST_ASSERT_EQUAL_STRING("{\"entity_id\":\"light.table_lamp\",\"effect\":\"say \\\"hi\\\"\"}", body);

    Action off;
    strcpy(off.entity, "light.table_lamp");
    off.type = Action::Type::TurnOff;
    off.setBrightness = true;   // ignored by turn_off
    servicePath(off, path, sizeof(path));
    TEST_ASSERT_EQUAL_STRING("/api/services/light/turn_off", path);
    serviceBody(off, body, sizeof(body));
    TEST_ASSERT_EQUAL_STRING("{\"entity_id\":\"light.table_lamp\"}", body);

    Action scene;
    strcpy(scene.entity, "scene.movie");
    scene.type = Action::Type::Toggle;
    servicePath(scene, path, sizeof(path));
    TEST_ASSERT_EQUAL_STRING("/api/services/scene/turn_on", path);
    Action script;
    strcpy(script.entity, "script.party");
    script.type = Action::Type::Run;
    servicePath(script, path, sizeof(path));
    TEST_ASSERT_EQUAL_STRING("/api/services/script/turn_on", path);

    Action bad;
    strcpy(bad.entity, "light.x\",\"y");
    bad.type = Action::Type::Toggle;
    TEST_ASSERT_FALSE(servicePath(bad, path, sizeof(path)));
    TEST_ASSERT_EQUAL_size_t(0, serviceBody(bad, body, sizeof(body)));
    Action sceneOff;
    strcpy(sceneOff.entity, "scene.movie");
    sceneOff.type = Action::Type::TurnOff;
    TEST_ASSERT_FALSE(servicePath(sceneOff, path, sizeof(path)));
}

void test_classify(void) {
    TEST_ASSERT_TRUE(classify(false, 0) == Outcome::Unreachable);
    TEST_ASSERT_TRUE(classify(true, 200) == Outcome::Ok);
    TEST_ASSERT_TRUE(classify(true, 401) == Outcome::Rejected);
    TEST_ASSERT_TRUE(classify(true, 403) == Outcome::Rejected);
    TEST_ASSERT_TRUE(classify(true, 400) == Outcome::BadReply);
    TEST_ASSERT_TRUE(classify(true, 500) == Outcome::BadReply);
}

void test_hex_and_percent(void) {
    char hex[8];
    formatHex(255, 40, 120, hex);
    TEST_ASSERT_EQUAL_STRING("#FF2878", hex);
    formatHex(0, 0, 0, hex);
    TEST_ASSERT_EQUAL_STRING("#000000", hex);
    TEST_ASSERT_EQUAL_INT(0, brightnessPercent(0));
    TEST_ASSERT_EQUAL_INT(100, brightnessPercent(255));
    TEST_ASSERT_EQUAL_INT(50, brightnessPercent(128));
    TEST_ASSERT_EQUAL_INT(1, brightnessPercent(2));
}

void test_presets(void) {
    TEST_ASSERT_EQUAL_STRING("Warm white", preset(0).name);
    TEST_ASSERT_EQUAL_UINT8(255, preset(1).r);
    TEST_ASSERT_EQUAL_STRING("Magenta", preset(kPresetCount - 1).name);
    TEST_ASSERT_EQUAL_STRING("Magenta", preset(99).name);   // clamped
}

// ---- LED mirror -------------------------------------------------------------

void test_mirror_color(void) {
    LedColor off = mirrorColor(false, true, 255, 0, 0, 255);
    TEST_ASSERT_TRUE(off == LedColor());
    TEST_ASSERT_TRUE(mirrorColor(true, true, 255, 0, 0, 0) == LedColor());   // brightness 0
    LedColor full = mirrorColor(true, true, 255, 128, 0, 255);
    TEST_ASSERT_EQUAL_UINT8(kLedCap, full.r);                 // capped
    TEST_ASSERT_EQUAL_UINT8(kLedCap / 2, full.g);
    TEST_ASSERT_EQUAL_UINT8(0, full.b);                       // an off channel stays off
    LedColor half = mirrorColor(true, true, 255, 0, 0, 128);
    TEST_ASSERT_EQUAL_UINT8(24, half.r);
    LedColor dim = mirrorColor(true, true, 255, 10, 0, 3);
    TEST_ASSERT_EQUAL_UINT8(1, dim.r);                        // never rounds to dark
    TEST_ASSERT_EQUAL_UINT8(1, dim.g);
    LedColor white = mirrorColor(true, false, 0, 0, 0, 255);  // no colour reported: warm white
    TEST_ASSERT_EQUAL_UINT8(kLedCap, white.r);
    TEST_ASSERT_TRUE(white.b > 0 && white.b < white.g && white.g < white.r);
}

// ---- list cursor ------------------------------------------------------------

void test_list_cursor(void) {
    ListCursor c;
    c.reset(6, 4);
    TEST_ASSERT_EQUAL_INT(0, c.selected());
    c.up();                                  // wraps to the last
    TEST_ASSERT_EQUAL_INT(5, c.selected());
    TEST_ASSERT_EQUAL_INT(2, c.top());
    c.down();                                // wraps to the first
    TEST_ASSERT_EQUAL_INT(0, c.selected());
    TEST_ASSERT_EQUAL_INT(0, c.top());
    for (int i = 0; i < 4; i++) c.down();
    TEST_ASSERT_EQUAL_INT(4, c.selected());
    TEST_ASSERT_EQUAL_INT(1, c.top());
    c.setCount(8);                           // a refreshed list keeps the place
    TEST_ASSERT_EQUAL_INT(4, c.selected());
    c.setCount(3);                           // a shorter one clamps
    TEST_ASSERT_EQUAL_INT(2, c.selected());
    TEST_ASSERT_EQUAL_INT(0, c.top());
    TEST_ASSERT_EQUAL_INT(3, c.visible());
    c.setCount(0);
    c.down();
    TEST_ASSERT_EQUAL_INT(0, c.selected());
}

// ---- soft takeover ----------------------------------------------------------

void test_takeover_no_change_on_open(void) {
    SoftTakeover t;
    t.arm(77, 230);   // light at 30 %, slider at 90 %
    int v = -1;
    TEST_ASSERT_FALSE(t.engaged());
    TEST_ASSERT_FALSE(t.update(230, &v));
    TEST_ASSERT_FALSE(t.update(200, &v));    // moving, not yet at the light's level
    TEST_ASSERT_FALSE(t.update(120, &v));
    TEST_ASSERT_EQUAL_INT(-1, v);
}

void test_takeover_picks_up_at_level_then_drives(void) {
    SoftTakeover t;
    t.arm(77, 230);
    int v = -1;
    TEST_ASSERT_TRUE(t.update(80, &v));      // within tolerance: picked up
    TEST_ASSERT_TRUE(t.engaged());
    TEST_ASSERT_EQUAL_INT(80, v);
    TEST_ASSERT_FALSE(t.update(81, &v));     // jitter below the step
    TEST_ASSERT_TRUE(t.update(100, &v));
    TEST_ASSERT_EQUAL_INT(100, v);
    TEST_ASSERT_TRUE(t.update(255, &v));
    TEST_ASSERT_EQUAL_INT(255, v);
}

void test_takeover_picks_up_on_crossing(void) {
    SoftTakeover t;
    t.arm(128, 250);
    int v = -1;
    TEST_ASSERT_FALSE(t.update(140, &v));
    TEST_ASSERT_TRUE(t.update(40, &v));      // a fast move jumped across the level
    TEST_ASSERT_EQUAL_INT(40, v);
    TEST_ASSERT_TRUE(t.engaged());
}

void test_takeover_engaged_at_open_waits_for_movement(void) {
    SoftTakeover t;
    t.arm(100, 103);
    int v = -1;
    TEST_ASSERT_TRUE(t.engaged());
    TEST_ASSERT_FALSE(t.update(103, &v));    // no send on open
    TEST_ASSERT_FALSE(t.update(104, &v));
    TEST_ASSERT_TRUE(t.update(110, &v));
    TEST_ASSERT_EQUAL_INT(110, v);
}

void test_takeover_reaches_the_ends(void) {
    SoftTakeover t;
    t.arm(254, 254);
    int v = -1;
    TEST_ASSERT_TRUE(t.update(255, &v));
    TEST_ASSERT_EQUAL_INT(255, v);
    t.arm(1, 1);
    TEST_ASSERT_TRUE(t.update(0, &v));
    TEST_ASSERT_EQUAL_INT(0, v);
}

void test_takeover_drops_when_someone_else_changes_it(void) {
    SoftTakeover t;
    t.arm(100, 100);
    int v = -1;
    TEST_ASSERT_TRUE(t.update(150, &v));
    t.setReal(152, 150);                     // the poll agrees: still picked up
    TEST_ASSERT_TRUE(t.engaged());
    t.setReal(30, 150);                      // changed elsewhere
    TEST_ASSERT_FALSE(t.engaged());
    TEST_ASSERT_FALSE(t.update(140, &v));
    TEST_ASSERT_TRUE(t.update(32, &v));
    TEST_ASSERT_EQUAL_INT(32, v);
}

// A poll with the same level arriving on the frame the slider jumped across
// it (the app reads polls before the slider) must not erase the crossing.
void test_takeover_unchanged_poll_between_move_and_pickup(void) {
    SoftTakeover t;
    t.arm(128, 140);
    int v = -1;
    TEST_ASSERT_FALSE(t.update(140, &v));
    t.setReal(128, 100);                     // same level, slider already past it
    TEST_ASSERT_TRUE(t.update(100, &v));     // still a crossing: picked up
    TEST_ASSERT_EQUAL_INT(100, v);
    // A poll with a different level does re-arm from where the slider is.
    SoftTakeover u;
    u.arm(128, 140);
    u.setReal(90, 100);
    TEST_ASSERT_FALSE(u.update(100, &v));
    TEST_ASSERT_TRUE(u.update(85, &v));
}

void test_takeover_off_light(void) {
    SoftTakeover t;
    t.arm(0, 180);                           // an off light reads 0
    int v = -1;
    TEST_ASSERT_FALSE(t.update(100, &v));
    TEST_ASSERT_TRUE(t.update(4, &v));
    TEST_ASSERT_TRUE(t.update(60, &v));
    TEST_ASSERT_EQUAL_INT(60, v);
}

// ---- send gate and poll guard -----------------------------------------------

void test_send_gate_latest_wins_at_most_five_per_second(void) {
    SendGate g;
    TEST_ASSERT_FALSE(g.ready(0, false));    // nothing to send
    g.markDirty();
    TEST_ASSERT_TRUE(g.ready(1000, false));
    TEST_ASSERT_FALSE(g.ready(1000, true));  // the previous send is still out
    g.sent(1000);
    TEST_ASSERT_FALSE(g.dirty());
    g.markDirty();
    g.markDirty();                           // more changes meanwhile: one send
    TEST_ASSERT_FALSE(g.ready(1100, false));
    TEST_ASSERT_FALSE(g.ready(1199, false));
    TEST_ASSERT_TRUE(g.ready(1200, false));
    int sends = 0;
    for (uint32_t t = 2000; t < 3000; t += 20) {
        g.markDirty();
        if (g.ready(t, false)) { g.sent(t); sends++; }
    }
    TEST_ASSERT_EQUAL_INT(5, sends);
}

void test_poll_guard_holds_a_touched_control(void) {
    PollGuard p;
    p.reset();
    TEST_ASSERT_TRUE(p.accepts(PollGuard::Brightness, 0));
    p.touched(PollGuard::Brightness, 10000);
    TEST_ASSERT_FALSE(p.accepts(PollGuard::Brightness, 10000));
    TEST_ASSERT_FALSE(p.accepts(PollGuard::Brightness, 11499));
    TEST_ASSERT_TRUE(p.accepts(PollGuard::Brightness, 11500));
    TEST_ASSERT_TRUE(p.accepts(PollGuard::Color, 10500));   // others unaffected
    p.reset();
    TEST_ASSERT_TRUE(p.accepts(PollGuard::Brightness, 10001));
}

// ---- hold-to-repeat ---------------------------------------------------------

void test_repeat_waits_then_accelerates(void) {
    RepeatAccel r;
    TEST_ASSERT_EQUAL_INT(0, r.steps(0));    // not held
    r.press(1000);
    TEST_ASSERT_EQUAL_INT(0, r.steps(1100));
    TEST_ASSERT_EQUAL_INT(0, r.steps(1399));
    TEST_ASSERT_EQUAL_INT(1, r.steps(1400)); // first repeat after the delay
    TEST_ASSERT_EQUAL_INT(0, r.steps(1420));
    TEST_ASSERT_EQUAL_INT(1, r.steps(1460));
    TEST_ASSERT_EQUAL_INT(5, r.steps(2300));
    TEST_ASSERT_EQUAL_INT(15, r.steps(3500));
    r.release();
    TEST_ASSERT_EQUAL_INT(0, r.steps(4000));
    // Total over a 3 s hold at 50 Hz is well past a full 0-255 sweep.
    r.press(0);
    int total = 0;
    for (uint32_t t = 0; t <= 3000; t += 20) total += r.steps(t);
    TEST_ASSERT_TRUE(total > 255);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_entity_line_full_light);
    RUN_TEST(test_entity_line_off_light_has_no_colour);
    RUN_TEST(test_entity_line_scene_and_script);
    RUN_TEST(test_entity_line_unavailable);
    RUN_TEST(test_entity_line_trims_carriage_return_and_spaces);
    RUN_TEST(test_entity_line_rejects_malformed);
    RUN_TEST(test_entity_name_is_cut_to_fit);
    RUN_TEST(test_empty_name_falls_back_to_id);
    RUN_TEST(test_entities_body_skips_bad_lines_and_caps);
    RUN_TEST(test_overlong_line_is_dropped);
    RUN_TEST(test_area_lines);
    RUN_TEST(test_effect_lines_skip_names_too_long_to_send_back);
    RUN_TEST(test_effect_names_round_trip_exactly);
    RUN_TEST(test_json_string_decode_rejects);
    RUN_TEST(test_json_string_decode_checks_raw_utf8);
    RUN_TEST(test_secure_wipe);
    RUN_TEST(test_row_template_tolerates_missing_attributes);
    RUN_TEST(test_dump_templates);
    RUN_TEST(test_takeover_unchanged_poll_between_move_and_pickup);
    RUN_TEST(test_ids);
    RUN_TEST(test_base_url);
    RUN_TEST(test_token_chars);
    RUN_TEST(test_json_escape);
    RUN_TEST(test_template_bodies);
    RUN_TEST(test_service_calls);
    RUN_TEST(test_classify);
    RUN_TEST(test_hex_and_percent);
    RUN_TEST(test_presets);
    RUN_TEST(test_mirror_color);
    RUN_TEST(test_list_cursor);
    RUN_TEST(test_takeover_no_change_on_open);
    RUN_TEST(test_takeover_picks_up_at_level_then_drives);
    RUN_TEST(test_takeover_picks_up_on_crossing);
    RUN_TEST(test_takeover_engaged_at_open_waits_for_movement);
    RUN_TEST(test_takeover_reaches_the_ends);
    RUN_TEST(test_takeover_drops_when_someone_else_changes_it);
    RUN_TEST(test_takeover_off_light);
    RUN_TEST(test_send_gate_latest_wins_at_most_five_per_second);
    RUN_TEST(test_poll_guard_holds_a_touched_control);
    RUN_TEST(test_repeat_waits_then_accelerates);
    return UNITY_END();
}
