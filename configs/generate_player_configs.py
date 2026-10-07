#!/usr/bin/env python3
"""
generate_player_configs.py — builds config.json for each of the 7 players.

All 7 players share the same chapter timeline (the same experience.wav sits
on every SD card). Only the `tags` section differs per player.

Every physical tag lives in tag_catalogue.json (next to this script), by role:

  players 1..7  person tags. For player N's card: the tags of player N start
                'recharge'; the tags of each of the other six start one of the
                six encounter chapters (ascending player number, skipping N).
  start         jumps to 'base' and starts the show clock (see --end-after)
  narrator      toggles the narrator overlay
  overlay1/2    toggle overlay 1 / overlay 2

A role can hold several physical tags (spares): any of them does the same
thing. Empty strings in the catalogue are spare slots not programmed yet.

Usage:
    python3 generate_player_configs.py            # testing: person tags work at any time
    python3 generate_player_configs.py --lock     # show mode: person tags only accepted in 'base'
    python3 generate_player_configs.py --end-after 12   # timed ending after 12 min (default 10, 0 = never)
    python3 generate_player_configs.py --duck 0.3       # effects: general audio drops to 30% while one plays
                                                       # (default 0.4, 1 = no ducking)
    -> writes output/player1/config.json ... output/player7/config.json
"""

import argparse
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
CATALOGUE_FILE = os.path.join(HERE, "tag_catalogue.json")

MAX_UIDS_PER_ROLE = 8                      # firmware limit: MAX_TAG_UIDS in Configloader.h
UID_RE = re.compile(r"^(?:[0-9A-F]{8}|[0-9A-F]{14})$")   # 4- or 7-byte NFC UIDs
PLANNED = {"player": 3, "start": 3, "narrator": 3, "overlay1": 6, "overlay2": 6}

# RAM FX slots for the three feedback sounds (files on the SD card:
# fx_connect.wav, fx_denied.wav, fx_ready.wav -> slots 4, 5, 6)
SOUNDS = {"connect": 4, "denied": 5, "ready": 6}

# Fixed order the 6 "other players" are mapped onto for any given player,
# ascending by player number, skipping self. Purely a default — edit
# ENCOUNTER_ORDER or hand-edit the generated JSON if a specific pairing
# should mean something different.
ENCOUNTER_ORDER = [
    "enc_play",
    "enc_suspense",
    "enc_loneliness",
    "enc_nurture",
    "enc_pressure",
    "enc_shift",
]

OUTPUT_DIR = "output"

# ── Shared chapter timeline — identical on every player's SD card ──
CHAPTERS = [
    {
        "id": "start_arrival", "label": "Start — Arrival",
        "start_ms": 0, "loop_end_ms": 45000, "loop": True,
        "led_background": {"animation": "level", "color": "0x223344",
                            "intensity": 0.5, "speed": 0.3},
        "led_enter": {"animation": "flash", "color": "0xFFFFFF",
                      "intensity": 0.6, "speed": 0.6, "duration_ms": 600},
    },
    {
        "id": "base", "label": "Base World",
        "start_ms": 45000, "loop_end_ms": 90000, "loop": True,
        "led_background": {"animation": "level", "color": "0x0044AA",
                            "intensity": 0.6, "speed": 0.4},
        "led_enter": {"animation": "pulse_ring", "color": "0x0044AA",
                      "intensity": 0.8, "speed": 0.5, "duration_ms": 1000},
    },
    {
        "id": "recharge", "label": "Recharge — Self",
        "start_ms": 90000, "loop_end_ms": 135000, "loop": False,
        "returns_to": "base",
        "led_background": {"animation": "level", "color": "0x88FF44",
                            "intensity": 0.6, "speed": 0.35},
        "led_enter": {"animation": "pulse_ring", "color": "0x88FF44",
                      "intensity": 0.85, "speed": 0.5, "duration_ms": 1000},
    },
    {
        "id": "enc_play", "label": "Encounter — Play",
        "start_ms": 135000, "loop_end_ms": 180000, "loop": False,
        "returns_to": "base",
        "led_background": {"animation": "level", "color": "0xFF6600",
                            "intensity": 0.7, "speed": 0.55},
        "led_enter": {"animation": "flash", "color": "0xFF6600",
                      "intensity": 0.9, "speed": 0.8, "duration_ms": 800},
    },
    {
        "id": "enc_suspense", "label": "Encounter — Suspense",
        "start_ms": 180000, "loop_end_ms": 225000, "loop": False,
        "returns_to": "base",
        "led_background": {"animation": "level", "color": "0x550088",
                            "intensity": 0.6, "speed": 0.3},
        "led_enter": {"animation": "flash", "color": "0x8800CC",
                      "intensity": 0.9, "speed": 0.7, "duration_ms": 800},
    },
    {
        "id": "enc_loneliness", "label": "Encounter — Loneliness",
        "start_ms": 225000, "loop_end_ms": 270000, "loop": False,
        "returns_to": "base",
        "led_background": {"animation": "level", "color": "0x224466",
                            "intensity": 0.45, "speed": 0.25},
        "led_enter": {"animation": "flash", "color": "0x336699",
                      "intensity": 0.7, "speed": 0.6, "duration_ms": 800},
    },
    {
        "id": "enc_nurture", "label": "Encounter — Nurture",
        "start_ms": 270000, "loop_end_ms": 315000, "loop": False,
        "returns_to": "base",
        "led_background": {"animation": "level", "color": "0xFFAA88",
                            "intensity": 0.6, "speed": 0.3},
        "led_enter": {"animation": "pulse_ring", "color": "0xFFAA88",
                      "intensity": 0.85, "speed": 0.5, "duration_ms": 1000},
    },
    {
        "id": "enc_pressure", "label": "Encounter — Pressure",
        "start_ms": 315000, "loop_end_ms": 360000, "loop": False,
        "returns_to": "base",
        "led_background": {"animation": "level", "color": "0xFF0033",
                            "intensity": 0.8, "speed": 0.7},
        "led_enter": {"animation": "flash", "color": "0xFF0033",
                      "intensity": 1.0, "speed": 0.9, "duration_ms": 800},
    },
    {
        "id": "enc_shift", "label": "Encounter — Shift",
        "start_ms": 360000, "loop_end_ms": 405000, "loop": False,
        "returns_to": "base",
        "led_background": {"animation": "level", "color": "0x00CCCC",
                            "intensity": 0.6, "speed": 0.45},
        "led_enter": {"animation": "comet", "color": "0x00CCCC",
                      "intensity": 0.85, "speed": 0.7, "duration_ms": 1000},
    },
    {
        "id": "end_reflection", "label": "End — Reflection",
        "start_ms": 405000, "loop_end_ms": 450000, "loop": False,
        "returns_to": "",   # empty = true end, experience goes idle
        "led_background": {"animation": "level", "color": "0xFFFFFF",
                            "intensity": 0.4, "speed": 0.2},
        "led_enter": {"animation": "flash", "color": "0xFFFFFF",
                      "intensity": 0.8, "speed": 0.5, "duration_ms": 1500},
    },
]


