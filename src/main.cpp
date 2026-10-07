#include <Arduino.h>
#include <Audio.h>
#include <SD.h>
#include "EspBridge.h"
#include "AudioPlaySdWavMulti.h"
#include "RamPlayer.h"
#include "ConfigLoader.h"
#include "LedAnimator.h"
#include "UsbMaintenance.h"

// ── Audio graph ───────────────────────────────────────────────
// PCM5102A on I2S1 (no codec/I2C — pure I2S-in, analog-out).
// MAX98357A on I2S2 for haptics, replacing the old PWM output —
// both confirmed working, see audioTest/ in the project dir.
AudioPlaySdWavMulti   multiPlayer;
AudioMixer4           stemMixerL;
AudioMixer4           stemMixerR;
AudioMixer4           mainMixerL;
AudioMixer4           mainMixerR;
AudioMixer4           hapticMixer;
AudioAnalyzePeak      hapticPeak;
AudioEffectWaveshaper hapticShaper;   // soft limiter in front of the MAX98357A
AudioOutputI2S        i2sOut;      // PCM5102A — same pins/config as before
AudioOutputI2S2       i2s2Out;     // MAX98357A — replaces AudioOutputPWM
RamPlayer             ramFx;

AudioConnection  pBaseL  (multiPlayer, 0, stemMixerL, 0);
AudioConnection  pBaseR  (multiPlayer, 1, stemMixerR, 0);
AudioConnection  pNarrL  (multiPlayer, 2, stemMixerL, 1);
AudioConnection  pNarrR  (multiPlayer, 2, stemMixerR, 1);
AudioConnection  pOv1L   (multiPlayer, 4, stemMixerL, 2);
AudioConnection  pOv1R   (multiPlayer, 5, stemMixerR, 2);
AudioConnection  pOv2L   (multiPlayer, 6, stemMixerL, 3);
AudioConnection  pOv2R   (multiPlayer, 7, stemMixerR, 3);
AudioConnection  pStemL  (stemMixerL,  0, mainMixerL, 0);
AudioConnection  pStemR  (stemMixerR,  0, mainMixerR, 0);
AudioConnection  pFxL    (ramFx.player,0, mainMixerL, 1);
AudioConnection  pFxR    (ramFx.player,0, mainMixerR, 1);
AudioConnection  pOutL   (mainMixerL,  0, i2sOut,     0);
AudioConnection  pOutR   (mainMixerR,  0, i2sOut,     1);
AudioConnection  pHaptic (multiPlayer, 3, hapticMixer, 0);
AudioConnection  pHapShape(hapticMixer, 0, hapticShaper, 0);
AudioConnection  pHapOutL(hapticShaper, 0, i2s2Out,    0);  // duplicated
AudioConnection  pHapOutR(hapticShaper, 0, i2s2Out,    1);  // mono -> L+R
AudioConnection  pHapPeak(hapticMixer, 0, hapticPeak,  0);  // LED ring tap: BEFORE the limiter

// ── Bridge + LED ──────────────────────────────────────────────
LedAnimator leds;
EspBridge   bridge;

// ── Config ────────────────────────────────────────────────────
#define BOOT_LED_PIN     13   // Teensy onboard LED — SD-mount-failure indicator only
#define EXPERIENCE_FILE  "experience.wav"
#define GAIN_STEP        0.02f
#define GAIN_TICK_MS     20      // period of the gain ramp (overlays, seek fades, ducking)
#define DUCK_ATTACK_MS   40      // general audio drops this fast when an effect starts
#define DUCK_RELEASE_MS  400     // ... and comes back this slowly after it ends
#define HAPTIC_GAIN_DEFAULT   1.0f   // mixer gain in front of the limiter (>1.0 hard-clips)
#define HAPTIC_DRIVE_DEFAULT  0.5f   // soft-limiter drive k; 0 = off, see setHapticDrive()

ExperienceConfig     cfg;
const ChapterConfig* currentChapter = nullptr;

// ── Overlay state ─────────────────────────────────────────────
float narrCurrent = 0.0f, narrTarget = 0.0f;
float ov1Current  = 0.0f, ov1Target  = 0.0f;
float ov2Current  = 0.0f, ov2Target  = 0.0f;

