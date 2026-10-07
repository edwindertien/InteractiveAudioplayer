#!/usr/bin/env python3
"""
Visualise.py — Interactive Audio Player config visualiser
Reads a config.json (or a generated playerN_config.json) and draws the
experience map: chapters on a timeline, overlay states, LED colours, which
chapters end where (returns_to / the end chapter), and which tag does what.

Usage:
    python3 Visualise.py                          # ./config.json -> experience_map.html, opens browser
    python3 Visualise.py my_config.json           # custom input
    python3 Visualise.py config.json out.html     # custom output, no auto-open
    python3 Visualise.py config.json out.html --svg map.svg --png map.png
                                                  # also write the bare SVG / a PNG
                                                  # (PNG needs:  pip install cairosvg)
"""

import json, sys, os, webbrowser, re
from pathlib import Path

# ── Load config ───────────────────────────────────────────────

def load(path):
    with open(path) as f:
        raw = f.read()
    # Strip _note keys (JSON with comments workaround)
    raw = re.sub(r'"_[^"]*"\s*:\s*"[^"]*"\s*,?\s*', '', raw)
    raw = re.sub(r',\s*}', '}', raw)
    raw = re.sub(r',\s*]', ']', raw)
    return json.loads(raw)

# ── Colour helpers ────────────────────────────────────────────

LED_PALETTES = {
    "breathe":    lambda c: hex_to_rgb(c),
    "fire":       lambda c: (220, 80, 10),
    "ocean":      lambda c: (0,  100, 200),
    "rainbow":    lambda c: (200, 80, 200),
    "sparkle":    lambda c: hex_to_rgb(c),
    "comet":      lambda c: hex_to_rgb(c),
    "pulse_ring": lambda c: hex_to_rgb(c),
    "spin":       lambda c: hex_to_rgb(c),
    "flash":      lambda c: hex_to_rgb(c),
    "confetti":   lambda c: (200, 100, 200),
    "solid":      lambda c: hex_to_rgb(c),
    "off":        lambda c: (20, 20, 20),
}

def hex_to_rgb(h):
    h = str(h).replace("0x","").replace("#","").zfill(6)
    return (int(h[0:2],16), int(h[2:4],16), int(h[4:6],16))

def led_color(chapter):
    bg = chapter.get("led_background", {})
    anim = bg.get("animation", "solid")
    color = bg.get("color", "0x444444")
    intensity = float(bg.get("intensity", 0.7))
    fn = LED_PALETTES.get(anim, lambda c: hex_to_rgb(c))
    r, g, b = fn(color)
    r = int(r * intensity)
    g = int(g * intensity)
    b = int(b * intensity)
    return f"rgb({r},{g},{b})"

def text_color(bg_css):
    """White or black text based on luminance."""
    m = re.match(r'rgb\((\d+),(\d+),(\d+)\)', bg_css)
    if not m: return "#fff"
    r,g,b = int(m[1]),int(m[2]),int(m[3])
    lum = 0.299*r + 0.587*g + 0.114*b
    return "#111" if lum > 128 else "#eee"

# ── Tag helpers ───────────────────────────────────────────────

# colour + icon per tag type
TAG_STYLE = {
    "person":   ("#ffcc66", "●"),   # starts a chapter (encounter / recharge)
    "start":    ("#66ff99", "▶"),   # begins the show, starts the end-time clock
    "location": ("#88ccff", "◆"),
    "device":   ("#ff88ff", "◈"),
}

def tag_style(tag):
    return TAG_STYLE.get(tag.get("type", "location"), TAG_STYLE["location"])

def tag_uids(tag):
    """Every uid of a tag entry: 'uid' and/or the 'uids' list (spares)."""
    out = []
    if tag.get("uid"): out.append(tag["uid"])
    for u in tag.get("uids", []):
        if u and u not in out: out.append(u)
    return out

def uid_label(tag, width=14):
    u = tag_uids(tag)
    if not u: return "?"
    return u[0][:width] + (f" +{len(u)-1}" if len(u) > 1 else "")

