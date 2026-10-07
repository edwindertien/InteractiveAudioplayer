#!/usr/bin/env python3
"""
make_docs.py — regenerates every figure in docs/ (SVG + PNG).

    python3 tools/make_docs.py              # all figures
    python3 tools/make_docs.py audio_graph  # just the ones whose name matches

Needs:  pip install cairosvg pillow
The experience map is produced by experience/Visualise.py from an example
card built with configs/generate_player_configs.py, so it always reflects the
current config schema and chapter timeline.

Figures live in docs/ next to the PCB placeholders; README.md and Context.md
embed the PNGs and link to the SVGs (vector, zoomable).
"""

import importlib.util
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DOCS = ROOT / "docs"

try:
    import cairosvg
    from PIL import ImageFont
except ImportError:
    sys.exit("pip install cairosvg pillow")

# ─────────────────────────────────────────────────────────────
#  Tiny SVG toolkit
# ─────────────────────────────────────────────────────────────

FONT = "DejaVu Sans, Verdana, Arial, sans-serif"
MONO = "DejaVu Sans Mono, Menlo, Consolas, monospace"
_FONTS = {}

def _font(size, bold=False, mono=False):
    key = (round(size * 4), bold, mono)
    if key not in _FONTS:
        name = {(False, False): "DejaVuSans.ttf", (True, False): "DejaVuSans-Bold.ttf",
                (False, True): "DejaVuSansMono.ttf", (True, True): "DejaVuSansMono-Bold.ttf"}[(bold, mono)]
        try:
            _FONTS[key] = ImageFont.truetype(f"/usr/share/fonts/truetype/dejavu/{name}", size)
        except OSError:                                    # no DejaVu: rough estimate
            _FONTS[key] = None
    return _FONTS[key]

def tw(text, size=12, bold=False, mono=False):
    """Rendered width of a text line in px (DejaVu metrics — wider than Arial, so safe)."""
    f = _font(size, bold, mono)
    return f.getlength(text) if f else len(text) * size * (0.62 if mono else 0.58)

def esc(t):
    return str(t).replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")

# palette: fill, stroke, text accent
C = {
    "teensy": ("#dbeafe", "#2563eb", "#1e3a8a"),
    "body":   ("#dcfce7", "#16a34a", "#14532d"),
    "hand":   ("#ffedd5", "#ea580c", "#7c2d12"),
    "tag":    ("#f3e8ff", "#9333ea", "#581c87"),
    "file":   ("#fef9c3", "#ca8a04", "#713f12"),
    "audio":  ("#ccfbf1", "#0d9488", "#134e4a"),
    "tool":   ("#e0e7ff", "#4f46e5", "#312e81"),
    "gray":   ("#f3f4f6", "#6b7280", "#1f2937"),
    "red":    ("#fee2e2", "#dc2626", "#7f1d1d"),
    "white":  ("#ffffff", "#9ca3af", "#1f2937"),
}
INK, MUTED, LINE = "#1f2937", "#6b7280", "#9ca3af"


class Svg:
    def __init__(self, name, w, h, title, subtitle=None):
        self.name, self.w, self.h = name, w, h
        self.parts = []
        self.markers = {}
        self.add(f'<rect width="{w}" height="{h}" fill="#ffffff"/>')
        self.text(24, 34, title, 20, INK, weight="bold")
        if subtitle:
            self.text(24, 56, subtitle, 12, MUTED)

    def add(self, s):
        self.parts.append(s)

    def fit(self, h):
        """Shrink/grow the canvas to h px (the white background is the first part)."""
        self.h = int(h)
        self.parts[0] = f'<rect width="{self.w}" height="{self.h}" fill="#ffffff"/>'

    # ── primitives ───────────────────────────────────────────
    def text(self, x, y, t, size=12, fill=INK, anchor="start", weight="normal", mono=False, italic=False, opacity=1):
        fam = MONO if mono else FONT
        st = ' font-style="italic"' if italic else ""
        op = f' opacity="{opacity}"' if opacity != 1 else ""
        self.add(f'<text x="{x:.1f}" y="{y:.1f}" font-family="{fam}" font-size="{size}" fill="{fill}" '
                 f'text-anchor="{anchor}" font-weight="{weight}"{st}{op}>{esc(t)}</text>')

    def lines(self, x, y, lines, size=11, fill=INK, anchor="start", lh=None, mono=False, weight="normal"):
        lh = lh or size * 1.4
        for i, ln in enumerate(lines):
            self.text(x, y + i * lh, ln, size, fill, anchor, weight, mono)

    def rect(self, x, y, w, h, fill="#fff", stroke=LINE, rx=8, sw=1.4, dash=None, opacity=1):
        d = f' stroke-dasharray="{dash}"' if dash else ""
        op = f' opacity="{opacity}"' if opacity != 1 else ""
        self.add(f'<rect x="{x:.1f}" y="{y:.1f}" width="{w:.1f}" height="{h:.1f}" rx="{rx}" '
                 f'fill="{fill}" stroke="{stroke}" stroke-width="{sw}"{d}{op}/>')

    def line(self, x1, y1, x2, y2, stroke=LINE, sw=1.4, dash=None):
        d = f' stroke-dasharray="{dash}"' if dash else ""
        self.add(f'<line x1="{x1:.1f}" y1="{y1:.1f}" x2="{x2:.1f}" y2="{y2:.1f}" stroke="{stroke}" stroke-width="{sw}"{d}/>')

    def _marker(self, color):
        if color not in self.markers:
            mid = f"m{len(self.markers)}"
            self.markers[color] = mid
        return self.markers[color]

    def arrow(self, pts, color="#4b5563", sw=1.8, dash=None, both=False):
        """Polyline with an arrowhead at the end (and the start if both=True)."""
        mid = self._marker(color)
        d = f' stroke-dasharray="{dash}"' if dash else ""
        p = " ".join(f"{x:.1f},{y:.1f}" for x, y in pts)
        ms = f' marker-start="url(#{mid}s)"' if both else ""
        self.add(f'<polyline points="{p}" fill="none" stroke="{color}" stroke-width="{sw}"{d} '
                 f'marker-end="url(#{mid})"{ms}/>')

    def circle(self, cx, cy, r, fill, stroke="none", sw=1):
        self.add(f'<circle cx="{cx:.1f}" cy="{cy:.1f}" r="{r}" fill="{fill}" stroke="{stroke}" stroke-width="{sw}"/>')

    def badge(self, cx, cy, n, fill="#374151"):
        self.circle(cx, cy, 11, fill)
        self.text(cx, cy + 4, str(n), 11, "#fff", "middle", "bold")

    def label(self, x, y, t, size=11, fill=MUTED, anchor="middle", bg=True, bold=False):
        """Text with a white pill behind it — for labels that sit on top of arrows."""
        w = tw(t, size, bold) + 10
        if bg:
            ax = x - w / 2 if anchor == "middle" else (x - 5 if anchor == "start" else x - w + 5)
            self.rect(ax, y - size - 1, w, size + 7, "#ffffff", "none", 4, 0, opacity=0.92)
        self.text(x, y, t, size, fill, anchor, "bold" if bold else "normal")

    # ── composite: a titled box ──────────────────────────────
    def box(self, x, y, w, title, body=(), kind="white", h=None, tsize=13, bsize=11, mono_body=False,
            sub=None, dash=None, align="left"):
        fill, stroke, accent = C[kind]
        pad = 10
        th = tsize + 12
        bh = len(body) * bsize * 1.45
        sh = (bsize + 5) if sub else 0
        h = h or (th + sh + bh + (8 if body else 2))
        self.rect(x, y, w, h, fill, stroke, 9, 1.6, dash)
        cx = x + w / 2 if align == "center" else x + pad
        anc = "middle" if align == "center" else "start"
        self.text(cx, y + tsize + 5, title, tsize, accent, anc, "bold")
        yy = y + th + 2
        if sub:
            self.text(cx, yy + bsize - 1, sub, bsize - 1, MUTED, anc, italic=True)
            yy += sh
        self.lines(cx, yy + bsize, body, bsize, INK, anc, bsize * 1.45, mono_body)
        return h

    def group(self, x, y, w, h, title, kind="gray"):
        fill, stroke, accent = C[kind]
        self.rect(x, y, w, h, "none", stroke, 14, 1.4, "7,5")
        self.text(x + 14, y + 20, title, 12, accent, weight="bold")

    # ── output ───────────────────────────────────────────────
    def render(self):
        defs = ["<defs>"]
        for color, mid in self.markers.items():
            defs.append(f'<marker id="{mid}" markerWidth="10" markerHeight="10" refX="8.5" refY="5" orient="auto" '
                        f'markerUnits="userSpaceOnUse"><path d="M1,1 L9,5 L1,9 Z" fill="{color}"/></marker>')
            defs.append(f'<marker id="{mid}s" markerWidth="10" markerHeight="10" refX="1.5" refY="5" orient="auto-start-reverse" '
                        f'markerUnits="userSpaceOnUse"><path d="M1,1 L9,5 L1,9 Z" fill="{color}"/></marker>')
        defs.append("</defs>")
        head = (f'<svg xmlns="http://www.w3.org/2000/svg" width="{self.w}" height="{self.h}" '
                f'viewBox="0 0 {self.w} {self.h}">')
        return head + "\n".join(defs) + "\n" + "\n".join(self.parts) + "\n</svg>\n"

    def save(self):
        DOCS.mkdir(exist_ok=True)
        svg = self.render()
        (DOCS / f"{self.name}.svg").write_text(svg)
        cairosvg.svg2png(bytestring=svg.encode("utf-8"), write_to=str(DOCS / f"{self.name}.png"), scale=2)
        print(f"  docs/{self.name}.svg + .png  ({self.w}x{self.h})")


