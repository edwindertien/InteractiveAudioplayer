#pragma once

// ============================================================
// ConfigLoader
// Parses /config.json from SD card into typed structs.
// Audio designers edit only the JSON — no reflashing needed.
//
// Call ConfigLoader::load(config) once after SD.begin().
// ============================================================

#include <SD.h>
#include <ArduinoJson.h>
#include "LedAnimator.h"

// ── Limits ───────────────────────────────────────────────────
static constexpr uint8_t MAX_CHAPTERS  = 16;
static constexpr uint8_t MAX_TAGS      = 32;
static constexpr uint8_t MAX_ACTIONS   = 6;
static constexpr uint8_t MAX_TAG_UIDS  = 8;    // physical tags that can share one role

// ── Action ───────────────────────────────────────────────────
enum class ActionType : uint8_t {
    CHAPTER,    // seek to chapter, apply its settings
    OVERLAY,    // set ov1 / ov2 / narr to on/off/toggle
    FX,         // play a RAM FX slot
    LED,        // trigger a foreground LED flash on the ring
};

enum class OverlayValue : uint8_t { ON, OFF, TOGGLE };

struct Action {
    ActionType   type;
    char         target[16];    // chapter id  OR  "ov1"/"ov2"/"narr"
    OverlayValue overlayValue;  // for OVERLAY actions
    int8_t       fxSlot;        // for FX actions (-1 = none)
    LedParams    ledParams;     // for LED actions
};

// ── Overlay defaults for a chapter ───────────────────────────
enum class OverlayMode : uint8_t {
    FRESH,   // apply chapter's overlay defaults on entry
    KEEP,    // keep whatever overlays are currently active
};

struct OverlayDefaults {
    bool ov1  = false;
    bool ov2  = false;
    bool narr = false;
};

// ── Chapter ───────────────────────────────────────────────────
struct ChapterConfig {
    char            id[16]      = {};
    char            label[32]   = {};
    uint32_t        startMs     = 0;
    uint32_t        loopEndMs   = 0;    // 0 = end of file
    int8_t          onEnterFx   = -1;   // RAM FX slot, -1 = none
    OverlayMode     overlayMode = OverlayMode::FRESH;
    OverlayDefaults overlays;
    bool            looping     = true; // false = play once then return to idle
    LedParams       ledBackground;      // persistent ring pattern — usually
                                        // 'level' or 'heartbeat' for haptic
                                        // sync, but any pattern works
    LedParams       ledEnter;           // one-shot ring flash on chapter entry
    char            returnsTo[16] = {}; // chapter id to jump to on natural end
                                        // (non-looping chapters only).
                                        // empty = end the experience (idle)
};

// ── Tag ──────────────────────────────────────────────────────
enum class TagType : uint8_t { LOCATION, DEVICE, PERSON, START };   // START = the tag that begins the show

struct TagConfig {
    char        uid[20]     = {};    // FIRST uid, uppercase hex e.g. "04A32B1C" (for logs)
    char        uids[MAX_TAG_UIDS][20] = {};   // every physical tag with this role
    uint8_t     uidCount    = 0;
    char        label[48]   = {};    // human-readable, for debug only
    TagType     type        = TagType::LOCATION;
    uint32_t    cooldownMs  = 0;     // 0 = no cooldown, else ms before re-trigger allowed
    Action      actions[MAX_ACTIONS];
    uint8_t     actionCount = 0;
};

// ── Global config ─────────────────────────────────────────────
struct ExperienceConfig {
    char          deviceId[16]      = "UNIT_01";
    char          startChapter[16]  = "intro";
    uint8_t       ledBrightness     = 160;   // 0-255 global LED scale
    LedParams     idleLed;           // ring pattern shown before experience starts
    ChapterConfig chapters[MAX_CHAPTERS];
    uint8_t       chapterCount      = 0;
    TagConfig     tags[MAX_TAGS];
    uint8_t       tagCount          = 0;

    // Person tags (type "person") normally switch chapter at any time. With
    // encounter_lock they are only accepted while baseChapter is playing;
    // anywhere else they are refused (and the 'denied' sound plays).
    bool          encounterLock     = false;
    char          baseChapter[16]   = "base";

    // RAM FX slots for the three feedback sounds, -1 = silent.
    int8_t        soundConnect      = -1;   // a person tag was accepted
    int8_t        soundDenied       = -1;   // a person tag was refused
    int8_t        soundReady        = -1;   // a chapter ended, back at base

    // Timed ending: this long after the START tag was seen, the show wraps up -
    // the chapter that is playing finishes, then endChapter plays, then sound
    // and LEDs go off. 0 = never.
    uint32_t      showEndMs         = 0;
    char          endChapter[16]    = "end_reflection";