def build_chapter(ch):
    return {
        "id": ch["id"],
        "label": ch["label"],
        "start_ms": ch["start_ms"],
        "loop_end_ms": ch["loop_end_ms"],
        "on_enter_fx": ch.get("on_enter_fx", -1),
        "loop": ch["loop"],
        "returns_to": ch.get("returns_to", ""),
        "overlay_mode": "fresh",
        "overlays": {"ov1": False, "ov2": False, "narr": False},
        "led_background": ch["led_background"],
        "led_enter": ch["led_enter"],
    }


def load_catalogue(path):
    """Returns (catalogue, slots, errors). catalogue maps role -> list of UIDs;
    slots maps role -> number of slots in the file (programmed or not)."""
    with open(path) as f:
        raw = json.load(f)

    errors, seen = [], {}
    cat, slots = {"players": {}}, {}

    def clean(role, items):
        if not isinstance(items, list):
            errors.append(f"{role}: must be a list of UID strings")
            return [], 0
        out = []
        for item in items:
            uid = str(item).strip().upper()
            if not uid:
                continue                                  # empty slot, not programmed yet
            if not UID_RE.match(uid):
                errors.append(f"{role}: '{item}' is not a 4- or 7-byte hex UID")
                continue
            if uid in seen:
                errors.append(f"{role}: {uid} is already used for {seen[uid]}")
                continue
            seen[uid] = role
            out.append(uid)
        if len(out) > MAX_UIDS_PER_ROLE:
            errors.append(f"{role}: {len(out)} tags, but the firmware takes at most {MAX_UIDS_PER_ROLE}")
        return out, len(items)

    players = raw.get("players", {})
    for n in range(1, 8):
        cat["players"][n], slots[f"player {n}"] = clean(f"player {n}", players.get(str(n), []))
    for key in ("start", "narrator", "overlay1", "overlay2"):
        cat[key], slots[key] = clean(key, raw.get(key, []))

    known = {"players", "start", "narrator", "overlay1", "overlay2"}
    for key in raw:
        if not key.startswith("_") and key not in known:
            errors.append(f"unknown key '{key}' in the catalogue (typo?)")
    return cat, slots, errors


def print_summary(cat, slots):
    print("tag catalogue:")
    rows = [(f"player {n}", cat["players"][n], PLANNED["player"]) for n in range(1, 8)]
    rows += [(k, cat[k], PLANNED[k]) for k in ("start", "narrator", "overlay1", "overlay2")]
    for role, uids, planned in rows:
        have = len(uids)
        total = max(slots[role], planned)
        flag = "" if have >= planned else f"   <- {planned - have} more to program"
        print(f"  {role:<9} {have}/{total}{flag}")