FIGURES = {}

def figure(fn):
    FIGURES[fn.__name__] = fn
    return fn


# ─────────────────────────────────────────────────────────────
#  1. System overview
# ─────────────────────────────────────────────────────────────

@figure
def system_overview():
    s = Svg("system_overview", 1500, 860, "System overview — one player",
            "Seven of these run side by side. Each wearer carries a hand unit; each player has its own body bridge, Teensy and SD card.")

    # tags (left)
    s.group(20, 80, 230, 300, "NFC tags", "tag")
    s.box(36, 112, 198, "start", ["begins the show"], "tag", tsize=12, bsize=10.5)
    s.box(36, 172, 198, "narrator / overlay 1 / 2", ["toggle that layer"], "tag", tsize=12, bsize=10.5)
    s.box(36, 232, 198, "person 1 … 7", ["start recharge or an", "encounter chapter"], "tag", tsize=12, bsize=10.5)
    s.text(36, 345, "spare tags per role:", 10.5, MUTED)
    s.text(36, 360, "3 / 3 / 6+6 / 3 per person", 10.5, MUTED)

    # hand unit
    s.group(290, 80, 350, 460, "Wearer — hand unit  (ESP32-S3 SuperMini)", "hand")
    parts = [("PN532 NFC reader", "UART 115200 · GPIO 5/6 · RST 10"),
             ("VL53L0X distance sensor", "I²C · GPIO 7/8 · XSHUT 1"),
             ("Vibration motor", "PWM 20 kHz · GPIO 2"),
             ("16-LED ring (WS2812B)", "GPIO 4 — tap and proximity feedback"),
             ("Status LED", "GPIO 48 — link / pairing / battery"),
             ("Button", "GPIO 9 — hold 2 s: sleep · tap: pairing"),
             ("LiPo battery", "ADC on GPIO 3 (divider)")]
    y = 112
    for t, d in parts:
        h = s.box(306, y, 318, t, [d], "hand", tsize=12, bsize=10.5)
        y += h + 8

    # body bridge
    s.group(750, 80, 260, 300, "Body bridge  (ESP32-S3 SuperMini)", "body")
    s.box(766, 112, 228, "ESP-NOW ⇄ UART", ["forwards hand-unit data", "to the Teensy, sends a", "heartbeat back every 1 s"], "body", tsize=12, bsize=10.5)
    s.box(766, 206, 228, "Status LED  (GPIO 48)", ["red / yellow / green link", "blue = tag or pairing"], "body", tsize=12, bsize=10.5)
    s.box(766, 280, 228, "Pair button  (GPIO 9)", ["hold 2 s = pairing mode"], "body", tsize=12, bsize=10.5)

    # prepared-on-a-pc note
    s.box(750, 420, 260, "Prepared on a Mac / PC", ["stems → assemble_71.sh", "tags → generate_player_configs.py", "cards via card reader, or the", "Teensy's USB maintenance mode"], "tool", tsize=12, bsize=10.5)

    # teensy
    s.group(1080, 80, 400, 760, "Player — Teensy 4.1", "teensy")
    s.box(1096, 112, 368, "SD card  (built-in SDIO slot)", ["experience.wav   8-ch 7.1, 16-bit 44.1 kHz", "config.json  (or  playerN_config.json)",
                                                            "fx_connect / fx_denied / fx_ready .wav", "led_animations.json   (optional)"], "file", tsize=12, bsize=10.5, mono_body=True)
    s.box(1096, 238, 368, "Show logic", ["chapters · tags · overlays · seek fades", "person-tag lock · timed ending · ducking", "USB: serial CLI + MTP maintenance mode"], "teensy", tsize=12, bsize=10.5)
    s.box(1096, 338, 368, "Audio engine  (Teensy Audio Library)", ["8-channel player → mixers → two I²S outputs", "RAM effects, haptic soft limiter"], "teensy", tsize=12, bsize=10.5)
    s.box(1096, 450, 176, "I²S 1  →  PCM5102A", ["MCLK 23 · BCLK 21", "LRCLK 20 · DIN 7"], "audio", tsize=12, bsize=10.5)
    s.box(1288, 450, 176, "I²S 2  →  MAX98357A", ["LRC 3 · BCLK 4", "DIN 2"], "audio", tsize=12, bsize=10.5)
    s.box(1096, 580, 176, "Headphones", ["(built-in amplifier)", "optional level pot"], "audio", tsize=12, bsize=10.5)
    s.box(1288, 580, 176, "Haptic transducer", ["vibration through", "the body"], "audio", tsize=12, bsize=10.5)
    s.box(1096, 710, 176, "LED ring (player)", ["pin 14 · 16 × SK6812"], "audio", tsize=12, bsize=10.5)
    s.box(1288, 710, 176, "LED animator", ["ring level follows", "the haptic signal"], "teensy", tsize=12, bsize=10.5)

    # flows
    s.arrow([(250, 190), (290, 190)], "#9333ea")
    s.label(270, 176, "tap", 10.5)
    s.arrow([(640, 160), (750, 160)], "#4b5563", both=True)
    s.label(695, 146, "ESP-NOW", 10.5, bold=True)
    s.label(695, 178, "2.4 GHz", 10)
    s.text(695, 216, "proximity 10 Hz", 9.5, MUTED, "middle")
    s.text(695, 230, "NFC, touch →", 9.5, MUTED, "middle")
    s.text(695, 244, "← heartbeat 1 s", 9.5, MUTED, "middle")
    s.arrow([(1010, 180), (1080, 180)], "#4b5563")
    s.label(1041, 166, "UART", 10.5, bold=True)
    s.label(1041, 198, "115200", 10)
    s.text(1041, 222, "JSON lines", 9.5, MUTED, "middle")
    s.text(1041, 236, "p · n · c", 9.5, MUTED, "middle")
    s.text(1041, 250, "pins 0/1", 9.5, MUTED, "middle")
    # SD card content comes from the PC
    s.arrow([(1010, 470), (1072, 470), (1072, 150), (1096, 150)], "#4f46e5", dash="5,4")
    s.label(1072, 330, "SD card", 10, "#4f46e5")
    # audio engine -> the two I2S outputs -> transducers
    s.arrow([(1280, 402), (1280, 426), (1184, 426), (1184, 450)], "#4b5563")
    s.arrow([(1280, 426), (1376, 426), (1376, 450)], "#4b5563")
    s.arrow([(1184, 520), (1184, 580)], "#0d9488")
    s.arrow([(1376, 520), (1376, 580)], "#0d9488")
    # haptic level -> LED animator -> ring
    s.arrow([(1376, 650), (1376, 710)], "#0d9488", dash="4,3")
    s.label(1376, 684, "level", 10)
    s.arrow([(1288, 745), (1272, 745)], "#4b5563")
    s.save()