def xml(s):
    return str(s).replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")

# ── Layout constants ──────────────────────────────────────────

W        = 1200   # total SVG width
MARGIN_L = 120    # left margin for labels
MARGIN_R = 20
TRACK_H  = 64     # chapter block height
OV_H     = 14     # overlay row height
OV_GAP   = 2
SECTION_GAP = 32
FONT     = "JetBrains Mono, Consolas, monospace"

# ── HTML generator ────────────────────────────────────────────

def generate(config):
    chapters  = config.get("chapters", [])
    tags      = config.get("tags",     [])
    device_id = config.get("device_id", "UNIT_01")
    start_ch  = config.get("start_chapter", "")
    idle_led  = config.get("idle", {}).get("led", {})
    end_ch    = config.get("end_chapter", "end_reflection")
    end_min   = float(config.get("end_after_min", 0) or 0)
    lock_on   = bool(config.get("encounter_lock", False))
    duck      = float(config.get("duck_level", 1.0))
    sounds    = config.get("sounds", {})

    # Timeline extent
    total_ms  = max((c.get("loop_end_ms", 0) for c in chapters), default=0)
    if total_ms == 0: total_ms = 1
    TW = W - MARGIN_L - MARGIN_R   # drawable timeline width

    def ms_to_x(ms):
        return MARGIN_L + (ms / total_ms) * TW

    # ── Build chapter map ─────────────────────────────────────
    ch_map = {c["id"]: c for c in chapters}

    # ── Compute SVG height ────────────────────────────────────
    overlay_rows = 3  # ov1, ov2, narr
    timeline_h   = TRACK_H + overlay_rows * (OV_H + OV_GAP) + 8

    # Tag flow section — one row per tag
    flow_h = max(len(tags) * 28 + 40, 40)

    # Tag table section
    table_h = len(tags) * 28 + 60

    total_h = 78 + timeline_h + SECTION_GAP + flow_h + SECTION_GAP + table_h + 40

    svg_parts = []

    def s(text):
        svg_parts.append(text)

    # ── SVG header ────────────────────────────────────────────
    s(f'''<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{total_h}"
          viewBox="0 0 {W} {total_h}" style="font-family:{FONT}; background:#1a1a2e;">''')
    s(f'<rect width="{W}" height="{total_h}" fill="#1a1a2e"/>')   # explicit background (PNG export)

    # ── Title ─────────────────────────────────────────────────
    s(f'<text x="12" y="26" fill="#aaa" font-size="11">device: {xml(device_id)}</text>')
    s(f'<text x="{W//2}" y="26" fill="#eee" font-size="14" font-weight="bold" '
      f'text-anchor="middle">Interactive Audio Player — Experience Map</text>')
    s(f'<text x="{W-12}" y="26" fill="#aaa" font-size="11" text-anchor="end">'
      f'start chapter: {xml(start_ch)}</text>')
    # one-line summary of the show settings
    bits = []
    bits.append(f"timed ending: {end_min:g} min after the start tag \u2192 {end_ch}" if end_min > 0 else "no timed ending")
    bits.append("person tags only in base" if lock_on else "person tags work at any time")
    if duck < 1.0: bits.append(f"effects duck the audio to {duck:g}")
    if sounds: bits.append("sounds: " + " ".join(f"{k}={v}" for k, v in sounds.items()))
    summary = "  \u00b7  ".join(bits)          # (computed here: a backslash inside an f-string expression needs Python 3.12)
    s(f'<text x="{W//2}" y="44" fill="#778" font-size="10" text-anchor="middle">{xml(summary)}</text>')

    y0 = 62  # top of timeline area

    # ── Time ruler ───────────────────────────────────────────
    tick_every = 10000  # ms (grid lines; seconds are labelled every 30 s, below the overlay rows)
    t = 0
    while t <= total_ms:
        x = ms_to_x(t)
        s(f'<line x1="{x:.1f}" y1="{y0}" x2="{x:.1f}" y2="{y0+timeline_h-4}" '
          f'stroke="#333" stroke-width="1"/>')
        if t % 30000 == 0:
            s(f'<text x="{x:.1f}" y="{y0+timeline_h+6}" fill="#556" font-size="8" '
              f'text-anchor="middle">{t//1000}s</text>')
        t += tick_every

    # ── Chapter blocks ────────────────────────────────────────
    ov_labels = [("ov1","#4a9eff"), ("ov2","#ff9f4a"), ("narr","#9fff4a")]
    ov_default_colors = {"ov1":"#4a9eff","ov2":"#ff9f4a","narr":"#9fff4a"}

    for ch in chapters:
        x1   = ms_to_x(ch.get("start_ms", 0))
        x2   = ms_to_x(ch.get("loop_end_ms", total_ms))
        w    = max(x2 - x1, 2)
        bgc  = led_color(ch)
        txtc = text_color(bgc)
        loop = ch.get("loop", True)
        mode = ch.get("overlay_mode","fresh")
        anim = ch.get("led_background",{}).get("animation","?")

        # Main chapter block
        s(f'<rect x="{x1:.1f}" y="{y0}" width="{w:.1f}" height="{TRACK_H}" '
          f'fill="{bgc}" rx="3" stroke="#111" stroke-width="1.5"/>')

        # Chapter label — "Encounter — Suspense" wraps onto two lines to fit the block
        label = ch.get("label", ch.get("id","?"))
        parts = [p.strip() for p in label.split("\u2014", 1)] if "\u2014" in label else [label]
        cap   = max(8, int(w / 6.2))                       # characters that fit at this width
        parts = [q if len(q) <= cap else q[:cap-1] + "\u2026" for q in parts]
        if len(parts) == 1:
            s(f'<text x="{x1+w/2:.1f}" y="{y0+21}" fill="{txtc}" font-size="10" '
              f'font-weight="bold" text-anchor="middle">{xml(parts[0])}</text>')
        else:
            s(f'<text x="{x1+w/2:.1f}" y="{y0+16}" fill="{txtc}" font-size="10" '
              f'font-weight="bold" text-anchor="middle">{xml(parts[0])}</text>')
            s(f'<text x="{x1+w/2:.1f}" y="{y0+28}" fill="{txtc}" font-size="10" '
              f'font-weight="bold" text-anchor="middle">{xml(parts[1])}</text>')

        # Duration
        dur_ms = ch.get("loop_end_ms",0) - ch.get("start_ms",0)
        s(f'<text x="{x1+w/2:.1f}" y="{y0+41}" fill="{txtc}" font-size="9" '
          f'text-anchor="middle" opacity="0.8">'
          f'{ch["start_ms"]//1000}s – {ch.get("loop_end_ms",0)//1000}s</text>')

        # Loop/Once indicator: where does a once-chapter go when it ends?
        ret = ch.get("returns_to", "")
        if loop:
            loop_tag, loop_col = "LOOP", "#88ff88"
        elif ch.get("id") == end_ch:
            loop_tag, loop_col = "ONCE \u2192 OFF", "#ff6666"
        elif ret:
            loop_tag, loop_col = f"ONCE \u2192 {ret}", "#ffbb66"
        else:
            loop_tag, loop_col = "ONCE \u2192 IDLE", "#ff8888"
        s(f'<text x="{x1+w/2:.1f}" y="{y0+53}" fill="{loop_col}" font-size="8" '
          f'text-anchor="middle">{xml(loop_tag)}</text>')
        s(f'<text x="{x1+w/2:.1f}" y="{y0+61}" fill="{txtc}" font-size="7" opacity="0.65" '
          f'text-anchor="middle">{xml(anim)}</text>')

        # START / END badges above the block
        if ch.get("id") == start_ch:
            s(f'<text x="{x1+3:.1f}" y="{y0-5}" fill="#66ff99" font-size="8" font-weight="bold">\u25b6 START</text>')
        if ch.get("id") == end_ch and end_min > 0:
            s(f'<text x="{x1+w-3:.1f}" y="{y0-5}" fill="#ff6666" font-size="8" font-weight="bold" '
              f'text-anchor="end">END (after {end_min:g} min) \u25a0</text>')

        # Overlay state rows
        ov_state = ch.get("overlays", {})
        for i, (ov_name, ov_col) in enumerate(ov_labels):
            oy = y0 + TRACK_H + 4 + i * (OV_H + OV_GAP)
            on = ov_state.get(ov_name, False)
            if mode == "keep" and not on:
                fill = "#334"
                label_s = f"{ov_name} (keep)"
            elif on:
                fill = ov_col
                label_s = ov_name
            else:
                fill = "#1e1e2e"
                label_s = ov_name
            s(f'<rect x="{x1:.1f}" y="{oy}" width="{w:.1f}" height="{OV_H}" '
              f'fill="{fill}" rx="2" stroke="#111" stroke-width="0.5"/>')
            if i == 0 and x1 < MARGIN_L + 5:
                pass  # labels added separately below

        # LED swatch dot
        s(f'<circle cx="{x2-7:.1f}" cy="{y0+7}" r="4" fill="{bgc}" '
          f'stroke="#888" stroke-width="0.8"/>')

    # Overlay row labels (left margin)
    for i, (ov_name, ov_col) in enumerate(ov_labels):
        oy = y0 + TRACK_H + 4 + i * (OV_H + OV_GAP) + OV_H//2 + 3
        s(f'<text x="{MARGIN_L-4}" y="{oy}" fill="{ov_col}" font-size="9" '
          f'text-anchor="end">{ov_name}</text>')


    # ── Section divider ───────────────────────────────────────
    y_flow = y0 + timeline_h + SECTION_GAP
    s(f'<line x1="0" y1="{y_flow-14}" x2="{W}" y2="{y_flow-14}" '
      f'stroke="#333" stroke-width="1"/>')
    s(f'<text x="12" y="{y_flow-3}" fill="#666" font-size="10">TAG NAVIGATION</text>')

    # ── Tag flow rows ─────────────────────────────────────────
    # Chapter centre X positions for arrows
    ch_cx = {}
    for ch in chapters:
        x1 = ms_to_x(ch.get("start_ms",0))
        x2 = ms_to_x(ch.get("loop_end_ms", total_ms))
        ch_cx[ch["id"]] = (x1 + x2) / 2

    for ti, tag in enumerate(tags):
        ty = y_flow + 18 + ti * 28
        uid   = uid_label(tag)
        label = tag.get("label","")
        ttype = tag.get("type","location")
        actions = tag.get("actions",[])

        col, icon = tag_style(tag)

        # Tag pill
        s(f'<rect x="0" y="{ty-11}" width="{MARGIN_L-8}" height="16" '
          f'fill="#1e1e2e" rx="3" stroke="{col}" stroke-width="0.8"/>')
        s(f'<text x="4" y="{ty+1}" fill="{col}" font-size="8">'
          f'{icon} {uid[:14]}</text>')

        # Label
        short_l = label[:40] if len(label) <= 40 else label[:38]+"…"
        arrow_x = MARGIN_L + len(short_l) * 5.6 + 8        # arrows start after the label text
        s(f'<text x="{MARGIN_L}" y="{ty+1}" fill="#888" font-size="9">'
          f'{xml(short_l)}</text>')

        # Draw arrows to target chapters
        for act in actions:
            if act.get("do") == "chapter":
                target = act.get("target","")
                if target in ch_cx:
                    tx = ch_cx[target]
                    s(f'<line x1="{arrow_x:.1f}" y1="{ty-3}" x2="{tx:.1f}" '
                      f'y2="{y0+TRACK_H-2}" stroke="{col}" stroke-width="1.2" '
                      f'stroke-dasharray="3,2" opacity="0.7" '
                      f'marker-end="url(#arr)"/>')
            elif act.get("do") == "overlay":
                ov = act.get("target","")
                val = act.get("value","toggle")
                ov_col2 = ov_default_colors.get(ov,"#aaa")
                s(f'<text x="{arrow_x + 6:.1f}" y="{ty+1}" fill="{ov_col2}" '
                  f'font-size="8">  {ov} → {val}</text>')
            elif act.get("do") == "fx":
                s(f'<text x="{arrow_x + 90:.1f}" y="{ty+1}" fill="#ffcc44" '
                  f'font-size="8">  fx:{act.get("slot","?")}</text>')
            elif act.get("do") == "led":
                anim2 = act.get("animation","?")
                s(f'<text x="{arrow_x + 140:.1f}" y="{ty+1}" fill="#ff88aa" '
                  f'font-size="8">  led:{anim2}</text>')

    # Arrow marker def
    s('''<defs>
      <marker id="arr" markerWidth="6" markerHeight="6" refX="5" refY="3"
              orient="auto">
        <path d="M0,0 L6,3 L0,6 Z" fill="#88ccff" opacity="0.7"/>
      </marker>
    </defs>''')

    # ── Section divider ───────────────────────────────────────
    y_table = y_flow + flow_h + SECTION_GAP
    s(f'<line x1="0" y1="{y_table-14}" x2="{W}" y2="{y_table-14}" '
      f'stroke="#333" stroke-width="1"/>')
    s(f'<text x="12" y="{y_table-3}" fill="#666" font-size="10">TAG INDEX</text>')

    # ── Tag table ─────────────────────────────────────────────
    col_x = [12, 160, 420, 500, 700]
    headers = ["UID (+ spares)", "Label", "Type", "Actions", "LED anim"]
    for hi, hdr in enumerate(headers):
        s(f'<text x="{col_x[hi]}" y="{y_table+14}" fill="#555" '
          f'font-size="9" font-weight="bold">{hdr}</text>')

    for ti, tag in enumerate(tags):
        ry = y_table + 30 + ti * 26
        uid   = uid_label(tag)
        label = tag.get("label","")
        ttype = tag.get("type","location")
        actions = tag.get("actions",[])

        action_strs = []
        led_anim = ""
        for a in actions:
            d = a.get("do","")
            if d == "chapter": action_strs.append(f"→{a.get('target','')}")
            elif d == "overlay": action_strs.append(f"{a.get('target','')}:{a.get('value','')}")
            elif d == "fx": action_strs.append(f"fx{a.get('slot','')}")
            elif d == "led": led_anim = a.get("animation","?")

        row_fill = "#1a1a2e" if ti % 2 == 0 else "#1e1e35"
        s(f'<rect x="0" y="{ry-11}" width="{W}" height="22" fill="{row_fill}"/>')

        tag_col = tag_style(tag)[0]
        vals = [uid, label[:40], ttype, "  ".join(action_strs), led_anim]
        for ci, val in enumerate(vals):
            s(f'<text x="{col_x[ci]}" y="{ry+4}" fill="{tag_col if ci==0 else "#aaa"}" '
              f'font-size="9">{xml(val)}</text>')

    # ── Idle indicator ────────────────────────────────────────
    idle_anim = idle_led.get("animation","breathe")
    idle_col_hex = idle_led.get("color","0x001133")
    r2,g2,b2 = hex_to_rgb(idle_col_hex)
    idle_css = f"rgb({int(r2*0.35)},{int(g2*0.35)},{int(b2*0.35)})"
    s(f'<rect x="{ms_to_x(0)-28:.1f}" y="{y0}" width="24" height="{TRACK_H}" '
      f'fill="{idle_css}" rx="3" stroke="#555" stroke-width="1" '
      f'stroke-dasharray="3,2"/>')
    s(f'<text x="{ms_to_x(0)-16:.1f}" y="{y0+TRACK_H//2+8}" fill="#888" font-size="8" '
      f'text-anchor="middle" transform="rotate(-90,{ms_to_x(0)-16:.1f},{y0+TRACK_H//2+8})">IDLE</text>')

    s('</svg>')
    return "\n".join(svg_parts)