def build_tags(cat, player_index, warn):
    """player_index is 0-based; returns the tag list for that player's card."""
    me = player_index + 1
    tags = []

    def add(uids, label, ttype, cooldown_ms, actions):
        if not uids:
            warn(f"'{label}' has no tags in the catalogue yet - left out")
            return
        tags.append({"label": label, "type": ttype, "cooldown_ms": cooldown_ms,
                     "uids": list(uids), "actions": actions})

    add(cat["start"],    "Start tag (entrance)", "start",    2000,
        [{"do": "chapter", "target": "base"}])
    add(cat["narrator"], "Narrator toggle",      "location", 1000,
        [{"do": "overlay", "target": "narr", "value": "toggle"}])
    add(cat["overlay1"], "Overlay 1 toggle",     "location", 1000,
        [{"do": "overlay", "target": "ov1", "value": "toggle"}])
    add(cat["overlay2"], "Overlay 2 toggle",     "location", 1000,
        [{"do": "overlay", "target": "ov2", "value": "toggle"}])

    add(cat["players"][me], f"Self (player {me}) - recharge", "person", 4000,
        [{"do": "chapter", "target": "recharge"}])
    others = [n for n in range(1, 8) if n != me]
    for n, enc_id in zip(others, ENCOUNTER_ORDER):
        add(cat["players"][n], f"Meet player {n} -> {enc_id}", "person", 4000,
            [{"do": "chapter", "target": enc_id}])
    return tags


def build_config(cat, player_index, lock, end_after, duck, warn):
    return {
        "device_id": f"PLAYER_{player_index + 1}",
        "start_chapter": "start_arrival",
        "led_brightness": 120,
        "idle": {
            "led": {"animation": "breathe", "color": "0x000066",
                    "intensity": 0.35, "speed": 0.2}
        },
        # person tags only accepted while 'base' plays (False = testing, switch any time)
        "encounter_lock": lock,
        "base_chapter": "base",
        "sounds": dict(SOUNDS),
        # timed ending: this many minutes after the start tag the show wraps up -
        # the chapter that is playing finishes, then end_chapter plays, then
        # sound and LEDs go off. 0 = never.
        "end_after_min": end_after,
        # ducking: while an effect plays, the general audio is lowered to this
        # fraction (0.4 = -8 dB). 1.0 = off.
        "duck_level": duck,
        "end_chapter": "end_reflection",
        "chapters": [build_chapter(c) for c in CHAPTERS],
        "tags": build_tags(cat, player_index, warn),
    }


def main():
    ap = argparse.ArgumentParser(description="Generate the 7 per-player config.json files.")
    ap.add_argument("--lock", action="store_true",
                    help="show mode: person tags are only accepted while 'base' is playing")
    ap.add_argument("--end-after", type=float, default=10.0, metavar="MINUTES",
                    help="minutes after the start tag until the ending begins (default 10, 0 = never)")
    ap.add_argument("--duck", type=float, default=0.4, metavar="LEVEL",
                    help="general audio level while an effect plays, 0-1 (default 0.4, 1 = no ducking)")
    ap.add_argument("--catalogue", default=CATALOGUE_FILE, help="path to tag_catalogue.json")
    ap.add_argument("--out", default=OUTPUT_DIR, help="output folder")
    args = ap.parse_args()

    if not 0.0 <= args.duck <= 1.0:
        sys.exit("--duck must be between 0 and 1")
    if args.end_after < 0:
        sys.exit("--end-after must be 0 or more")
    if len(ENCOUNTER_ORDER) != 6:
        sys.exit("ENCOUNTER_ORDER must have exactly 6 entries")

    cat, slots, errors = load_catalogue(args.catalogue)
    if errors:
        print("catalogue problems - nothing written:")
        for e in errors:
            print("  ERROR:", e)
        sys.exit(1)
    print_summary(cat, slots)

    warnings = []
    def warn(msg):
        if msg not in warnings:
            warnings.append(msg)

    print(f"\ntimed ending: {'after %g min' % args.end_after if args.end_after else 'never'} (end chapter 'end_reflection')")
    print(f"effect ducking: {'general audio drops to %g while an effect plays' % args.duck if args.duck < 1 else 'off'}")
    print(f"encounter lock: {'ON (show mode)' if args.lock else 'off (testing: person tags work at any time)'}")
    for i in range(7):
        cfg = build_config(cat, i, args.lock, args.end_after, args.duck, warn)
        out_dir = os.path.join(args.out, f"player{i+1}")
        os.makedirs(out_dir, exist_ok=True)
        out_path = os.path.join(out_dir, "config.json")
        with open(out_path, "w") as f:
            json.dump(cfg, f, indent=2)
        n_uids = sum(len(t["uids"]) for t in cfg["tags"])
        print(f"wrote {out_path}  ({len(cfg['tags'])} tags, {n_uids} uids)")

    for w in warnings:
        print("WARNING:", w)


if __name__ == "__main__":
    main()