# ─────────────────────────────────────────────────────────────
#  2. Content workflow
# ─────────────────────────────────────────────────────────────

@figure
def content_workflow():
    s = Svg("content_workflow", 1500, 700, "Content workflow — from stems and tags to a ready SD card",
            "Everything the experience designer edits lives in files; no firmware change is needed.")

    # 1 · audio
    s.group(20, 80, 1100, 270, "1 · Audio", "file")
    s.box(40, 112, 170, "Ableton", ["export 5 stems, same", "length, 44.1 kHz 16-bit"], "tool", tsize=12, bsize=10.5)
    stems = [("base.wav", "stereo"), ("ov1.wav", "stereo"), ("ov2.wav", "stereo"), ("narr.wav", "mono"), ("haptic.wav", "mono")]
    y = 108
    for n, k in stems:
        s.rect(250, y, 190, 28, "#fff", "#ca8a04", 6)
        s.text(262, y + 19, n, 11.5, INK, mono=True)
        s.text(428, y + 19, k, 10.5, MUTED, "end")
        y += 31
    s.box(490, 126, 250, "assemble_71.sh", ["checks every stem's channel count", "mono ↔ stereo is fixed for you", "refuses to leave a wrong file"], "tool", tsize=12, bsize=10.5)
    s.box(790, 126, 210, "experience.wav", ["8 channels: FL FR FC LFE", "BL BR SL SR (see audio graph)"], "file", tsize=12, bsize=10.5)
    s.box(790, 238, 300, "short effects  (mono, 16-bit, 44.1 kHz)", ["fx_connect.wav  fx_denied.wav  fx_ready.wav", "each up to ~2 s"], "file", tsize=11.5, bsize=10.5, mono_body=True)
    s.arrow([(210, 146), (250, 146)])
    s.arrow([(440, 160), (490, 160)])
    s.arrow([(740, 160), (790, 160)])

    # 2 · tags and configuration
    s.group(20, 370, 1100, 230, "2 · Tags and configuration", "tag")
    s.box(40, 402, 190, "Read the UIDs", ["tap each tag on a hand unit,", "copy  [Bridge] NFC: <uid>", "from the Teensy serial", "monitor"], "tag", tsize=12, bsize=10.5)
    s.box(260, 402, 220, "tag_catalogue.json", ["players 1–7 · start · narrator", "overlay1 · overlay2", "several UIDs per role (spares)"], "file", tsize=12, bsize=10.5)
    s.box(510, 402, 290, "generate_player_configs.py", ["--lock       person tags only in base", "--end-after  minutes until the end", "--duck       effect ducking level"], "tool", tsize=12, bsize=10.5, mono_body=True)
    s.box(830, 402, 260, "output/playerN/config.json", ["7 cards: shared chapters,", "own tag → recharge,", "show, sound and duck settings"], "file", tsize=12, bsize=10.5)
    s.arrow([(230, 440), (260, 440)]); s.arrow([(480, 440), (510, 440)]); s.arrow([(800, 440), (830, 440)])
    s.box(830, 505, 260, "Check it", ["Visualise.py → experience map", "led_editor.py · editor.py"], "tool", tsize=12, bsize=10.5)
    s.arrow([(960, 482), (960, 505)], dash="4,3")
    s.text(40, 580, "led_animations.json (custom ring animations, optional) is edited with tools/led_editor.py.", 10.5, MUTED)

    # 3 · SD card -> player (right column)
    s.group(1160, 80, 320, 520, "3 · SD card → player", "teensy")
    s.box(1176, 112, 288, "SD card, one per player", ["experience.wav", "config.json   (or playerN_config.json)", "fx_connect.wav  fx_denied.wav", "fx_ready.wav", "led_animations.json  (optional)"], "file", tsize=12, bsize=10.5, mono_body=True)
    s.box(1176, 270, 288, "macOS: dot_clean", ["removes the hidden ._ and .DS_Store", "files Finder adds to FAT cards"], "tool", tsize=12, bsize=10.5)
    s.box(1176, 365, 288, "Into the Teensy", ["card reader → slot, or leave the card", "in and use  usb  (MTP) over USB"], "teensy", tsize=12, bsize=10.5)
    s.box(1176, 460, 288, "Boot", ["config loads, the show starts in the", "start chapter (see startup figure)"], "teensy", tsize=12, bsize=10.5)
    s.arrow([(1320, 222), (1320, 270)]); s.arrow([(1320, 335), (1320, 365)]); s.arrow([(1320, 430), (1320, 460)])

    # bus: audio files and config -> SD card
    bus = "#ca8a04"
    s.line(1000, 160, 1138, 160, bus, 1.8)
    s.line(1090, 275, 1138, 275, bus, 1.8)
    s.line(1090, 440, 1138, 440, bus, 1.8)
    s.line(1138, 440, 1138, 150, bus, 1.8)
    s.arrow([(1138, 150), (1176, 150)], bus, 1.8)
    s.save()


# ─────────────────────────────────────────────────────────────
#  3. Audio graph
# ─────────────────────────────────────────────────────────────