// ── Seek crossfade ────────────────────────────────────────────
enum class SeekState : uint8_t { IDLE, FADE_OUT, SEEK, FADE_IN };
SeekState  seekState    = SeekState::IDLE;
uint32_t   seekTargetMs = 0;
float      stemGain     = 1.0f;
#define    SEEK_FADE_STEP 0.05f

bool experienceStarted = false;

// ── Timed ending ──────────────────────────────────────────────
// A START tag begins the clock. cfg.showEndMs later (config "end_after_min")
// the show wraps up: the chapter that is playing finishes first (a looping one
// finishes its current pass), then the end chapter plays, then sound and LEDs
// go off. Only the start tag (or a reboot) begins it again.
bool     showRunning  = false;   // a start tag was seen, the clock is running
bool     showEnding   = false;   // time is up: finish this chapter, then the end chapter
bool     showOver     = false;   // the end chapter finished: sound and LEDs are off
bool     loopReleased = false;   // a looping chapter was told to stop at its loop end
uint32_t showStartMs  = 0;

elapsedMillis gainRampTimer;
elapsedMillis statusTimer;
elapsedMillis tagCooldown[MAX_TAGS];  // per-tag retrigger cooldown
char    serialBuf[32] = {};
uint8_t serialPos = 0;

// ── Helpers ───────────────────────────────────────────────────
float stepGain(float cur, float tgt) {
    if (fabsf(cur - tgt) <= GAIN_STEP) return tgt;
    return cur + (tgt > cur ? GAIN_STEP : -GAIN_STEP);
}

// Ducking: while a RAM effect plays the general audio is lowered to 'level'
// (config "duck_level", 1.0 = off), quickly down and slowly back up. One step
// per GAIN_TICK_MS; the result multiplies the stem gain on mainMixer channel 0,
// so it works together with the seek fades. Pure function, kept separate so it
// can be tested.
float duckCurrent = 1.0f;

float duckNext(float cur, float level, bool fxPlaying) {
    float span = 1.0f - level;
    if (span <= 0.0f) return 1.0f;                       // ducking off
    float target = fxPlaying ? level : 1.0f;
    float step   = span * GAIN_TICK_MS / (target < cur ? DUCK_ATTACK_MS : DUCK_RELEASE_MS);
    if (fabsf(cur - target) <= step) return target;
    return cur + (target > cur ? step : -step);
}

void applyOverlayDefaults(const ChapterConfig& ch) {
    if (ch.overlayMode == OverlayMode::KEEP) {
        Serial.println("[Chapter] Keeping overlay state");
        return;
    }
    ov1Target  = ch.overlays.ov1  ? 1.0f : 0.0f;
    ov2Target  = ch.overlays.ov2  ? 1.0f : 0.0f;
    narrTarget = ch.overlays.narr ? 1.0f : 0.0f;
    Serial.printf("[Chapter] Overlays: ov1=%s ov2=%s narr=%s\n",
                  ch.overlays.ov1  ? "ON":"OFF",
                  ch.overlays.ov2  ? "ON":"OFF",
                  ch.overlays.narr ? "ON":"OFF");
}

void goToChapter(const char* id) {
    const ChapterConfig* ch = ConfigLoader::findChapter(id, cfg);
    if (!ch) { Serial.printf("[Chapter] '%s' not found\n", id); return; }
    currentChapter = ch;
    loopReleased   = false;
    seekTargetMs   = ch->startMs;
    seekState      = SeekState::FADE_OUT;
    if (ch->onEnterFx >= 0) ramFx.play(ch->onEnterFx);
    applyOverlayDefaults(*ch);
    if (ch->looping) {
        multiPlayer.setLoop(ch->startMs, ch->loopEndMs);
        Serial.printf("[Chapter] -> '%s' LOOP %lu-%lu ms\n", ch->id, ch->startMs, ch->loopEndMs);
    } else {
        multiPlayer.setPlayOnce(ch->startMs, ch->loopEndMs);
        Serial.printf("[Chapter] -> '%s' ONCE %lu-%lu ms\n", ch->id, ch->startMs, ch->loopEndMs);
    }
    leds.setBackgroundFromParams(ch->ledBackground);
    leds.triggerForegroundFromParams(ch->ledEnter);
}