    // Ducking: while a RAM effect plays, the general audio is lowered to this
    // fraction (0.4 = -8 dB) so the effect can be heard. 1.0 = off.
    float         duckLevel         = 1.0f;
};

// ── Loader ────────────────────────────────────────────────────
class ConfigLoader {
public:
    // ── Which file to load ────────────────────────────────────
    //   1. /config.json, if the card has one
    //   2. otherwise the ONE file in the card root named *_config.json
    //      (e.g. player2_config.json), so a card can be labelled by name
    // Ignored: folders, hidden files (macOS adds "._player2_config.json" when
    // copying to a card) and names that merely contain the text. If several
    // files match, nothing is loaded — it never guesses which one you meant.

    // BEGIN config-path resolver
    static bool endsWithNoCase(const char* s, const char* suffix) {
        size_t ls = strlen(s), lx = strlen(suffix);
        if (ls < lx) return false;
        for (size_t i = 0; i < lx; i++) {
            char a = s[ls - lx + i], b = suffix[i];
            if (a >= 'A' && a <= 'Z') a += 32;
            if (b >= 'A' && b <= 'Z') b += 32;
            if (a != b) return false;
        }
        return true;
    }

    // true:  "player2_config.json"  "PLAYER2_CONFIG.JSON"
    // false: "config.json"  "._player2_config.json"  "x_config.json.bak"  "_config.json"
    static bool isNamedConfig(const char* name) {
        static const char* SUFFIX = "_config.json";
        if (name[0] == '.') return false;
        return strlen(name) > strlen(SUFFIX) && endsWithNoCase(name, SUFFIX);
    }

    // Writes the path to load into 'out'. Returns false (after logging why)
    // when there is nothing usable on the card.
    static bool resolveConfigPath(char* out, size_t outLen) {
        File probe = SD.open("/config.json");
        if (probe) {
            probe.close();
            strlcpy(out, "/config.json", outLen);
            return true;
        }

        char    names[4][48] = {};
        uint8_t count = 0;
        File root = SD.open("/");
        while (File e = root.openNextFile()) {
            const char* n = e.name();
            if (!e.isDirectory() && isNamedConfig(n)) {
                if (strlen(n) >= sizeof(names[0])) {
                    Serial.printf("[Config] ignoring over-long name: %s\n", n);
                } else {
                    if (count < 4) strlcpy(names[count], n, sizeof(names[0]));
                    count++;
                }
            }
            e.close();
        }
        root.close();

        if (count == 0) {
            Serial.println("[Config] ERROR: no /config.json and no *_config.json on the card");
            return false;
        }
        if (count > 1) {
            Serial.printf("[Config] ERROR: no /config.json and %u files match *_config.json"
                          " — keep one, or name the right one config.json:\n", count);
            for (uint8_t i = 0; i < count && i < 4; i++) Serial.printf("[Config]   %s\n", names[i]);
            return false;
        }
        snprintf(out, outLen, "/%s", names[0]);
        Serial.printf("[Config] No /config.json — using %s\n", out);
        return true;
    }
    // END config-path resolver

    // path == nullptr (the default): find the file as described above.
    static bool load(ExperienceConfig& cfg, const char* path = nullptr) {
        char resolved[64];
        if (!path) {
            if (!resolveConfigPath(resolved, sizeof(resolved))) return false;
            path = resolved;
        }
        File f = SD.open(path);
        if (!f) {
            Serial.printf("[Config] ERROR: cannot open %s\n", path);
            return false;
        }

        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, f);
        f.close();

        if (err) {
            Serial.printf("[Config] JSON error: %s\n", err.c_str());
            return false;
        }

        strlcpy(cfg.deviceId,      doc["device_id"]     | "UNIT_01", sizeof(cfg.deviceId));
        strlcpy(cfg.startChapter,  doc["start_chapter"] | "intro",   sizeof(cfg.startChapter));
        cfg.ledBrightness   = doc["led_brightness"]   | 160;
        cfg.encounterLock   = doc["encounter_lock"]   | false;
        strlcpy(cfg.baseChapter, doc["base_chapter"] | "base", sizeof(cfg.baseChapter));
        cfg.soundConnect    = doc["sounds"]["connect"] | -1;
        cfg.soundDenied     = doc["sounds"]["denied"]  | -1;
        cfg.soundReady      = doc["sounds"]["ready"]   | -1;
        { float mins = doc["end_after_min"] | 0.0f;
          cfg.showEndMs = (mins > 0.0f) ? (uint32_t)(mins * 60000.0f + 0.5f) : 0; }
        strlcpy(cfg.endChapter, doc["end_chapter"] | "end_reflection", sizeof(cfg.endChapter));
        cfg.duckLevel = doc["duck_level"] | 1.0f;
        if (cfg.duckLevel < 0.0f) cfg.duckLevel = 0.0f;
        if (cfg.duckLevel > 1.0f) cfg.duckLevel = 1.0f;