def build_html(svg, config):
    device_id = config.get("device_id","UNIT_01")
    return f"""<!DOCTYPE html>
<html>
<head>
  <meta charset="utf-8">
  <title>Experience Map — {device_id}</title>
  <style>
    body {{ background:#0d0d1a; margin:0; padding:20px;
           font-family: 'JetBrains Mono', Consolas, monospace; color:#aaa; }}
    h1 {{ color:#eee; font-size:16px; margin-bottom:8px; }}
    .hint {{ font-size:11px; color:#555; margin-bottom:16px; }}
    .wrap {{ overflow-x:auto; }}
    svg {{ min-width:1200px; }}
    .legend {{ display:flex; gap:24px; margin-top:12px; font-size:10px; color:#666; }}
    .legend span {{ display:flex; align-items:center; gap:6px; }}
    .dot {{ width:10px; height:10px; border-radius:2px; display:inline-block; }}
  </style>
</head>
<body>
  <h1>🎵 Interactive Audio Experience Map</h1>
  <div class="hint">
    Generated from the config &nbsp;·&nbsp;
    Chapters on timeline · Tags as dashed arrows to target chapters
  </div>
  <div class="wrap">{svg}</div>
  <div class="legend">
    <span><span class="dot" style="background:#4a9eff"></span>ov1 overlay</span>
    <span><span class="dot" style="background:#ff9f4a"></span>ov2 overlay</span>
    <span><span class="dot" style="background:#9fff4a"></span>narr overlay</span>
    <span><span class="dot" style="background:#334; border:1px solid #555"></span>keep (inherits)</span>
    <span><span class="dot" style="background:#ffcc66; border-radius:50%"></span>person tag (starts a chapter)</span>
    <span><span class="dot" style="background:#66ff99; border-radius:50%"></span>start tag (begins the show)</span>
    <span><span class="dot" style="background:#88ccff; border-radius:50%"></span>location tag (overlays, narrator)</span>
    <span style="color:#88ff88">LOOP — chapter loops</span>
    <span style="color:#ffbb66">ONCE → x — plays once, then continues with chapter x</span>
    <span style="color:#ff6666">ONCE → OFF — the end chapter: afterwards sound and LEDs go off</span>
  </div>
</body>
</html>"""