void applyOverlayAction(const char* target, OverlayValue val) {
    float* tgt = nullptr;
    if      (strcmp(target, "ov1")  == 0) tgt = &ov1Target;
    else if (strcmp(target, "ov2")  == 0) tgt = &ov2Target;
    else if (strcmp(target, "narr") == 0) tgt = &narrTarget;
    else { Serial.printf("[Overlay] Unknown: %s\n", target); return; }
    switch (val) {
        case OverlayValue::ON:     *tgt = 1.0f; break;
        case OverlayValue::OFF:    *tgt = 0.0f; break;
        case OverlayValue::TOGGLE: *tgt = (*tgt > 0.5f) ? 0.0f : 1.0f; break;
    }
    Serial.printf("[Overlay] %s -> %.0f\n", target, *tgt);
}

void returnToIdle() {
    experienceStarted = false;
    currentChapter    = nullptr;
    ov1Target = 0.0f; ov2Target = 0.0f; narrTarget = 0.0f;
    multiPlayer.stop();
    leds.setBackgroundFromParams(cfg.idleLed);
    Serial.println("[Idle] Experience ended — waiting for tag tap");
}

void startExperience() {
    if (experienceStarted) return;
    experienceStarted = true;
    if (!multiPlayer.play(EXPERIENCE_FILE)) {
        Serial.println("[Audio] ERROR: experience.wav not found");
        experienceStarted = false;
        return;
    }
    Serial.println("[Audio] Experience started");
}

// ── Feedback sounds + person-tag lock ─────────────────────────
// Three short effects, played from RAM (slots come from config.json "sounds"):
//   connect - a person tag was accepted      denied - a person tag was refused
//   ready   - a chapter ended, back at base
bool encounterLockOn = false;      // from config "encounter_lock"; 'lock on|off' changes it live

void playSound(int8_t slot) { if (slot >= 0) ramFx.play(slot); }

enum class TagVerdict : uint8_t { RUN, DENIED };

// Person tags start a chapter. While the show is ending they are refused (no
// new encounters). With the lock on they are only accepted while the base
// chapter is playing; with it off (testing) they work at any time.
TagVerdict judgeTag(const TagConfig& tag, bool lockOn,
                    const ChapterConfig* current, const char* baseId, bool ending) {
    if (tag.type != TagType::PERSON) return TagVerdict::RUN;
    if (ending)  return TagVerdict::DENIED;
    if (!lockOn) return TagVerdict::RUN;
    return (current && strcmp(current->id, baseId) == 0) ? TagVerdict::RUN : TagVerdict::DENIED;
}

void dispatchTag(const TagConfig& tag, const char* tappedUid = nullptr) {
    Serial.printf("[Tag] '%s' (%s)\n", tag.label, tappedUid ? tappedUid : tag.uid);
    for (uint8_t i = 0; i < tag.actionCount; i++) {
        const Action& a = tag.actions[i];
        switch (a.type) {
            case ActionType::CHAPTER: goToChapter(a.target); break;
            case ActionType::OVERLAY: applyOverlayAction(a.target, a.overlayValue); break;
            case ActionType::FX:      if (a.fxSlot >= 0) ramFx.play(a.fxSlot); break;
            case ActionType::LED:     leds.triggerForegroundFromParams(a.ledParams); break;
        }
    }
}

const char* tagTypeName(TagType t) {
    return t == TagType::PERSON ? "person" : t == TagType::START ? "start"
         : t == TagType::DEVICE ? "device" : "location";
}

// ── Timed ending: the functions ───────────────────────────────
void beginShow() {                       // the start tag was seen (or 'show start')
    showStartMs  = millis();
    showRunning  = true;
    showEnding   = false;
    showOver     = false;
    loopReleased = false;
    if (cfg.showEndMs > 0)
        Serial.printf("[Show] started - '%s' follows after %.1f min\n", cfg.endChapter, cfg.showEndMs / 60000.0f);
    else
        Serial.println("[Show] started (no end time set)");
}

// Time is up (or 'show end'): let the current chapter finish, then the end chapter.
void triggerShowEnd() {
    if (showEnding || showOver) return;
    showEnding = true;
    Serial.printf("[Show] time is up - '%s' follows when the current chapter has finished\n", cfg.endChapter);
    // A looping chapter has no end of its own: let this pass run out.
    if (currentChapter && currentChapter->looping && strcmp(currentChapter->id, cfg.endChapter) != 0) {
        multiPlayer.setPlayOnce(currentChapter->startMs, currentChapter->loopEndMs);
        loopReleased = true;
    }
}