@figure
def audio_graph():
    s = Svg("audio_graph", 1500, 860, "Audio graph — what happens to the 8 channels of experience.wav",
            "Teensy Audio Library objects as wired in main.cpp. Gains in italics are changed while the show runs.")

    # effects (top left)
    s.box(40, 90, 250, "RamPlayer  (effects)", ["7 slots loaded from the SD card at boot", "played with AudioPlayMemory", "while one plays the stems are ducked"], "file", tsize=12, bsize=10.5)

    # player
    s.rect(40, 210, 250, 340, C["teensy"][0], C["teensy"][1], 9, 1.6)
    s.text(52, 232, "AudioPlaySdWavMulti", 13, C["teensy"][2], weight="bold")
    s.text(52, 249, "experience.wav · 8 channels · one SD read", 10, MUTED, italic=True)
    ports = [(270, "0  FL   base L"), (305, "1  FR   base R"), (340, "2  FC   narration"), (375, "4  BL   overlay 1 L"),
             (410, "5  BR   overlay 1 R"), (445, "6  SL   overlay 2 L"), (480, "7  SR   overlay 2 R"), (530, "3  LFE  haptic")]
    for y, t in ports:
        s.text(60, y + 4, t, 11, INK, mono=True)
        s.circle(290, y, 4, C["teensy"][1])
    s.text(52, 512, "(channel 3 is drawn last)", 9.5, MUTED, italic=True)

    # stem mixers
    for name, y0, ins in (("stemMixerL", 215, ("0  base L", "1  narration", "2  overlay 1 L", "3  overlay 2 L")),
                          ("stemMixerR", 390, ("0  base R", "1  narration", "2  overlay 1 R", "3  overlay 2 R"))):
        s.rect(420, y0, 170, 150, C["teensy"][0], C["teensy"][1], 9, 1.6)
        s.text(432, y0 + 20, name, 13, C["teensy"][2], weight="bold")
        for i, t in enumerate(ins):
            s.circle(420, y0 + 50 + i * 30, 3.5, C["teensy"][1])
            s.text(430, y0 + 54 + i * 30, t, 10.5, INK, mono=True)
        s.circle(590, y0 + 75, 3.5, C["teensy"][1])
    s.text(500, 380, "gains: 1 · narr · ov1 · ov2", 9.5, MUTED, "middle", italic=True)
    s.text(500, 555, "(ramped 0.02 per 20 ms)", 9.5, MUTED, "middle", italic=True)

    # port -> mixer arrows (lanes)
    teal = "#2563eb"
    L = {"in0": 265, "in1": 295, "in2": 325, "in3": 355}
    R = {"in0": 440, "in1": 470, "in2": 500, "in3": 530}
    s.arrow([(290, 270), (330, 270), (330, L["in0"]), (420, L["in0"])], teal)
    s.arrow([(290, 305), (340, 305), (340, R["in0"]), (420, R["in0"])], teal)
    s.arrow([(290, 340), (350, 340), (350, L["in1"]), (420, L["in1"])], teal)
    s.arrow([(350, 340), (350, R["in1"]), (420, R["in1"])], teal)
    s.arrow([(290, 375), (360, 375), (360, L["in2"]), (420, L["in2"])], teal)
    s.arrow([(290, 410), (370, 410), (370, R["in2"]), (420, R["in2"])], teal)
    s.arrow([(290, 445), (380, 445), (380, L["in3"]), (420, L["in3"])], teal)
    s.arrow([(290, 480), (390, 480), (390, R["in3"]), (420, R["in3"])], teal)

    # main mixers
    for name, y0 in (("mainMixerL", 215), ("mainMixerR", 390)):
        s.rect(700, y0, 200, 110, C["teensy"][0], C["teensy"][1], 9, 1.6)
        s.text(712, y0 + 20, name, 13, C["teensy"][2], weight="bold")
        s.circle(700, y0 + 47, 3.5, C["teensy"][1]); s.text(710, y0 + 51, "0  stems", 10.5, INK, mono=True)
        s.text(712, y0 + 64, "gain = seek fade × duck", 9.5, MUTED, italic=True)
        s.circle(700, y0 + 82, 3.5, C["teensy"][1]); s.text(710, y0 + 86, "1  effects", 10.5, INK, mono=True)
        s.circle(900, y0 + 55, 3.5, C["teensy"][1])
    s.arrow([(590, 290), (650, 290), (650, 262), (700, 262)])
    s.arrow([(590, 465), (650, 465), (650, 437), (700, 437)])
    s.arrow([(290, 135), (670, 135), (670, 297), (700, 297)], "#ca8a04")
    s.arrow([(670, 297), (670, 472), (700, 472)], "#ca8a04")

    # I2S 1 -> DAC -> headphones
    s.box(980, 290, 160, "AudioOutputI2S", ["I²S 1", "L from mainMixerL", "R from mainMixerR"], "audio", tsize=12, bsize=10.5)
    s.arrow([(900, 270), (940, 270), (940, 335), (980, 335)])
    s.arrow([(900, 445), (940, 445), (940, 365), (980, 365)])
    s.box(1200, 290, 140, "PCM5102A", ["DAC · H3L jumper", "bridged to HIGH"], "audio", tsize=12, bsize=10.5)
    s.box(1370, 290, 110, "Headphones", ["built-in amp", "analog pot"], "audio", tsize=12, bsize=10.5)
    s.arrow([(1140, 335), (1200, 335)], "#0d9488"); s.arrow([(1340, 335), (1370, 335)], "#0d9488")

    # haptic chain
    s.arrow([(290, 530), (305, 530), (305, 685), (420, 685)], "#dc2626")
    s.label(305, 600, "LFE", 10.5, "#dc2626")
    s.box(420, 640, 170, "hapticMixer", ["gain  (hgain)", "1.0 by default"], "teensy", tsize=12, bsize=10.5)
    s.box(640, 640, 200, "hapticShaper", ["AudioEffectWaveshaper", "tanh soft limiter (hdrive)"], "teensy", tsize=12, bsize=10.5)
    s.box(890, 640, 150, "AudioOutputI2S2", ["I²S 2", "mono → L + R"], "audio", tsize=12, bsize=10.5)
    s.box(1090, 640, 160, "MAX98357A", ["class-D amp", "SD → VIN"], "audio", tsize=12, bsize=10.5)
    s.box(1300, 640, 180, "Haptic transducer", ["vibration through the body"], "audio", tsize=12, bsize=10.5)
    for x1, x2 in ((590, 640), (840, 890), (1040, 1090), (1250, 1300)):
        s.arrow([(x1, 685), (x2, 685)], "#dc2626")
    s.box(420, 770, 250, "AudioAnalyzePeak → LED animator", ["tapped BEFORE the limiter;", "drives the ring's level pattern"], "teensy", tsize=11.5, bsize=10.5)
    s.arrow([(505, 730), (505, 770)], "#4b5563", dash="4,3")
    s.box(720, 782, 220, "LED ring  (pin 14)", ["16 × SK6812"], "audio", tsize=12, bsize=10.5)
    s.arrow([(670, 810), (720, 810)], "#4b5563")
    s.save()


# ─────────────────────────────────────────────────────────────
#  4. Show state machine
# ─────────────────────────────────────────────────────────────