        // Idle LED — defaults to very dim slow breathe
        JsonObject idleLed = doc["idle"]["led"];
        if (!idleLed.isNull()) {
            parseLedParams(idleLed, cfg.idleLed);
        } else {
            strlcpy(cfg.idleLed.animation, "breathe", sizeof(cfg.idleLed.animation));
            cfg.idleLed.color     = 0x001133;
            cfg.idleLed.intensity = 0.12f;
            cfg.idleLed.speed     = 0.15f;
        }

        // ── Chapters ─────────────────────────────────────────
        cfg.chapterCount = 0;
        for (JsonObject ch : doc["chapters"].as<JsonArray>()) {
            if (cfg.chapterCount >= MAX_CHAPTERS) break;
            ChapterConfig& c = cfg.chapters[cfg.chapterCount++];

            strlcpy(c.id,    ch["id"]    | "unnamed", sizeof(c.id));
            strlcpy(c.label, ch["label"] | "",        sizeof(c.label));
            c.startMs    = ch["start_ms"]   | 0;
            c.loopEndMs  = ch["loop_end_ms"]| 0;
            c.onEnterFx  = ch["on_enter_fx"]| -1;
            strlcpy(c.returnsTo, ch["returns_to"] | "", sizeof(c.returnsTo));

            const char* mode = ch["overlay_mode"] | "fresh";
            c.overlayMode = (strcmp(mode, "keep") == 0)
                            ? OverlayMode::KEEP
                            : OverlayMode::FRESH;
            // Use explicit check — ArduinoJson's | operator treats false as "missing"
            c.looping = ch["loop"].is<bool>() ? ch["loop"].as<bool>() : true;

            JsonObject ov = ch["overlays"];
            if (!ov.isNull()) {
                c.overlays.ov1  = ov["ov1"]  | false;
                c.overlays.ov2  = ov["ov2"]  | false;
                c.overlays.narr = ov["narr"] | false;
            }

            // Ring background — persistent, usually haptic-sync
            JsonObject ledBg = ch["led_background"];
            if (!ledBg.isNull()) parseLedParams(ledBg, c.ledBackground);
            else {
                strlcpy(c.ledBackground.animation, "level", 20);
                c.ledBackground.color     = 0x0044FF;
                c.ledBackground.intensity = 0.6f;
                c.ledBackground.speed     = 0.4f;
            }
            // Ring foreground — one-shot flash on chapter entry
            JsonObject ledEn = ch["led_enter"];
            if (!ledEn.isNull()) parseLedParams(ledEn, c.ledEnter);
            else {
                strlcpy(c.ledEnter.animation, "flash", 20);
                c.ledEnter.color = 0xFFFFFF;
                c.ledEnter.intensity = 1.0f;
                c.ledEnter.speed = 0.8f;
                c.ledEnter.durationMs = 800;
            }
        }

        // ── Tags ─────────────────────────────────────────────
        cfg.tagCount = 0;
        for (JsonObject tag : doc["tags"].as<JsonArray>()) {
            if (cfg.tagCount >= MAX_TAGS) break;
            TagConfig& t = cfg.tags[cfg.tagCount++];

            strlcpy(t.label, tag["label"] | "", sizeof(t.label));

            // One role can have several physical tags (spares): either
            // "uid": "X" or "uids": ["X","Y",...] (or both). Stored uppercase.
            t.uidCount = 0;
            addUid(t, tag["uid"] | "");
            for (JsonVariant v : tag["uids"].as<JsonArray>()) addUid(t, v | "");
            strlcpy(t.uid, t.uidCount ? t.uids[0] : "", sizeof(t.uid));

            const char* type = tag["type"] | "location";
            t.type = (strcmp(type, "person") == 0) ? TagType::PERSON
                   : (strcmp(type, "start")  == 0) ? TagType::START
                   : (strcmp(type, "device") == 0) ? TagType::DEVICE
                   :                                 TagType::LOCATION;
            t.cooldownMs = tag["cooldown_ms"] | 0;

            t.actionCount = 0;
            for (JsonObject act : tag["actions"].as<JsonArray>()) {
                if (t.actionCount >= MAX_ACTIONS) break;
                Action& a = t.actions[t.actionCount++];

                const char* doStr = act["do"] | "";
                if (strcmp(doStr, "chapter") == 0) {
                    a.type = ActionType::CHAPTER;
                    strlcpy(a.target, act["target"] | "", sizeof(a.target));

                } else if (strcmp(doStr, "overlay") == 0) {
                    a.type = ActionType::OVERLAY;
                    strlcpy(a.target, act["target"] | "", sizeof(a.target));
                    const char* val = act["value"] | "toggle";
                    if      (strcmp(val, "on")  == 0) a.overlayValue = OverlayValue::ON;
                    else if (strcmp(val, "off") == 0) a.overlayValue = OverlayValue::OFF;
                    else                              a.overlayValue = OverlayValue::TOGGLE;

                } else if (strcmp(doStr, "fx") == 0) {
                    a.type   = ActionType::FX;
                    a.fxSlot = act["slot"] | -1;

                } else if (strcmp(doStr, "led") == 0) {
                    a.type = ActionType::LED;
                    parseLedParams(act, a.ledParams);
                }
            }
        }