void showTick() {                        // call every loop()
    if (showRunning && !showEnding && !showOver && cfg.showEndMs > 0 &&
        millis() - showStartMs >= cfg.showEndMs)
        triggerShowEnd();
}

// The end chapter has finished: everything off.
void endShow() {
    showRunning = false; showEnding = false; showOver = true; loopReleased = false;
    experienceStarted = false;
    currentChapter    = nullptr;
    ov1Target = 0.0f; ov2Target = 0.0f; narrTarget = 0.0f;
    multiPlayer.stop();                                  // silence (the haptics too)
    leds.setBackground("off", 0x000000, 0.0f, 0.0f);     // ring dark
    Serial.println("[Show] over - sound and LEDs off. Tap the start tag (or reboot) to begin again");
}

void printShow() {
    if (showOver) { Serial.println("[Show] over - sound and LEDs off; tap the start tag or reboot"); return; }
    if (showEnding) {
        Serial.printf("[Show] time is up - '%s' follows when '%s' has finished\n",
                      cfg.endChapter, currentChapter ? currentChapter->id : "-");
    } else if (showRunning && cfg.showEndMs > 0) {
        float el = (millis() - showStartMs) / 60000.0f, tot = cfg.showEndMs / 60000.0f;
        Serial.printf("[Show] running %.1f of %.1f min - the end starts in %.1f min (the chapter then playing finishes first)\n",
                      el, tot, tot > el ? tot - el : 0.0f);
    } else if (showRunning) {
        Serial.println("[Show] running, no end time set");
    } else if (cfg.showEndMs > 0) {
        Serial.printf("[Show] waiting for the start tag (the end follows %.1f min after it)\n", cfg.showEndMs / 60000.0f);
    } else {
        Serial.println("[Show] waiting for the start tag; no end time set");
    }
}

// ── Haptic soft limiter ───────────────────────────────────────
// hapticMixer -> hapticShaper -> I2S2 (MAX98357A). The shaper pushes every
// sample through y = tanh(k*x) / tanh(k): full scale in is still full scale
// out, but quieter parts get louder (small-signal gain k/tanh(k)) and peaks
// round off instead of hard-clipping. k = 0 is a straight line (bypass).
//
// An AudioEffectWaveshaper with no table outputs SILENCE, so setup() must
// install one before any audio is expected.
#define HAPTIC_SHAPE_POINTS 1025          // library requires 2^n + 1
float hapticGain    = HAPTIC_GAIN_DEFAULT;
float hapticDrive   = HAPTIC_DRIVE_DEFAULT;
float hapticPeakMax = 0.0f;               // loudest source peak seen (pre-limiter)

// Pure maths, kept separate so it can be tested: fills n points over -1..+1.
void buildHapticTable(float* table, int n, float k) {
    const bool  bypass = (k < 0.01f);
    const float norm   = bypass ? 1.0f : tanhf(k);
    for (int i = 0; i < n; i++) {
        float x = -1.0f + 2.0f * (float)i / (float)(n - 1);
        table[i] = bypass ? x : tanhf(k * x) / norm;
    }
}

void setHapticDrive(float k) {
    static float table[HAPTIC_SHAPE_POINTS];
    if (k < 0.0f) k = 0.0f;
    if (k > 8.0f) k = 8.0f;
    buildHapticTable(table, HAPTIC_SHAPE_POINTS, k);
    // shape() frees and reallocates the table the audio interrupt reads,
    // so the audio update must not run while it swaps.
    AudioNoInterrupts();
    hapticShaper.shape(table, HAPTIC_SHAPE_POINTS);
    AudioInterrupts();
    hapticDrive = k;
}

void printHaptic() {
    float smallSignal = (hapticDrive < 0.01f) ? 1.0f : hapticDrive / tanhf(hapticDrive);
    Serial.printf("[Haptic] gain %.2f  drive %.2f (%+.1f dB small-signal, peaks capped at full scale)\n",
                  hapticGain, hapticDrive, 20.0f * log10f(smallSignal));
    if (hapticPeakMax > 0.0f)
        Serial.printf("[Haptic] loudest source peak since boot/reset: %.2f (%.1f dBFS)\n",
                      hapticPeakMax, 20.0f * log10f(hapticPeakMax));
    else
        Serial.println("[Haptic] no source signal seen yet since boot/reset");
}