@figure
def show_flow():
    s = Svg("show_flow", 1500, 640, "A show from start to finish — states and what moves between them",
            "Chapter names are those of the generated configs. Tags that toggle layers (narrator, overlays) work in every state.")

    # waiting room
    s.box(30, 150, 250, "WAITING ROOM", ["start_arrival  (loops)", "plays from power-on —", "no tag needed yet"], "gray", tsize=13, bsize=11, h=105)
    s.text(155, 125, "boot", 11, MUTED, "middle")
    s.arrow([(155, 128), (155, 150)], "#4b5563")

    # running group
    s.group(380, 100, 800, 290, "SHOW RUNNING — the clock started when the start tag was seen", "teensy")
    s.box(420, 150, 280, "BASE", ["base  (loops)", "the hub between encounters"], "teensy", tsize=13, bsize=11, h=105)
    s.box(850, 150, 300, "ENCOUNTER", ["recharge (own tag) or one of", "enc_play · enc_suspense · enc_loneliness", "enc_nurture · enc_pressure · enc_shift", "play once, ~45 s each"], "hand", tsize=13, bsize=11, h=105)
    s.arrow([(700, 180), (850, 180)], "#ea580c")
    s.label(775, 166, "person tag", 11, "#9a3412", bold=True)
    s.text(775, 198, "connect sound", 10, MUTED, "middle")
    s.arrow([(850, 232), (700, 232)], "#16a34a")
    s.label(775, 222, "chapter ends", 10.5, "#166534", bold=True)
    s.text(775, 250, "→ back to base · ready sound", 10, MUTED, "middle")
    s.arrow([(280, 190), (380, 190)], "#16a34a")
    s.label(330, 176, "start tag", 11, "#166534", bold=True)
    s.text(330, 210, "starts the clock", 10, MUTED, "middle")
    s.text(405, 335, "lock on: a person tag is accepted in BASE only — anywhere else it gets the 'denied' sound", 10.5, INK)
    s.text(405, 354, "lock off (testing): person tags switch chapter at any time", 10.5, MUTED)
    s.text(30, 290, "A start tag in any state restarts the show", 10.5, MUTED)
    s.text(30, 306, "from BASE with a fresh clock.", 10.5, MUTED)

    # ending chain
    s.box(420, 450, 280, "ENDING", ["time is up (end_after_min)", "the chapter that is playing finishes;", "a loop finishes its current pass", "new person tags are refused"], "red", tsize=13, bsize=11)
    s.box(780, 450, 250, "END CHAPTER", ["end_reflection", "plays once"], "teensy", tsize=13, bsize=11, h=100)
    s.box(1110, 450, 250, "SHOW OVER", ["sound and LEDs off", "only the start tag is accepted"], "gray", tsize=13, bsize=11, h=100)
    s.arrow([(560, 390), (560, 450)], "#dc2626")
    s.label(560, 424, "time is up", 11, "#991b1b", bold=True)
    s.arrow([(700, 500), (780, 500)], "#4b5563")
    s.label(740, 488, "done", 10.5)
    s.arrow([(1030, 500), (1110, 500)], "#4b5563")
    s.label(1070, 488, "ends", 10.5)
    s.arrow([(1150, 450), (1150, 390)], "#16a34a")
    s.label(1150, 428, "start tag", 11, "#166534", bold=True)
    s.text(1150, 444, "starts again", 10, MUTED, "middle")

    # side notes
    s.box(30, 450, 330, "At any time", ["narrator / overlay tags toggle that layer", "effects play from RAM; the general audio", "ducks while one plays (duck_level)", "serial:  show · show 0.5 · show end · lock · duck"], "tool", tsize=12, bsize=10.5)
    s.save()


# ─────────────────────────────────────────────────────────────
#  5. Startup sequences (flow helper)
# ─────────────────────────────────────────────────────────────

def boot_flow(name, title, subtitle, kind, steps, loop_title, loop_lines, width=1500):
    """steps: list of dicts {t, d:[lines], branch:(label, lines, kind) | None}"""
    LH = 11 * 1.45
    heights = [max(46, 30 + len(st["d"]) * LH + 6) for st in steps]
    rows = (len(loop_lines) + 1) // 2
    total = 84 + sum(heights) + 14 * len(steps) + 16 + (rows * LH + 46) + 30
    s = Svg(name, width, int(total), title, subtitle)
    x, w = 70, 640
    y = 84
    bx = x + w + 160
    prev_bottom = None
    for i, st in enumerate(steps):
        h = heights[i]
        if prev_bottom is not None:
            s.arrow([(x + w / 2, prev_bottom), (x + w / 2, y)], "#4b5563")
        fill, stroke, accent = C[st.get("kind", kind)]
        s.rect(x, y, w, h, fill, stroke, 9, 1.6)
        s.badge(x - 22, y + h / 2, i + 1)
        s.text(x + 14, y + 22, st["t"], 13, accent, weight="bold")
        s.lines(x + 14, y + 40, st["d"], 11, INK, lh=LH)
        if st.get("branch"):
            lab, lines, bk = st["branch"]
            bf, bs, ba = C[bk]
            bh = max(46, 30 + len(lines) * LH + 6)
            s.rect(bx, y + h / 2 - bh / 2, 470, bh, bf, bs, 9, 1.6)
            s.text(bx + 14, y + h / 2 - bh / 2 + 22, lines[0], 12, ba, weight="bold")
            s.lines(bx + 14, y + h / 2 - bh / 2 + 40, lines[1:], 11, INK, lh=LH)
            s.arrow([(x + w, y + h / 2), (bx, y + h / 2)], bs, dash="5,4")
            s.text(x + w + 8, y + h / 2 - 7, lab, 10.5, ba, weight="bold")
        prev_bottom = y + h
        y += h + 14
    # loop box
    s.arrow([(x + w / 2, prev_bottom), (x + w / 2, y + 6)], "#4b5563")
    y += 6
    lh = rows * LH + 46
    fill, stroke, accent = C["gray"]
    s.rect(x, y, width - 2 * x, lh, fill, stroke, 9, 1.6, dash="7,5")
    s.text(x + 14, y + 22, loop_title, 13, accent, weight="bold")
    half = (len(loop_lines) + 1) // 2
    s.lines(x + 14, y + 42, loop_lines[:half], 11, INK, lh=LH)
    s.lines(x + (width - 2 * x) / 2 + 10, y + 42, loop_lines[half:], 11, INK, lh=LH)
    return s