def _pop_flag(args, name):
    """Remove '--name VALUE' from args and return VALUE (or None)."""
    if name in args:
        i = args.index(name)
        val = args[i + 1] if i + 1 < len(args) else None
        del args[i:i + 2]
        return val
    return None


if __name__ == "__main__":
    args = sys.argv[1:]
    svg_path = _pop_flag(args, "--svg")
    png_path = _pop_flag(args, "--png")
    config_path = args[0] if len(args) > 0 else "config.json"
    output_path = args[1] if len(args) > 1 else "experience_map.html"
    auto_open   = len(args) < 2  # auto-open if no output path given

    if not os.path.exists(config_path):
        print(f"ERROR: {config_path} not found")
        sys.exit(1)

    print(f"Reading {config_path}...")
    config = load(config_path)

    print(f"Generating {output_path}...")
    svg  = generate(config)
    html = build_html(svg, config)

    with open(output_path, "w") as f:
        f.write(html)

    if svg_path:
        with open(svg_path, "w") as f:
            f.write(svg)
        print(f"Written: {svg_path}")
    if png_path:
        try:
            import cairosvg
            cairosvg.svg2png(bytestring=svg.encode("utf-8"), write_to=png_path, scale=2)
            print(f"Written: {png_path}")
        except ImportError:
            print("PNG skipped: pip install cairosvg")

    print(f"Written: {output_path}")
    chs  = len(config.get("chapters",[]))
    tags = len(config.get("tags",[]))
    print(f"  {chs} chapters, {tags} tags")

    if auto_open:
        url = f"file://{os.path.abspath(output_path)}"
        print(f"Opening {url}")
        webbrowser.open(url)
        webbrowser.open(url)