// ── USB maintenance mode ──────────────────────────────────────
// Stops playback (which closes experience.wav) so the PC can safely read
// and write the SD card over MTP. Leave with 'reboot' — that also reloads
// config.json.
void enterUsbMaintenance() {
    if (usbMaintActive()) { Serial.println("[USB] already in maintenance mode"); return; }
    Serial.println("[USB] Entering maintenance mode — stopping playback");
    multiPlayer.stop();                 // closes the experience file
    experienceStarted = false;
    currentChapter    = nullptr;
    seekState         = SeekState::IDLE;
    stemGain          = 1.0f;
    ov1Target = 0.0f; ov2Target = 0.0f; narrTarget = 0.0f;
    leds.setBackground("spin", 0x00CCFF, 0.8f, 0.5f);   // cyan spinner = maintenance
    usbMaintStart();
    Serial.println("[USB] SD card is shared over MTP as 'SD Card'. Type 'reboot' to leave.");
}

void handleSerialCommand(const char* cmd) {
    // In maintenance mode the card belongs to the PC — refuse everything
    // that could touch playback or the SD card.
    if (usbMaintActive() && strcmp(cmd, "reboot") != 0) {
        Serial.println("[USB] maintenance mode — only 'reboot' is available");
        return;
    }
    if      (strcmp(cmd, "usb")    == 0)   enterUsbMaintenance();
    else if (strcmp(cmd, "reboot") == 0)   { Serial.println("Rebooting..."); rebootTeensy(); }
    else if (strcmp(cmd, "haptic") == 0)   printHaptic();
    else if (strcmp(cmd, "haptic reset") == 0) { hapticPeakMax = 0.0f; Serial.println("[Haptic] peak reset"); }
    else if (strncmp(cmd, "hdrive ", 7) == 0) { setHapticDrive(atof(cmd + 7)); printHaptic(); }
    else if (strncmp(cmd, "hgain ", 6) == 0) {
        hapticGain = constrain(atof(cmd + 6), 0.0f, 4.0f);
        hapticMixer.gain(0, hapticGain);
        printHaptic();
        if (hapticGain > 1.0f) Serial.println("[Haptic] note: above 1.0 the MIXER hard-clips before the soft limiter — use hdrive for loudness");
    }
    else if (strncmp(cmd, "duck", 4) == 0 && (cmd[4] == '\0' || cmd[4] == ' ')) {
        const char* a = cmd + 4;
        while (*a == ' ') a++;
        if ((*a >= '0' && *a <= '9') || *a == '.') cfg.duckLevel = constrain(atof(a), 0.0f, 1.0f);
        else if (*a != '\0') { Serial.println("[Duck] usage: duck | duck <0-1>   (1 = off)"); return; }
        if (cfg.duckLevel >= 1.0f)
            Serial.println("[Duck] off - the general audio is not lowered while an effect plays");
        else
            Serial.printf("[Duck] general audio drops to %.2f (%.1f dB) while an effect plays; attack %d ms, release %d ms\n",
                          cfg.duckLevel, cfg.duckLevel > 0.001f ? 20.0f * log10f(cfg.duckLevel) : -99.0f,
                          DUCK_ATTACK_MS, DUCK_RELEASE_MS);
    }
    else if (strncmp(cmd, "show", 4) == 0 && (cmd[4] == '\0' || cmd[4] == ' ')) {
        const char* a = cmd + 4;
        while (*a == ' ') a++;
        if      (*a == '\0')           printShow();
        else if (!strcmp(a, "start"))  { if (!experienceStarted) startExperience(); goToChapter(cfg.baseChapter); beginShow(); printShow(); }
        else if (!strcmp(a, "end"))    { if (!experienceStarted) Serial.println("[Show] nothing is playing"); else { triggerShowEnd(); printShow(); } }
        else if ((*a >= '0' && *a <= '9') || *a == '.') {
            float m = atof(a);
            cfg.showEndMs = (m > 0.0f) ? (uint32_t)(m * 60000.0f + 0.5f) : 0;
            printShow();
        }
        else Serial.println("[Show] usage: show | show <minutes> | show start | show end");
    }
    else if (strncmp(cmd, "lock", 4) == 0 && (cmd[4] == '\0' || cmd[4] == ' ')) {
        const char* a = cmd + 4;
        while (*a == ' ') a++;
        if      (!strcmp(a, "on"))  encounterLockOn = true;
        else if (!strcmp(a, "off")) encounterLockOn = false;
        Serial.printf("[Lock] person tags are %s (base chapter '%s')\n",
                      encounterLockOn ? "only accepted in the base chapter" : "accepted at any time",
                      cfg.baseChapter);
    }
    else if (strncmp(cmd, "tag ", 4) == 0) {            // which role does this uid have?
        char up[20] = {};
        for (int i = 0; cmd[4 + i] && i < 19; i++) up[i] = (cmd[4+i] >= 'a' && cmd[4+i] <= 'z') ? cmd[4+i] - 32 : cmd[4+i];
        int8_t idx = ConfigLoader::findTagIndex(up, cfg);
        if (idx < 0) Serial.printf("[Tag] %s is not in the config\n", up);
        else         Serial.printf("[Tag] %s -> '%s' (%s)\n", up, cfg.tags[idx].label,
                                   tagTypeName(cfg.tags[idx].type));
    }
    else if (strncmp(cmd, "go ",  3) == 0) { if (!experienceStarted) startExperience(); goToChapter(cmd + 3); }
    else if (strncmp(cmd, "seek ",5) == 0) { seekTargetMs=atoi(cmd+5); seekState=SeekState::FADE_OUT; ramFx.play(1); }
    else if (strncmp(cmd, "fx ",  3) == 0) ramFx.play(atoi(cmd + 3));
    else if (strcmp(cmd,  "narr")   == 0)  { narrTarget = (narrTarget > 0.5f) ? 0.0f : 1.0f; }
    else if (strcmp(cmd,  "ov1")    == 0)  { ov1Target  = (ov1Target  > 0.5f) ? 0.0f : 1.0f; }
    else if (strcmp(cmd,  "ov2")    == 0)  { ov2Target  = (ov2Target  > 0.5f) ? 0.0f : 1.0f; }
    else if (strcmp(cmd, "chapters")== 0) {
        for (uint8_t i=0; i<cfg.chapterCount; i++) {
            const ChapterConfig& c = cfg.chapters[i];
            Serial.printf("  [%u] '%s' %lu-%lu ms  %s  fx=%d  %s  led:%s  ->%s\n",
                i, c.id, c.startMs, c.loopEndMs,
                c.looping ? "LOOP" : "once",
                c.onEnterFx,
                c.overlayMode==OverlayMode::KEEP?"keep":"fresh",
                c.ledBackground.animation,
                strlen(c.returnsTo) > 0 ? c.returnsTo : "(idle)");
        }
    } else if (strcmp(cmd, "tags") == 0) {
        for (uint8_t i=0; i<cfg.tagCount; i++) {
            const TagConfig& t = cfg.tags[i];
            Serial.printf("  [%u] %s (%u uid%s) %s '%s' %u actions\n",
                i, t.uid, t.uidCount, t.uidCount == 1 ? "" : "s",
                tagTypeName(t.type),
                t.label, t.actionCount);
        }
    } else if (strcmp(cmd, "bridge") == 0) {
        Serial.printf("[Bridge] alive:%s  last msg %lums ago  dist:%u mm  touch:%u\n",
                      bridge.isAlive() ? "yes" : "no",
                      bridge.lastMessageAgeMs(),
                      bridge.lastProximityMm(),
                      bridge.lastTouchState());
    } else {
        Serial.println("Commands: go <id> | seek <ms> | fx <n> | narr | ov1 | ov2 | chapters | tags | bridge | haptic | hdrive <k> | hgain <g> | duck [0-1] | show [min|start|end] | lock [on|off] | tag <uid> | usb | reboot");
    }
}