@figure
def boot_teensy():
    steps = [
        {"t": "Serial 115200 · banner · LED ring starts", "d": ["leds.begin(): pin 14, 16 × SK6812"]},
        {"t": "Audio engine", "d": ["AudioMemory(40); stem gain 1, overlays 0; mainMixer 1 / 1",
                                    "haptic soft-limiter table installed (an empty one would be silence)"]},
        {"t": "Mount the SD card (SDIO)", "d": ["built-in slot, FIFO_SDIO"],
         "branch": ("fails", ["Blink the onboard LED forever", "pin 13, 100 ms — nothing else runs"], "red")},
        {"t": "Load the config", "d": ["/config.json, else the one *_config.json on the card",
                                       "chapters, tags (several UIDs per role), sounds, ducking, timed ending",
                                       "warns about duplicate UIDs and a missing end chapter"],
         "branch": ("several *_config.json", ["Nothing is loaded", "keep one, or name it config.json"], "red")},
        {"t": "LED brightness + custom animations", "d": ["led_brightness from the config · /led_animations.json (optional)"]},
        {"t": "Load the RAM effects", "d": ["slots 0-6: fx_dev · fx_loc_a · fx_loc_b · fx_boot · fx_connect · fx_denied · fx_ready",
                                          "a missing file is logged and that slot stays silent; RAM use is printed"]},
        {"t": "Start the body-bridge link", "d": ["Serial1, 115200, pins 0 / 1 — newline-terminated JSON"]},
        {"t": "Auto-start into the start chapter", "d": ["open experience.wav, go to start_chapter (e.g. start_arrival)",
                                                         "the audio plays from power-on, no tag needed"]},
    ]
    loop = ["USB maintenance mode: service MTP while active",
            "haptic peak → LED ring level; leds.update() at 25 fps",
            "every 20 ms: overlay fades · seek fades · ducking → mainMixer",
            "show clock: has end_after_min passed since the start tag?",
            "chapter finished? → end chapter · returns_to (ready sound) · idle",
            "body bridge: a confirmed tag 'c' → look up UID → dispatch",
            "serial commands (go, show, lock, duck, usb, …)",
            "status line every 3 s: chapter · position · overlays · bridge · CPU · memory"]
    s = boot_flow("boot_teensy", "Teensy player — startup sequence, then the main loop",
                  "setup() in main.cpp, in the order it runs.", "teensy", steps, "loop() — repeated continuously", loop)
    s.save()


@figure
def boot_hand():
    steps = [
        {"t": "CPU 80 MHz · Serial 115200 · banner", "d": ["80 MHz is plenty and runs much cooler than 240 MHz"]},
        {"t": "Button and wake reason", "d": ["released the pin hold latched before sleep",
                                              "did the unit wake from a button press?"],
         "branch": ("no: power-up or reset", ["Go straight back to deep sleep", "press the button to start the unit"], "red")},
        {"t": "Wait for the button to be released", "d": ["a button wake is confirmed, then the real start-up continues"]},
        {"t": "Motor and distance sensor", "d": ["motor PWM · VL53L0X XSHUT toggle + init",
                                                  "this unit's stored tof settings (if any) are applied"]},
        {"t": "LED ring, status LED, battery", "d": ["16-LED ring at brightness 40 · status LED (GPIO 48)",
                                                      "battery offset loaded; USB or battery detected"]},
        {"t": "NFC reader", "d": ["PN532 reset on GPIO 10, then UART init; UIDs go to a queue"]},
        {"t": "ESP-NOW", "d": ["Wi-Fi station mode, ESP-NOW init, stored pairing loaded from flash",
                               "no pairing → status LED red until you pair (see pairing figure)"]},
        {"t": "Ready signal", "d": ["ring solid GREEN: NFC and distance sensor OK · AMBER: one is missing",
                                   "two short motor pulses, ~300 ms, then the ring goes dark"]},
    ]
    loop = ["power: hold the button 2 s → sleep · tap → confirm a pending pairing",
            "battery checked every 10 s: low < 3.6 V (orange), critical < 3.3 V (sleep)",
            "status LED: red / yellow / green link, blue tag flash, blue blink pairing",
            "distance sensor read without blocking",
            "touch state machine: connect → silence period → release",
            "vibration follows proximity (about 20-300 mm), when idle",
            "NFC UID from the queue → ESP-NOW NFC message to the body",
            "if no touch active: motor double-click, TOUCH message (= confirmed), white flash",
            "proximity message every 100 ms · LED ring at 25 fps · serial commands"]
    s = boot_flow("boot_hand", "Hand unit — startup / wake sequence, then the main loop",
                  "setup() on the ESP32-S3; sleep and wake use the button on GPIO 9.", "hand", steps, "loop() — repeated continuously", loop)
    s.save()


@figure
def boot_body():
    steps = [
        {"t": "CPU 80 MHz · Serial 115200 · banner", "d": ["Body Bridge"]},
        {"t": "UART to the Teensy", "d": ["Serial1 at 115200: TX GPIO 43 → Teensy pin 0, RX GPIO 44 ← Teensy pin 1"]},
        {"t": "Status LED and pair button", "d": ["status LED on GPIO 48 (colour order GRB on the standard boards)",
                                                  "pair button on GPIO 9, internal pull-up"]},
        {"t": "ESP-NOW", "d": ["Wi-Fi station mode, ESP-NOW init, broadcast peer added",
                               "stored hand-unit pairing loaded from flash"],
         "branch": ("no stored pairing", ["Status LED red", "pair with 'espnow scan' on the hand unit,", "or hold the body's button 2 s"], "red")},
        {"t": "Waiting for the hand unit", "d": ["paired but silent: yellow · packets arriving from the paired hand: green"]},
    ]
    loop = ["serial commands: status · mac · stream · clear · help",
            "status LED derived every frame from pairing + time of the last packet",
            "heartbeat to the hand unit every 1 s",
            "pair button: hold 2 s → pairing mode, PAIR_REQUEST broadcast every 300 ms for 30 s",
            "ESP-NOW receive: PING → PONG and pair · PAIR_ACK → pair",
            "data (proximity, NFC, touch) is accepted ONLY from the paired hand",
            "→ forwarded to the Teensy as JSON lines  p / n / c",
            "packets from anyone else are counted and ignored"]
    s = boot_flow("boot_body", "Body bridge — startup sequence, then the main loop",
                  "setup() on the ESP32-S3; the bridge has no sleep mode.", "body", steps, "loop() and the receive callback", loop)
    s.save()


# ─────────────────────────────────────────────────────────────
#  6. Sequence diagrams
# ─────────────────────────────────────────────────────────────

class Seq:
    """Minimal sequence diagram: participants with lifelines, messages and notes."""

    def __init__(self, s, parts, top):
        # parts: {key: (title, subtitle, kind, x_center)}
        self.s, self.parts, self.top, self.ev = s, parts, top, []

    def msg(self, a, b, text, detail=None, color="#4b5563", dash=None, both=False):
        self.ev.append(("msg", a, b, text, detail, color, dash, both))

    def note(self, keys, lines, kind="gray", width=None):
        self.ev.append(("note", keys, lines, kind, width))

    def gap(self, px=12):
        self.ev.append(("gap", px))

    def section(self, text):
        self.ev.append(("section", text))

    def render(self):
        s, P = self.s, self.parts
        y = self.top + 44 + 20
        layout = []
        for e in self.ev:
            if e[0] == "msg":
                h = 46 + (14 if e[4] else 0)
            elif e[0] == "note":
                h = len(e[2]) * 15 + 22 + 12
            elif e[0] == "section":
                h = 34
            else:
                h = e[1]
            layout.append((y, h)); y += h
        bottom = y + 8
        # lifelines first (painter order)
        for k, (t, sub, kind, x) in P.items():
            s.line(x, self.top + 44, x, bottom, "#d1d5db", 1.4, "5,4")
        for e, (yy, h) in zip(self.ev, layout):
            if e[0] == "msg":
                _, a, b, text, detail, color, dash, both = e
                xa, xb = P[a][3], P[b][3]
                ya = yy + h - 12
                s.arrow([(xa, ya), (xb, ya)], color, 1.8, dash, both)
                s.label((xa + xb) / 2, ya - 8 - (14 if detail else 0) + (0 if not detail else 0), text, 11.5, INK, bold=True)
                if detail:
                    s.text((xa + xb) / 2, ya - 8, detail, 10, MUTED, "middle")
            elif e[0] == "note":
                _, keys, lines, kind, width = e
                xs = [P[k][3] for k in keys]
                x1, x2 = min(xs), max(xs)
                w = width or max(x2 - x1 + 180, max(tw(l, 10.5) for l in lines) + 28)
                cx = (x1 + x2) / 2
                fill, stroke, accent = C[kind]
                s.rect(cx - w / 2, yy + 4, w, h - 12, fill, stroke, 7, 1.3)
                s.lines(cx - w / 2 + 12, yy + 22, lines, 10.5, INK, lh=15)
            elif e[0] == "section":
                s.text(self.s.w / 2 if False else 24, yy + 22, e[1], 11.5, MUTED, weight="bold")
                s.line(24, yy + 28, s.w - 24, yy + 28, "#e5e7eb", 1)
        # participant headers on top
        for k, (t, sub, kind, x) in P.items():
            fill, stroke, accent = C[kind]
            s.rect(x - 90, self.top, 180, 44, fill, stroke, 8, 1.6)
            s.text(x, self.top + 19, t, 12.5, accent, "middle", "bold")
            s.text(x, self.top + 34, sub, 9.5, MUTED, "middle")
        return bottom