        uint16_t uidTotal = 0;
        for (uint8_t i = 0; i < cfg.tagCount; i++) uidTotal += cfg.tags[i].uidCount;
        Serial.printf("[Config] Loaded: %u chapters, %u tags (%u uids), device=%s\n",
                      cfg.chapterCount, cfg.tagCount, uidTotal, cfg.deviceId);
        warnDuplicateUids(cfg);
        Serial.printf("[Config] encounter lock %s, base chapter '%s', sounds connect=%d denied=%d ready=%d\n",
                      cfg.encounterLock ? "ON" : "off", cfg.baseChapter,
                      cfg.soundConnect, cfg.soundDenied, cfg.soundReady);
        if (cfg.duckLevel < 1.0f)
            Serial.printf("[Config] ducking: general audio drops to %.2f while an effect plays\n", cfg.duckLevel);
        if (cfg.showEndMs > 0)
            Serial.printf("[Config] show ends %.1f min after the start tag -> '%s'\n",
                          cfg.showEndMs / 60000.0f, cfg.endChapter);
        return true;
    }

    static void parseLedParams(JsonObjectConst obj, LedParams& p) {
        strlcpy(p.animation, obj["animation"] | "level", sizeof(p.animation));
        const char* colorStr = obj["color"] | "0x0044FF";
        p.color     = (uint32_t)strtoul(colorStr, nullptr, 16);
        p.intensity = obj["intensity"] | 0.6f;
        p.speed     = obj["speed"]     | 0.4f;
        p.durationMs= obj["duration_ms"]| 0;
    }

    // Find a chapter by id
    static const ChapterConfig* findChapter(const char* id,
                                            const ExperienceConfig& cfg) {
        for (uint8_t i = 0; i < cfg.chapterCount; i++) {
            if (strcmp(cfg.chapters[i].id, id) == 0) return &cfg.chapters[i];
        }
        return nullptr;
    }

    // Find a tag by UID string — returns pointer
    static const TagConfig* findTag(const char* uid,
                                    const ExperienceConfig& cfg) {
        for (uint8_t i = 0; i < cfg.tagCount; i++) {
            if (strcmp(cfg.tags[i].uid, uid) == 0) return &cfg.tags[i];
        }
        return nullptr;
    }

    // Find a tag by UID string — returns index (-1 if not found)
    static int8_t findTagIndex(const char* uid, const ExperienceConfig& cfg) {
        for (uint8_t i = 0; i < cfg.tagCount; i++) {
            for (uint8_t u = 0; u < cfg.tags[i].uidCount; u++) {
                if (strcmp(cfg.tags[i].uids[u], uid) == 0) return (int8_t)i;
            }
        }
        return -1;
    }

    // Adds one uid to a tag: uppercased, empty/duplicate ignored.
    static void addUid(TagConfig& t, const char* uid) {
        if (!uid || !uid[0]) return;
        char up[20];
        size_t n = 0;
        for (; uid[n] && n < sizeof(up) - 1; n++)
            up[n] = (uid[n] >= 'a' && uid[n] <= 'z') ? uid[n] - 32 : uid[n];
        up[n] = '\0';
        for (uint8_t i = 0; i < t.uidCount; i++)
            if (strcmp(t.uids[i], up) == 0) return;
        if (t.uidCount >= MAX_TAG_UIDS) {
            Serial.printf("[Config] WARNING: tag '%s' has more than %u uids - ignoring %s\n",
                          t.label, MAX_TAG_UIDS, up);
            return;
        }
        strlcpy(t.uids[t.uidCount++], up, sizeof(t.uids[0]));
    }

    // The same physical tag listed under two roles: the first entry wins.
    static void warnDuplicateUids(const ExperienceConfig& cfg) {
        for (uint8_t i = 0; i < cfg.tagCount; i++)
            for (uint8_t u = 0; u < cfg.tags[i].uidCount; u++)
                for (uint8_t j = i + 1; j < cfg.tagCount; j++)
                    for (uint8_t v = 0; v < cfg.tags[j].uidCount; v++)
                        if (strcmp(cfg.tags[i].uids[u], cfg.tags[j].uids[v]) == 0)
                            Serial.printf("[Config] WARNING: uid %s is in both '%s' and '%s' - the first wins\n",
                                          cfg.tags[i].uids[u], cfg.tags[i].label, cfg.tags[j].label);
    }
};