// ── Setup ─────────────────────────────────────────────────────
void setup() {
    pinMode(BOOT_LED_PIN, OUTPUT);
    Serial.begin(115200);
    delay(500);
    Serial.println("=== Interactive Audio Player ===");

    leds.begin();
    AudioMemory(40);
    // No codec object — PCM5102A needs no I2C init, pure I2S-in/analog-out.
    // MAX98357A (I2S2) likewise needs no software setup; its SD/GAIN pins
    // are configured entirely in hardware (SD tied to VIN, GAIN floating).

    stemMixerL.gain(0,1.0f); stemMixerR.gain(0,1.0f);
    stemMixerL.gain(1,0.0f); stemMixerR.gain(1,0.0f);
    stemMixerL.gain(2,0.0f); stemMixerR.gain(2,0.0f);
    stemMixerL.gain(3,0.0f); stemMixerR.gain(3,0.0f);
    mainMixerL.gain(0,1.0f); mainMixerR.gain(0,1.0f);
    mainMixerL.gain(1,1.0f); mainMixerR.gain(1,1.0f);
    mainMixerL.gain(2,0.0f); mainMixerR.gain(2,0.0f);
    mainMixerL.gain(3,0.0f); mainMixerR.gain(3,0.0f);
    hapticMixer.gain(0, hapticGain);
    setHapticDrive(hapticDrive);          // must run before audio: no table = silence

    if (!SD.sdfs.begin(SdioConfig(FIFO_SDIO))) {
        Serial.println("[SD] mount failed");
        while (true) { digitalWrite(BOOT_LED_PIN, !digitalRead(BOOT_LED_PIN)); delay(100); }
    }
    Serial.println("[SD] mounted OK");

    ConfigLoader::load(cfg);
    encounterLockOn = cfg.encounterLock;
    if (cfg.showEndMs > 0 && !ConfigLoader::findChapter(cfg.endChapter, cfg))
        Serial.printf("[Show] WARNING: end chapter '%s' is not in the config - the timed ending cannot work\n", cfg.endChapter);
    leds.setGlobalBrightness(cfg.ledBrightness);
    leds.loadAnimations();

    ramFx.load(0, "fx_dev.wav");
    ramFx.load(1, "fx_loc_a.wav");
    ramFx.load(2, "fx_loc_b.wav");
    ramFx.load(3, "fx_boot.wav");
    ramFx.load(4, "fx_connect.wav");   // person tag accepted
    ramFx.load(5, "fx_denied.wav");    // person tag refused
    ramFx.load(6, "fx_ready.wav");     // chapter ended, ready for the next
    ramFx.printMemoryUsage();

    // ESP32 body bridge — forwards hand-unit proximity + NFC over UART
    bridge.begin(Serial1, 115200);

    // Auto-start straight into the configured start chapter (e.g.
    // 'start_arrival') — the experience plays continuously from power-on,
    // it does not wait for a first tag tap.
    Serial.printf("[Boot] Auto-starting -> '%s'\n", cfg.startChapter);
    startExperience();
    goToChapter(cfg.startChapter);

    Serial.println("Type 'chapters' or 'tags' to inspect. Tap a tag on the hand unit to navigate.");
}