@figure
def pairing_sequence():
    s = Svg("pairing_sequence", 1500, 820, "Pairing a hand unit with its body bridge",
            "Two ways. Both store the other unit's MAC address in flash, so pairing survives reboots and sleep.")
    s.text(24, 90, "A · from the hand unit's serial CLI", 14, INK, weight="bold")
    s.text(780, 90, "B · with the two buttons", 14, INK, weight="bold")

    A = Seq(s, {"h": ("Hand unit", "serial CLI", "hand", 170), "b": ("Body bridge", "powered, unpaired", "body", 570)}, 104)
    A.note(["h"], ["you type", "espnow scan"], "gray", 180)
    A.msg("h", "b", "PING", "broadcast", "#ea580c")
    A.note(["b"], ["pairs with the sender", "stores its MAC in flash"], "body", 240)
    A.msg("b", "h", "PONG", "unicast reply", "#16a34a")
    A.note(["h"], ["pairs with the body", "stores its MAC in flash"], "hand", 240)
    A.msg("b", "h", "HEARTBEAT every 1 s", None, "#16a34a", "6,4")
    A.msg("h", "b", "PROXIMITY every 100 ms", None, "#ea580c")
    A.note(["h", "b"], ["both status LEDs turn GREEN", "(red = unpaired · yellow = paired, no answer)"], "gray")
    A.note(["h", "b"], ["The body pairs with whichever hand unit pings.", "With several pairs in one room, pair one set at a time."], "red")
    ba = A.render()

    B = Seq(s, {"h": ("Hand unit", "awake, unpaired", "hand", 870), "b": ("Body bridge", "powered", "body", 1270)}, 104)
    B.note(["b"], ["hold the button (GPIO 9)", "for 2 seconds"], "body", 230)
    B.note(["b"], ["pairing mode: LED blinks", "BLUE fast, for up to 30 s"], "body", 230)
    B.msg("b", "h", "PAIR_REQUEST", "broadcast every 300 ms", "#2563eb")
    B.note(["h"], ["LED blinks BLUE:", "a request is pending"], "hand", 220)
    B.note(["h"], ["you tap the hand unit's", "button (shorter than 2 s)"], "gray", 220)
    B.msg("h", "b", "PAIR_ACK", "unicast", "#ea580c")
    B.note(["h", "b"], ["both store each other's MAC; pairing mode ends", "HEARTBEAT / PROXIMITY start → both LEDs GREEN"], "gray")
    B.note(["h", "b"], ["Only the body in pairing mode can be paired this way,", "so it is the safe method with many pairs in one room."], "red")
    bb = B.render()
    bottom = max(ba, bb)
    s.line(755, 76, 755, bottom, "#e5e7eb", 1.5)
    s.fit(bottom + 30)
    s.save()


@figure
def runtime_messages():
    s = Svg("runtime_messages", 1500, 800, "What happens when a tag is tapped",
            "ESP-NOW between hand unit and body bridge, JSON lines over UART from the body bridge to the Teensy.")
    P = {"t": ("NFC tag", "", "tag", 140), "h": ("Hand unit", "ESP32-S3 + PN532", "hand", 500),
         "b": ("Body bridge", "ESP32-S3", "body", 900), "p": ("Teensy player", "main.cpp", "teensy", 1330)}
    q = Seq(s, P, 84)
    q.section("continuously")
    q.msg("h", "b", "PROXIMITY  0x01", "every 100 ms  (distance in mm, touch state)", "#ea580c")
    q.msg("b", "p", '{"t":"p","d":150,"s":0}', "informational — not acted on", "#16a34a")
    q.msg("b", "h", "HEARTBEAT  0x12", "every 1 s — keeps the hand unit's LED green", "#16a34a", "6,4")
    q.section("a tag is tapped")
    q.msg("t", "h", "tap", "the PN532 reads the UID", "#9333ea")
    q.msg("h", "b", "NFC  0x02", "raw detection of a tag in range", "#ea580c")
    q.msg("b", "p", '{"t":"n","u":"DE0CC698","d":150}', "logged only — this is how you discover a tag's UID", "#16a34a")
    q.note(["h"], ["touch state machine is idle →", "motor double-click + white flash"], "hand", 260)
    q.msg("h", "b", "TOUCH  0x03", "= a confirmed connection (once per genuine tap)", "#ea580c")
    q.msg("b", "p", '{"t":"c","u":"DE0CC698"}', "the only message that dispatches", "#dc2626")
    q.note(["b", "p"], ["look up the UID → role (start, person, narrator, overlay)",
                   "per-tag cooldown · show over? · ending? · encounter lock?",
                   "→ chapter switch with crossfade · overlay toggle · effect · LED",
                   "'connect' or 'denied' sound; general audio ducks while it plays"], "teensy", 600)
    bottom = q.render()
    s.fit(bottom + 30)
    s.save()


# ─────────────────────────────────────────────────────────────
#  7. LED language
# ─────────────────────────────────────────────────────────────

def _wave(s, x, y, w, h, kind, color):
    """A small plot of one blink pattern, left to right."""
    import math
    pts = []
    n = 120
    for i in range(n + 1):
        t = i / n
        if kind == "slow":      v = 0.5 - 0.5 * math.cos(2 * math.pi * t * 1)
        elif kind == "pulse":   v = 0.5 - 0.5 * math.cos(2 * math.pi * t * 2)
        elif kind == "fast":    v = 0.5 - 0.5 * math.cos(2 * math.pi * t * 4)
        elif kind == "double":  v = max(0, math.sin(2 * math.pi * ((t * 2) % 1) * 2)) if (t * 2) % 1 < 0.5 else 0
        elif kind == "blink":   v = 1.0 if int(t * 8) % 2 == 0 else 0.0
        else:                   v = 1.0
        pts.append((x + t * w, y + h - v * h))
    s.add(f'<rect x="{x}" y="{y - 4}" width="{w}" height="{h + 8}" rx="6" fill="#111827"/>')
    s.add('<polyline points="' + " ".join(f"{a:.1f},{b:.1f}" for a, b in pts) +
          f'" fill="none" stroke="{color}" stroke-width="2.2" stroke-linejoin="round"/>')