// ── Loop ──────────────────────────────────────────────────────
void loop() {

    if (usbMaintActive()) usbMaintUpdate();   // keep MTP responsive

    // Drive the ring from live haptic level — fast attack, slow decay
    if (hapticPeak.available()) {
        float p = hapticPeak.read();
        leds.setBeatLevel(p);
        if (p > hapticPeakMax) hapticPeakMax = p;
    }
    leds.update();

    if (gainRampTimer >= GAIN_TICK_MS) {
        gainRampTimer = 0;

        narrCurrent = stepGain(narrCurrent, narrTarget);
        ov1Current  = stepGain(ov1Current,  ov1Target);
        ov2Current  = stepGain(ov2Current,  ov2Target);
        duckCurrent = duckNext(duckCurrent, cfg.duckLevel, ramFx.isPlaying());
        stemMixerL.gain(1, narrCurrent); stemMixerR.gain(1, narrCurrent);
        stemMixerL.gain(2, ov1Current);  stemMixerR.gain(2, ov1Current);
        stemMixerL.gain(3, ov2Current);  stemMixerR.gain(3, ov2Current);

        switch (seekState) {
            case SeekState::FADE_OUT:
                stemGain -= SEEK_FADE_STEP;
                if (stemGain <= 0.0f) { stemGain = 0.0f; seekState = SeekState::SEEK; }
                break;
            case SeekState::SEEK:
                multiPlayer.seekMs(seekTargetMs);
                seekState = SeekState::FADE_IN;
                break;
            case SeekState::FADE_IN:
                stemGain += SEEK_FADE_STEP;
                if (stemGain >= 1.0f) { stemGain=1.0f; seekState=SeekState::IDLE; }
                break;
            default: break;
        }
        // stems x seek fade x ducking
        mainMixerL.gain(0, stemGain * duckCurrent); mainMixerR.gain(0, stemGain * duckCurrent);
    }

    showTick();   // timed ending: has the show run long enough?

    // Detect end of a chapter that plays once (a non-looping one, or a loop
    // released for the end of the show) → end chapter, returns_to, or idle
    if (experienceStarted &&
        currentChapter && (!currentChapter->looping || loopReleased) &&
        seekState == SeekState::IDLE &&
        !multiPlayer.isPlaying()) {
        if (strcmp(currentChapter->id, cfg.endChapter) == 0) {
            endShow();                                  // the end chapter is done: sound + LEDs off
        } else if (showEnding) {
            if (ConfigLoader::findChapter(cfg.endChapter, cfg)) {
                Serial.printf("[Chapter] '%s' finished -> '%s' (time is up)\n",
                              currentChapter->id, cfg.endChapter);
                goToChapter(cfg.endChapter);            // no 'ready' sound: nothing follows
            } else {
                Serial.printf("[Show] end chapter '%s' not found - ending without it\n", cfg.endChapter);
                endShow();
            }
        } else if (strlen(currentChapter->returnsTo) > 0) {
            Serial.printf("[Chapter] '%s' finished -> '%s'\n",
                          currentChapter->id, currentChapter->returnsTo);
            if (strcmp(currentChapter->returnsTo, cfg.baseChapter) == 0) playSound(cfg.soundReady);
            goToChapter(currentChapter->returnsTo);
        } else {
            returnToIdle();
        }
    }

    // ── ESP32 body bridge — confirmed tag connection events ───
    // Non-blocking line reader; near-instant, no polling interval needed.
    char uid[20] = {};
    if (bridge.poll(uid) && !usbMaintActive()) {
        int8_t idx = ConfigLoader::findTagIndex(uid, cfg);
        if (idx < 0) {
            Serial.printf("[Bridge] Unknown tag: %s -- add to config.json\n", uid);
        } else {
            const TagConfig& tag = cfg.tags[idx];
            if (tag.cooldownMs > 0 && tagCooldown[idx] < tag.cooldownMs) {
                uint32_t remaining = tag.cooldownMs - tagCooldown[idx];
                Serial.printf("[Tag] '%s' cooling down (%lums left)\n",
                              tag.label, remaining);
            } else {
                tagCooldown[idx] = 0;  // reset cooldown
                if (showOver && tag.type != TagType::START) {
                    Serial.printf("[Tag] '%s' ignored - the show is over (tap the start tag to begin again)\n", tag.label);
                } else if (judgeTag(tag, encounterLockOn, currentChapter, cfg.baseChapter, showEnding) == TagVerdict::DENIED) {
                    if (showEnding)
                        Serial.printf("[Tag] '%s' ignored - the show is ending\n", tag.label);
                    else
                        Serial.printf("[Tag] '%s' refused - only accepted in '%s' (now in '%s')\n",
                                      tag.label, cfg.baseChapter, currentChapter ? currentChapter->id : "-");
                    playSound(cfg.soundDenied);
                } else {
                    if (!experienceStarted) startExperience();
                    dispatchTag(tag, uid);
                    if (tag.type == TagType::PERSON) playSound(cfg.soundConnect);
                    if (tag.type == TagType::START)  beginShow();
                }
            }
        }
    }

    while (Serial.available()) {
        char c = Serial.read();
        if (c == '\n' || c == '\r') {
            if (serialPos > 0) {
                serialBuf[serialPos] = '\0';
                serialPos = 0;
                handleSerialCommand(serialBuf);
            }
        } else if (serialPos < sizeof(serialBuf) - 1) {
            serialBuf[serialPos++] = c;
        }
    }

    if (statusTimer >= 3000) {
        statusTimer = 0;
        if (!usbMaintActive()) Serial.printf("[%6lu ms] ch:%s pos:%lu/%lu narr:%.2f ov1:%.2f ov2:%.2f  bridge:%s  CPU:%.1f%%  mem:%u/%u\n",
                      millis(),
                      currentChapter ? currentChapter->id : "none",
                      multiPlayer.positionMs(), multiPlayer.lengthMs(),
                      narrCurrent, ov1Current, ov2Current,
                      bridge.isAlive() ? "ok" : "DOWN",
                      AudioProcessorUsage(),
                      AudioMemoryUsage(), AudioMemoryUsageMax());
    }
}