@figure
def led_language():
    s = Svg("led_language", 1500, 700, "What the LEDs tell you",
            "The status LED (the board's own RGB LED on GPIO 48) means the same on the hand unit and the body bridge. The colour is recomputed every frame — nothing latches.")
    rows = [("#ef4444", "slow", "red · slow pulse", "not paired", "both"),
            ("#facc15", "pulse", "yellow · pulse", "paired, but the other unit is not answering", "both"),
            ("#22c55e", "pulse", "green · pulse", "linked — packets arriving from the paired unit", "both"),
            ("#3b82f6", "double", "blue · quick double pulse", "a tag / touch message was just sent or received", "both"),
            ("#3b82f6", "blink", "blue · fast blink", "pairing: body = pairing mode (button held 2 s) · hand = a request is waiting for your tap", "both"),
            ("#f97316", "fast", "orange · fast pulse", "battery low (below 3.6 V corrected; shuts down below 3.3 V)", "hand only")]
    y = 92
    s.text(24, y - 8, "Status LED", 14, INK, weight="bold")
    for color, kind, name, meaning, who in rows:
        s.rect(24, y, 1452, 52, "#f9fafb", "#e5e7eb", 8, 1.2)
        s.circle(58, y + 26, 14, color)
        s.text(90, y + 22, name, 13, INK, weight="bold")
        s.text(90, y + 40, who, 10.5, MUTED)
        _wave(s, 400, y + 10, 190, 28, kind, color)
        s.text(620, y + 31, meaning, 12, INK)
        y += 62
    s.text(24, y + 22, "A red LED that glows faintly next to the status LED is a separate red LED on GPIO 48 (see Context.md).", 10.5, MUTED)
    y += 56

    s.text(24, y, "Other lights", 14, INK, weight="bold")
    y += 14
    others = [("hand unit · ring", "solid green at start-up", "NFC reader and distance sensor both OK  (amber: one is missing)"),
              ("hand unit · ring", "white flash on a tap", "the tag was accepted as a connection"),
              ("hand unit · vibration", "double click on a tap · gentle buzz near a hand", "intensity follows the distance, about 20-300 mm"),
              ("player · ring", "the chapter's own mood", "level / heartbeat follow the live haptic signal; each chapter sets its colour and entry flash"),
              ("player · ring", "dark", "the show is over — only the start tag brings it back"),
              ("player · onboard LED", "fast blink", "the SD card could not be mounted")]
    for who, what, meaning in others:
        s.rect(24, y, 1452, 40, "#f9fafb", "#e5e7eb", 8, 1.2)
        s.text(40, y + 25, who, 12, INK, weight="bold")
        s.text(300, y + 25, what, 12, INK)
        s.text(700, y + 25, meaning, 11.5, MUTED)
        y += 48
    s.fit(y + 16)
    s.save()


# ─────────────────────────────────────────────────────────────
#  8. Experience map (experience/Visualise.py on an example card)
# ─────────────────────────────────────────────────────────────

def _load_module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


@figure
def experience_map():
    """Player 1's card from the real generator, drawn by the real visualiser."""
    gen = _load_module("gen", ROOT / "configs" / "generate_player_configs.py")
    vis = _load_module("vis", ROOT / "experience" / "Visualise.py")

    def uids(first, n):                       # first = a real tag, the rest are made-up spares
        return [first] + [f"{first[:4]}{i:04X}" for i in range(1, n)]

    cat = {"players": {1: uids("5040DABE", 3), 2: uids("1033D9BE", 3), 3: uids("505EDABE", 3), 4: uids("503CDABE", 3),
                       5: uids("2031D9BE", 3), 6: uids("605CDABE", 3), 7: uids("5038DABE", 3)},
           "start": uids("302FD9BE", 3), "narrator": uids("805ADABE", 3),
           "overlay1": uids("7034DABE", 6), "overlay2": uids("70A1DABE", 6)}
    cfg = gen.build_config(cat, 0, False, 10.0, 0.4, lambda m: None)
    svg = vis.generate(cfg)
    DOCS.mkdir(exist_ok=True)
    (DOCS / "experience_map.svg").write_text(svg)
    (DOCS / "experience_map.html").write_text(vis.build_html(svg, cfg))
    cairosvg.svg2png(bytestring=svg.encode("utf-8"), write_to=str(DOCS / "experience_map.png"), scale=2)
    print("  docs/experience_map.svg + .png + .html")


# ─────────────────────────────────────────────────────────────
#  9. PCB placeholders — never overwrites a real file
# ─────────────────────────────────────────────────────────────

PCB_FILES = [
    ("pcb_player_schematic",    "Player PCB - schematic",     "Teensy 4.1 carrier: PCM5102A, MAX98357A, LED ring, bridge UART"),
    ("pcb_player_board",        "Player PCB - board layout",  "top / bottom view of the finished layout"),
    ("pcb_hand_unit_schematic", "Hand unit PCB - schematic",  "ESP32-S3 SuperMini, PN532, VL53L0X, motor driver, LED ring, battery"),
    ("pcb_hand_unit_board",     "Hand unit PCB - board layout", "top / bottom view of the finished layout"),
]


@figure
def placeholders():
    from PIL import Image, ImageDraw
    DOCS.mkdir(exist_ok=True)
    made = []
    for stem, title, what in PCB_FILES:
        png, pdf = DOCS / f"{stem}.png", DOCS / f"{stem}.pdf"
        if png.exists() and pdf.exists():
            continue
        im = Image.new("RGB", (1600, 1000), "#f3f4f6")
        d = ImageDraw.Draw(im)
        for i in range(-1000, 1600, 60):                       # diagonal hatching
            d.line([(i, 0), (i + 1000, 1000)], fill="#e5e7eb", width=2)
        d.rectangle([40, 40, 1560, 960], outline="#9ca3af", width=4)
        f_big = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", 84)
        f_mid = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", 44)
        f_sm = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 30)
        def center(y, text, font, fill):
            w = d.textlength(text, font=font)
            d.text(((1600 - w) / 2, y), text, font=font, fill=fill)
        center(300, "PLACEHOLDER", f_big, "#6b7280")
        center(430, title, f_mid, "#1f2937")
        center(510, what, f_sm, "#6b7280")
        center(640, f"replace  docs/{stem}.png  and  docs/{stem}.pdf", f_sm, "#374151")
        center(690, "with the exported schematic / board images (same file names)", f_sm, "#374151")
        if not png.exists():
            im.save(png)
        if not pdf.exists():
            im.save(pdf, "PDF", resolution=150)
        made.append(stem)
    print("  placeholders:", ", ".join(made) if made else "none needed (files already exist)")


# ─────────────────────────────────────────────────────────────
if __name__ == "__main__":
    wanted = sys.argv[1:]
    print("Writing figures to", DOCS)
    for name, fn in FIGURES.items():
        if not wanted or any(w in name for w in wanted):
            fn()
