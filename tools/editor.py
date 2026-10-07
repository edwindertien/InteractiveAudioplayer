#!/usr/bin/env python3
"""
editor.py — Interactive Audio Player config editor
Serves a web UI on http://localhost:8080 for editing config.json.

Usage:
    python3 tools/editor.py                    # opens config.json in current dir
    python3 tools/editor.py /path/to/config.json
"""

import json, sys, os, re, webbrowser, threading
from pathlib import Path
from http.server import HTTPServer, BaseHTTPRequestHandler
from urllib.parse import urlparse

# ── Config path ───────────────────────────────────────────────
CONFIG_PATH = Path(sys.argv[1]) if len(sys.argv) > 1 else Path("config.json")

def load_config():
    if not CONFIG_PATH.exists():
        return {}
    raw = CONFIG_PATH.read_text()
    raw = re.sub(r'"_[^"]*"\s*:\s*"[^"]*"\s*,?\s*', '', raw)
    raw = re.sub(r',\s*([}\]])', r'\1', raw)
    try:
        return json.loads(raw)
    except:
        return json.loads(CONFIG_PATH.read_text())

def save_config(data):
    CONFIG_PATH.write_text(json.dumps(data, indent=2))

# ── SVG timeline (embedded from visualize.py logic) ───────────

LED_PALETTES = {
    "breathe": lambda c: hex_rgb(c),
    "fire":    lambda c: (220, 80, 10),
    "ocean":   lambda c: (0, 100, 200),
    "rainbow": lambda c: (180, 80, 200),
    "sparkle": lambda c: hex_rgb(c),
    "comet":   lambda c: hex_rgb(c),
    "pulse_ring": lambda c: hex_rgb(c),
    "spin":    lambda c: hex_rgb(c),
    "flash":   lambda c: hex_rgb(c),
    "confetti":lambda c: (180, 80, 200),
    "solid":   lambda c: hex_rgb(c),
    "off":     lambda c: (15, 15, 15),
    "level":   lambda c: hex_rgb(c),
    "heartbeat": lambda c: hex_rgb(c),
}

def hex_rgb(h):
    h = str(h).replace("0x","").replace("#","").zfill(6)
    return (int(h[0:2],16), int(h[2:4],16), int(h[4:6],16))

def led_css(ch):
    bg = ch.get("led_background", {})
    fn = LED_PALETTES.get(bg.get("animation","solid"), lambda c: hex_rgb(c))
    r,g,b = fn(bg.get("color","0x333333"))
    i = float(bg.get("intensity", 0.6))
    return f"rgb({int(r*i)},{int(g*i)},{int(b*i)})"

def lum(css):
    m = re.match(r'rgb\((\d+),(\d+),(\d+)\)', css)
    if not m: return 0
    return 0.299*int(m[1]) + 0.587*int(m[2]) + 0.114*int(m[3])

def build_svg(config):
    chapters = config.get("chapters", [])
    tags = config.get("tags", [])
    total_ms = max((c.get("loop_end_ms", 0) for c in chapters), default=96000) or 96000
    W, ML, MR = 860, 90, 16
    TW = W - ML - MR
    def x(ms): return ML + (ms / total_ms) * TW

    TRACK_H, OV_H, OV_GAP = 50, 12, 2
    overlay_rows = 3
    timeline_h = TRACK_H + overlay_rows * (OV_H + OV_GAP) + 8
    flow_h = max(len(tags) * 26 + 32, 40)
    total_h = 48 + timeline_h + 28 + flow_h + 16

    s = []
    s.append(f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{total_h}" '
             f'style="font-family:monospace;background:#0d0d1a;border-radius:6px;">')
    s.append(f'<text x="{W-8}" y="14" fill="#333" font-size="8" text-anchor="end">'
             f'chapters may overlap · order in JSON is cosmetic</text>')

    # Tick marks
    y0 = 36
    t = 0
    while t <= total_ms:
        px = x(t)
        s.append(f'<line x1="{px:.0f}" y1="{y0}" x2="{px:.0f}" y2="{y0+timeline_h-4}" '
                 f'stroke="#252535" stroke-width="1"/>')
        s.append(f'<text x="{px:.0f}" y="{y0-4}" fill="#444" font-size="9" '
                 f'text-anchor="middle">{t//1000}s</text>')
        t += 10000

    # Chapter blocks
    ov_colors = {"ov1":"#4a9eff","ov2":"#ff9f4a","narr":"#9fff4a"}
    ch_cx = {}
    for ci, ch in enumerate(chapters):
        x1 = x(ch.get("start_ms",0))
        x2 = x(ch.get("loop_end_ms", total_ms))
        w = max(x2 - x1, 2)
        bg = led_css(ch)
        tc = "#eee" if lum(bg) < 100 else "#111"
        loop = ch.get("loop", True)
        anim = ch.get("led_background",{}).get("animation","?")

        s.append(f'<rect x="{x1:.0f}" y="{y0}" width="{w:.0f}" height="{TRACK_H}" '
                 f'fill="{bg}" rx="3" stroke="#111" stroke-width="1.5" opacity="0.88"/>')
        label = ch.get("label", ch.get("id","?"))
        s.append(f'<text x="{x1+w/2:.0f}" y="{y0+16}" fill="{tc}" font-size="9.5" '
                 f'font-weight="bold" text-anchor="middle">{label[:22]}</text>')
        s.append(f'<text x="{x1+w/2:.0f}" y="{y0+28}" fill="{tc}" font-size="8" '
                 f'text-anchor="middle" opacity=".7">'
                 f'{ch.get("start_ms",0)//1000}s–{ch.get("loop_end_ms",0)//1000}s</text>')
        lc = "#88ff88" if loop else "#ff8888"
        lt = "LOOP" if loop else "ONCE→IDLE"
        s.append(f'<text x="{x1+w/2:.0f}" y="{y0+40}" fill="{lc}" font-size="7.5" '
                 f'text-anchor="middle">{lt}  {anim}</text>')

        ovs = ch.get("overlays", {})
        mode = ch.get("overlay_mode","fresh")
        for i, (ov, oc) in enumerate(ov_colors.items()):
            oy = y0 + TRACK_H + 4 + i*(OV_H+OV_GAP)
            on = ovs.get(ov, False)
            fill = oc if on else ("#334" if mode=="keep" else "#1a1a2e")
            s.append(f'<rect x="{x1:.0f}" y="{oy}" width="{w:.0f}" height="{OV_H}" '
                     f'fill="{fill}" rx="2"/>')

        ch_cx[ch.get("id","")] = (x1 + x2) / 2

    # Overlay labels
    for i, (ov, oc) in enumerate(ov_colors.items()):
        oy = y0 + TRACK_H + 4 + i*(OV_H+OV_GAP) + OV_H//2 + 3
        s.append(f'<text x="{ML-4}" y="{oy}" fill="{oc}" font-size="8" text-anchor="end">{ov}</text>')
    s.append(f'<text x="{ML-4}" y="{y0+26}" fill="#666" font-size="8" text-anchor="end">chapter</text>')

    # Tag flow
    y_flow = y0 + timeline_h + 26
    s.append(f'<text x="8" y="{y_flow-4}" fill="#555" font-size="9">TAG NAVIGATION</text>')
    s.append(f'<line x1="0" y1="{y_flow-14}" x2="{W}" y2="{y_flow-14}" stroke="#252535"/>')
    s.append('<defs><marker id="arr" markerWidth="5" markerHeight="5" refX="4" refY="2.5" orient="auto">'
             '<path d="M0,0 L5,2.5 L0,5 Z" fill="#88ccff" opacity=".7"/></marker></defs>')

    for ti, tag in enumerate(tags):
        ty = y_flow + 14 + ti*26
        uid = tag.get("uid","?")
        ttype = tag.get("type","location")
        tc2 = "#ff88ff" if ttype=="device" else "#88ccff"
        s.append(f'<rect x="0" y="{ty-10}" width="{ML-6}" height="14" fill="#1a1a2e" rx="2" '
                 f'stroke="{tc2}" stroke-width=".7"/>')
        s.append(f'<text x="3" y="{ty+1}" fill="{tc2}" font-size="7.5">'
                 f'{"◈" if ttype=="device" else "◆"} {uid[:12]}</text>')
        label = tag.get("label","")[:36]
        s.append(f'<text x="{ML}" y="{ty+1}" fill="#777" font-size="8">{label}</text>')
        for act in tag.get("actions",[]):
            if act.get("do")=="chapter" and act.get("target") in ch_cx:
                tx2 = ch_cx[act["target"]]
                s.append(f'<line x1="{ML-2}" y1="{ty-3}" x2="{tx2:.0f}" y2="{y0+TRACK_H}" '
                         f'stroke="{tc2}" stroke-width="1" stroke-dasharray="3,2" '
                         f'opacity=".6" marker-end="url(#arr)"/>')

    s.append('</svg>')
    return "".join(s)

# ── HTML UI ───────────────────────────────────────────────────

HTML = r"""<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<title>Experience Editor</title>
<style>
  * { box-sizing: border-box; margin: 0; padding: 0; }
  :root {
    --bg:     #0d0d1a;
    --panel:  #13132a;
    --card:   #1a1a35;
    --border: #252545;
    --accent: #5566ff;
    --green:  #44ff88;
    --red:    #ff4466;
    --amber:  #ffaa22;
    --text:   #dde;
    --muted:  #778;
    --font:   'JetBrains Mono', 'Fira Code', Consolas, monospace;
  }
  body { background:var(--bg); color:var(--text); font-family:var(--font);
         font-size:13px; display:flex; flex-direction:column; height:100vh; }

  header { background:var(--panel); border-bottom:1px solid var(--border);
            padding:10px 16px; display:flex; align-items:center; gap:12px; flex-shrink:0; }
  header h1 { font-size:14px; font-weight:600; color:#eef; letter-spacing:.04em; }
  header .path { color:var(--muted); font-size:11px; flex:1; }
  .btn { background:var(--accent); color:#fff; border:none; padding:6px 14px;
         border-radius:4px; cursor:pointer; font-family:var(--font); font-size:12px;
         font-weight:600; letter-spacing:.03em; }
  .btn:hover { filter:brightness(1.15); }
  .btn.danger { background:#c03; }
  .btn.secondary { background:transparent; border:1px solid var(--border); color:var(--muted); }
  .btn.secondary:hover { border-color:var(--accent); color:var(--accent); }
  .saved { color:var(--green); font-size:11px; opacity:0; transition:opacity .4s; }
  .saved.show { opacity:1; }

  .layout { display:flex; flex:1; overflow:hidden; }

  /* ── Left: timeline ── */
  .timeline-pane { width:900px; min-width:900px; border-right:1px solid var(--border);
                   padding:16px; overflow-y:auto; background:var(--panel); flex-shrink:0; }
  .timeline-pane h2 { font-size:11px; color:var(--muted); letter-spacing:.08em;
                      text-transform:uppercase; margin-bottom:10px; }

  /* ── Right: editor ── */
  .editor-pane { flex:1; display:flex; flex-direction:column; overflow:hidden; }

  .tabs { display:flex; border-bottom:1px solid var(--border); background:var(--panel); flex-shrink:0; }
  .tab { padding:10px 18px; cursor:pointer; font-size:12px; color:var(--muted);
         border-bottom:2px solid transparent; margin-bottom:-1px; }
  .tab.active { color:#eef; border-bottom-color:var(--accent); }
  .tab:hover { color:#dde; }

  .tab-content { flex:1; overflow-y:auto; padding:16px; display:none; }
  .tab-content.active { display:block; }

  /* ── Cards ── */
  .card { background:var(--card); border:1px solid var(--border); border-radius:6px;
          padding:14px; margin-bottom:10px; }
  .card-header { display:flex; align-items:center; gap:8px; margin-bottom:12px; cursor:pointer; }
  .card-header h3 { font-size:12px; color:#eef; flex:1; }
  .card-body { display:none; }
  .card.open .card-body { display:block; }
  .toggle-icon { color:var(--muted); font-size:10px; transition:.2s; }
  .card.open .toggle-icon { transform:rotate(90deg); }

  .color-dot { width:12px; height:12px; border-radius:3px; flex-shrink:0; border:1px solid #333; }

  /* ── Form fields ── */
  .field { margin-bottom:10px; }
  .field label { display:block; font-size:10px; color:var(--muted); letter-spacing:.06em;
                 text-transform:uppercase; margin-bottom:4px; }
  .field input[type=text], .field input[type=number], .field select {
    width:100%; background:#111122; border:1px solid var(--border); color:var(--text);
    padding:6px 8px; border-radius:4px; font-family:var(--font); font-size:12px; }
  .field input:focus, .field select:focus { outline:none; border-color:var(--accent); }
  .field.row { display:flex; gap:8px; }
  .field.row .field { flex:1; margin-bottom:0; }

  /* Color input with preview */
  .color-field { display:flex; gap:8px; align-items:center; }
  .color-field input[type=text] { flex:1; }
  .color-preview { width:28px; height:28px; border-radius:4px; border:1px solid #333;
                   flex-shrink:0; cursor:pointer; }
  .color-field input[type=color] { width:28px; height:28px; padding:0; border:none;
                                    cursor:pointer; background:none; }

  /* Checkbox / toggle */
  .toggle-row { display:flex; align-items:center; gap:8px; margin-bottom:8px; }
  .toggle-row label { font-size:11px; color:var(--text); cursor:pointer; }
  input[type=checkbox] { accent-color:var(--accent); width:14px; height:14px; cursor:pointer; }

  /* Range slider */
  .range-row { display:flex; align-items:center; gap:10px; }
  .range-row input[type=range] { flex:1; accent-color:var(--accent); }
  .range-val { font-size:11px; color:var(--accent); min-width:32px; text-align:right; }

  /* Section label */
  .section-label { font-size:10px; color:var(--muted); letter-spacing:.08em;
                   text-transform:uppercase; margin:14px 0 6px; border-bottom:1px solid var(--border);
                   padding-bottom:4px; }

  /* Actions */
  .action-row { display:flex; gap:6px; align-items:center; background:#111122;
                border:1px solid var(--border); border-radius:4px; padding:6px 8px;
                margin-bottom:5px; }
  .action-row select { flex:1; background:#111122; border:none; color:var(--text);
                       font-family:var(--font); font-size:11px; }
  .action-row input { flex:1; background:#111122; border:none; color:var(--text);
                      font-family:var(--font); font-size:11px; padding:0 4px; }
  .action-row .rm { background:none; border:none; color:#c03; cursor:pointer;
                    font-size:14px; padding:0 4px; }
  .add-action { background:none; border:1px dashed var(--border); color:var(--muted);
                padding:5px; width:100%; cursor:pointer; border-radius:4px; font-family:var(--font); }
  .add-action:hover { border-color:var(--accent); color:var(--accent); }

  /* Tag type badge */
  .badge { font-size:9px; padding:2px 6px; border-radius:10px; font-weight:600; letter-spacing:.05em; }
  .badge.location { background:#1a2a4a; color:#4a9eff; }
  .badge.device   { background:#2a1a3a; color:#ff88ff; }

  /* Global fields */
  .global-grid { display:grid; grid-template-columns:1fr 1fr; gap:10px; }
</style>
</head>
<body>

<header>
  <h1>⬡ Experience Editor</h1>
  <span class="path" id="cfgPath">config.json</span>
  <span class="saved" id="savedMsg">✓ Saved</span>
  <button class="btn secondary" onclick="reloadFromServer()">↺ Reload</button>
  <button class="btn" onclick="saveConfig()">Save to SD card file</button>
</header>

<div class="layout">
  <!-- Timeline -->
  <div class="timeline-pane">
    <h2>Experience Map</h2>
    <div id="svgContainer">Loading…</div>
  </div>

  <!-- Editor -->
  <div class="editor-pane">
    <div class="tabs">
      <div class="tab active" onclick="switchTab('global')">Global</div>
      <div class="tab" onclick="switchTab('chapters')">Chapters</div>
      <div class="tab" onclick="switchTab('tags')">Tags</div>
    </div>

    <div id="tab-global" class="tab-content active"></div>
    <div id="tab-chapters" class="tab-content"></div>
    <div id="tab-tags" class="tab-content"></div>
  </div>
</div>

<script>
let cfg = {};

// ── API ──────────────────────────────────────────────────────
async function loadConfig() {
  const r = await fetch('/api/config');
  cfg = await r.json();
  renderAll();
}

async function reloadFromServer() {
  await loadConfig();
}

async function saveConfig() {
  const r = await fetch('/api/config', {
    method: 'POST',
    headers: {'Content-Type':'application/json'},
    body: JSON.stringify(cfg, null, 2)
  });
  if (r.ok) {
    const el = document.getElementById('savedMsg');
    el.classList.add('show');
    setTimeout(() => el.classList.remove('show'), 2000);
    refreshSvg();
  }
}

async function refreshSvg() {
  const r = await fetch('/api/svg', {
    method:'POST', headers:{'Content-Type':'application/json'},
    body: JSON.stringify(cfg)
  });
  document.getElementById('svgContainer').innerHTML = await r.text();
}

// ── Render ───────────────────────────────────────────────────
function renderAll() {
  renderGlobal();
  renderChapters();
  renderTags();
  refreshSvg();
}

function switchTab(name) {
  document.querySelectorAll('.tab').forEach((t,i) => {
    const names = ['global','chapters','tags'];
    t.classList.toggle('active', names[i]===name);
  });
  document.querySelectorAll('.tab-content').forEach(el => {
    el.classList.toggle('active', el.id === 'tab-'+name);
  });
}

// ── Colour helpers ────────────────────────────────────────────
function hexToCSS(h) {
  h = String(h).replace('0x','').replace('#','').padStart(6,'0');
  return '#' + h;
}
function cssToHex(css) {
  return '0x' + css.replace('#','').toUpperCase();
}
function ledPreviewCSS(params) {
  const palettes = {
    fire:'rgb(180,55,5)', ocean:'rgb(0,80,160)', rainbow:'rgb(160,60,200)',
    confetti:'rgb(160,60,200)'
  };
  if (palettes[params.animation]) return palettes[params.animation];
  const h = String(params.color||'0x333333').replace('0x','').padStart(6,'0');
  const r=parseInt(h.slice(0,2),16), g=parseInt(h.slice(2,4),16), b=parseInt(h.slice(4,6),16);
  const i = parseFloat(params.intensity||0.7);
  return `rgb(${Math.round(r*i)},${Math.round(g*i)},${Math.round(b*i)})`;
}

// ── Field builders ────────────────────────────────────────────
function textField(label, val, onChange) {
  return `<div class="field">
    <label>${label}</label>
    <input type="text" value="${esc(val)}" oninput="(${onChange})(this.value)">
  </div>`;
}
function numField(label, val, min, max, onChange) {
  return `<div class="field">
    <label>${label}</label>
    <input type="number" value="${val}" min="${min}" max="${max}" step="any"
           oninput="(${onChange})(parseFloat(this.value)||0)">
  </div>`;
}
function selectField(label, val, opts, onChange) {
  const options = opts.map(o => `<option${o===val?' selected':''}>${o}</option>`).join('');
  return `<div class="field"><label>${label}</label>
    <select onchange="(${onChange})(this.value)">${options}</select></div>`;
}
function sliderField(label, val, min, max, step, onChange) {
  const id = 'sl_'+Math.random().toString(36).slice(2);
  return `<div class="field"><label>${label}</label>
    <div class="range-row">
      <input type="range" id="${id}" min="${min}" max="${max}" step="${step}" value="${val}"
             oninput="this.nextElementSibling.textContent=parseFloat(this.value).toFixed(2);(${onChange})(parseFloat(this.value))">
      <span class="range-val">${parseFloat(val).toFixed(2)}</span>
    </div></div>`;
}
function colorField(label, val, onChange) {
  const css = hexToCSS(val||'0x444444');
  return `<div class="field"><label>${label}</label>
    <div class="color-field">
      <input type="text" value="${esc(val||'0x000000')}"
             oninput="this.nextElementSibling.value=hexToCSS(this.value)||'#000';
                      this.nextElementSibling.nextElementSibling.style.background=hexToCSS(this.value)||'#000';
                      (${onChange})(this.value)">
      <input type="color" value="${css}"
             oninput="this.previousElementSibling.value=cssToHex(this.value);
                      this.nextElementSibling.style.background=this.value;
                      (${onChange})(cssToHex(this.value))">
      <div class="color-preview" style="background:${css}"></div>
    </div></div>`;
}
function checkField(label, val, onChange) {
  return `<div class="toggle-row">
    <input type="checkbox" ${val?'checked':''} onchange="(${onChange})(this.checked)">
    <label>${label}</label></div>`;
}
function ledBlock(label, params, pathFn) {
  const previewCSS = ledPreviewCSS(params);
  const anims = ['breathe','solid','sparkle','fire','ocean','rainbow',
                 'pulse_ring','spin','flash','confetti','comet','off',
                 'level','heartbeat'];
  return `<div class="section-label">
    <span style="background:${previewCSS};width:10px;height:10px;border-radius:2px;display:inline-block;vertical-align:middle;margin-right:6px;border:1px solid #333"></span>
    ${label}
  </div>
  ${selectField('Animation', params.animation||'breathe', anims,
    `v=>{${pathFn}.animation=v;refreshSvg()}`)}
  ${colorField('Color', params.color||'0x004400',
    `v=>{${pathFn}.color=v}`)}
  ${sliderField('Intensity', params.intensity||0.7, 0, 1, 0.01,
    `v=>{${pathFn}.intensity=v}`)}
  ${sliderField('Speed / Sensitivity', params.speed||0.4, 0, 1, 0.01,
    `v=>{${pathFn}.speed=v}`)}
  ${params.durationMs !== undefined ? sliderField('Duration (ms)', params.durationMs||800, 0, 5000, 50,
    `v=>{${pathFn}.durationMs=v}`) : ''}`;
}
function esc(s) { return String(s||'').replace(/"/g,'&quot;').replace(/</g,'&lt;'); }

// ── Global tab ────────────────────────────────────────────────
function renderGlobal() {
  const el = document.getElementById('tab-global');
  el.innerHTML = `
  <div class="global-grid">
    ${textField('Device ID', cfg.device_id||'UNIT_01', `v=>{cfg.device_id=v}`)}
    ${textField('Start Chapter', cfg.start_chapter||'intro', `v=>{cfg.start_chapter=v}`)}
  </div>
  <div class="global-grid">
    ${numField('LED Brightness (0-255)', cfg.led_brightness||160, 0, 255,
      `v=>{cfg.led_brightness=Math.round(v)}`)}
    ${numField('NFC Sensitivity (0-5)', cfg.nfc_sensitivity||3, 0, 5,
      `v=>{cfg.nfc_sensitivity=Math.round(v)}`)}
  </div>
  <div class="section-label">Idle LED (shown before first tag tap)</div>
  ${ledBlock('Idle animation', cfg.idle?.led||{animation:'breathe',color:'0x000066',intensity:0.35,speed:0.2},
    `cfg.idle.led`)}`;
  if (!cfg.idle) cfg.idle = {led:{}};
}

// ── Chapters tab ──────────────────────────────────────────────
function renderChapters() {
  const el = document.getElementById('tab-chapters');
  const addBtn = `<button class="btn secondary" style="width:100%;margin-top:8px"
    onclick="addChapter()">+ Add chapter</button>`;
  if (!cfg.chapters?.length) { el.innerHTML = addBtn; return; }
  el.innerHTML = cfg.chapters.map((ch, ci) => chapterCard(ch, ci)).join('') + addBtn;
}

function chapterCard(ch, ci) {
  const bg = ledPreviewCSS(ch.led_background||{});
  const overlays = ch.overlays || {};
  return `
  <div class="card" id="chcard_${ci}">
    <div class="card-header" onclick="toggleCard('chcard_${ci}')">
      <div class="color-dot" style="background:${bg}"></div>
      <h3>${esc(ch.label||ch.id||'Chapter')}</h3>
      <span style="font-size:10px;color:${ch.loop===false?'#ff8888':'#88ff88'}">
        ${ch.loop===false?'ONCE':'LOOP'}
      </span>
      <span style="font-size:10px;color:var(--muted)">${ch.start_ms||0}–${ch.loop_end_ms||0}ms</span>
      <span class="toggle-icon">▶</span>
      <button class="btn secondary" style="padding:2px 7px;font-size:11px"
        onclick="event.stopPropagation();moveChapter(${ci},-1)" ${ci===0?'disabled':''}>↑</button>
      <button class="btn secondary" style="padding:2px 7px;font-size:11px"
        onclick="event.stopPropagation();moveChapter(${ci},1)"
        ${ci===(cfg.chapters.length-1)?'disabled':''}>↓</button>
    </div>
    <div class="card-body">
      <div class="field row">
        ${textField('ID', ch.id||'', `v=>{cfg.chapters[${ci}].id=v;renderChapters()}`)}
        ${textField('Label', ch.label||'', `v=>{cfg.chapters[${ci}].label=v}`)}
      </div>
      <div class="field row">
        ${numField('Start (ms)', ch.start_ms||0, 0, 999999, `v=>{cfg.chapters[${ci}].start_ms=Math.round(v)}`)}
        ${numField('Loop end (ms)', ch.loop_end_ms||0, 0, 999999, `v=>{cfg.chapters[${ci}].loop_end_ms=Math.round(v)}`)}
      </div>
      <div class="field row">
        ${numField('Enter FX slot (-1=none)', ch.on_enter_fx??-1, -1, 7, `v=>{cfg.chapters[${ci}].on_enter_fx=Math.round(v)}`)}
        ${selectField('Overlay mode', ch.overlay_mode||'fresh', ['fresh','keep'],
          `v=>{cfg.chapters[${ci}].overlay_mode=v}`)}
      </div>
      ${checkField('Loop (false = play once → idle)', ch.loop!==false,
        `v=>{cfg.chapters[${ci}].loop=v;renderChapters()}`)}

      <div class="section-label">Overlay defaults (for overlay_mode: fresh)</div>
      ${['ov1','ov2','narr'].map(ov =>
        checkField(ov, overlays[ov]||false, `v=>{cfg.chapters[${ci}].overlays['${ov}']=v}`)
      ).join('')}

      ${ledBlock('Background LED (NFC ring)', ch.led_background||{},
        `cfg.chapters[${ci}].led_background`)}
      ${ledBlock('Enter LED (NFC ring, one-shot)', ch.led_enter||{durationMs:1200},
        `cfg.chapters[${ci}].led_enter`)}
      ${ledBlock('Beat LED (transducer ring)', ch.led_beat||{},
        `cfg.chapters[${ci}].led_beat`)}
      <div style="display:flex;gap:8px;margin-top:14px">
        <button class="btn secondary" style="flex:1"
          onclick="duplicateChapter(${ci})">⧉ Duplicate</button>
        <button class="btn danger" style="flex:1"
          onclick="removeChapter(${ci})">Remove chapter</button>
      </div>
    </div>
  </div>`;
}

// ── Tags tab ──────────────────────────────────────────────────
function renderTags() {
  const el = document.getElementById('tab-tags');
  const addBtn = `<button class="btn secondary" style="width:100%;margin-top:8px"
    onclick="addTag()">+ Add tag</button>`;
  if (!cfg.tags?.length) { el.innerHTML = addBtn; return; }
  el.innerHTML = cfg.tags.map((t,ti) => tagCard(t,ti)).join('') + addBtn;
}

function tagCard(tag, ti) {
  const tc = tag.type==='device' ? '#ff88ff' : '#4a9eff';
  return `
  <div class="card" id="tgcard_${ti}">
    <div class="card-header" onclick="toggleCard('tgcard_${ti}')">
      <span class="badge ${tag.type||'location'}">${tag.type||'location'}</span>
      <h3 style="color:${tc}">${esc(tag.uid||'NEW TAG')}</h3>
      <span style="font-size:10px;color:var(--muted)">${esc(tag.label||'')}</span>
      <span class="toggle-icon">▶</span>
      <button class="btn secondary" style="padding:2px 7px;font-size:11px"
        onclick="event.stopPropagation();moveTag(${ti},-1)" ${ti===0?'disabled':''}>↑</button>
      <button class="btn secondary" style="padding:2px 7px;font-size:11px"
        onclick="event.stopPropagation();moveTag(${ti},1)"
        ${ti===(cfg.tags.length-1)?'disabled':''}>↓</button>
    </div>
    <div class="card-body">
      ${textField('UID (from serial monitor)', tag.uid||'', `v=>{cfg.tags[${ti}].uid=v;const h=document.querySelector('#tgcard_${ti} h3');if(h)h.textContent=v||'NEW TAG';}`)}
      ${textField('Label', tag.label||'', `v=>{cfg.tags[${ti}].label=v}`)}
      ${selectField('Type', tag.type||'location', ['location','device'],
        `v=>{cfg.tags[${ti}].type=v;renderTags();refreshSvg()}`)}
      ${numField('Cooldown (ms, 0=none)', tag.cooldown_ms||0, 0, 30000,
        `v=>{cfg.tags[${ti}].cooldown_ms=Math.round(v)}`)}

      <div class="section-label">Actions</div>
      <div id="actions_${ti}">
        ${(tag.actions||[]).map((a,ai) => actionRow(a, ti, ai)).join('')}
      </div>
      <button class="add-action" onclick="addAction(${ti})">+ Add action</button>
      <button class="btn danger" style="width:100%;margin-top:8px"
        onclick="removeTag(${ti})">Remove tag</button>
    </div>
  </div>`;
}

function actionRow(act, ti, ai) {
  const chIds = (cfg.chapters||[]).map(c=>c.id||'').join(',');
  const doType = act.do||'chapter';

  let extra = '';
  if (doType==='chapter')
    extra = `<select oninput="cfg.tags[${ti}].actions[${ai}].target=this.value;refreshSvg()"
               style="flex:1;background:#111122;border:1px solid var(--border);color:var(--text);font-family:var(--font);font-size:11px;padding:3px">
      ${(cfg.chapters||[]).map(c =>
        `<option${c.id===(act.target||'')?' selected':''}>${esc(c.id||'')}</option>`
      ).join('')}
    </select>`;
  else if (doType==='overlay')
    extra = `<input placeholder="ov1/ov2/narr" value="${esc(act.target||'ov1')}"
               oninput="cfg.tags[${ti}].actions[${ai}].target=this.value;refreshSvg()">
             <select oninput="cfg.tags[${ti}].actions[${ai}].value=this.value">
               ${['toggle','on','off'].map(v=>`<option${v===(act.value||'toggle')?' selected':''}>${v}</option>`).join('')}
             </select>`;
  else if (doType==='fx')
    extra = `<input type="number" min="0" max="7" placeholder="slot" value="${act.slot??0}"
               oninput="cfg.tags[${ti}].actions[${ai}].slot=parseInt(this.value)||0">`;
  else if (doType==='led')
    extra = `<div style="display:flex;flex-direction:column;gap:4px;flex:1">
      <div style="display:flex;gap:4px">
        <select oninput="cfg.tags[${ti}].actions[${ai}].animation=this.value" style="flex:1;background:#111122;border:1px solid var(--border);color:var(--text);font-family:var(--font);font-size:11px;padding:2px">
          ${['sparkle','flash','pulse_ring','comet','spin','confetti','rainbow','breathe','solid','fire','ocean','off'].map(a=>
            `<option${a===(act.animation||'sparkle')?' selected':''}>${a}</option>`).join('')}
        </select>
        <input type="color" value="${hexToCSS(act.color||'0xFFFFAA')}"
          oninput="cfg.tags[${ti}].actions[${ai}].color=cssToHex(this.value)"
          style="width:28px;height:24px;padding:0;border:none;cursor:pointer">
      </div>
      <div style="display:flex;gap:4px;align-items:center;font-size:10px;color:var(--muted)">
        <span>int</span>
        <input type="range" min="0" max="1" step="0.05" value="${act.intensity||1.0}"
          oninput="cfg.tags[${ti}].actions[${ai}].intensity=parseFloat(this.value)"
          style="flex:1;accent-color:var(--accent)">
        <span>spd</span>
        <input type="range" min="0" max="1" step="0.05" value="${act.speed||0.8}"
          oninput="cfg.tags[${ti}].actions[${ai}].speed=parseFloat(this.value)"
          style="flex:1;accent-color:var(--accent)">
        <input type="number" min="0" max="10000" step="100" value="${act.durationMs||1500}"
          oninput="cfg.tags[${ti}].actions[${ai}].durationMs=parseInt(this.value)"
          placeholder="ms" style="width:52px;background:#111122;border:1px solid var(--border);color:var(--text);font-family:var(--font);font-size:10px;padding:2px 4px">
      </div>
    </div>`;

  return `<div class="action-row">
    <select onchange="cfg.tags[${ti}].actions[${ai}].do=this.value;renderTags()">
      ${['chapter','overlay','fx','led'].map(d=>
        `<option${d===doType?' selected':''}>${d}</option>`).join('')}
    </select>
    ${extra}
    <button class="rm" onclick="removeAction(${ti},${ai})">✕</button>
  </div>`;
}

function addChapter() {
  cfg.chapters = cfg.chapters || [];
  const lastCh = cfg.chapters[cfg.chapters.length - 1];
  const newStart = lastCh ? (lastCh.loop_end_ms || 0) : 0;
  const newEnd   = newStart + 32000;
  cfg.chapters.push({
    id:           'chapter_' + (cfg.chapters.length + 1),
    label:        'New Chapter ' + (cfg.chapters.length + 1),
    start_ms:     newStart,
    loop_end_ms:  newEnd,
    on_enter_fx:  -1,
    loop:         true,
    overlay_mode: 'fresh',
    overlays:     { ov1: false, ov2: false, narr: false },
    led_background: { animation: 'breathe', color: '0x0044FF', intensity: 0.6, speed: 0.3 },
    led_enter:      { animation: 'pulse_ring', color: '0x0088FF', intensity: 0.8, speed: 0.6, durationMs: 1200 },
    led_beat:       { animation: 'level', color: '0x0088FF', intensity: 0.6, speed: 0.5 }
  });
  renderChapters();
  refreshSvg();
  // Open the new card
  const idx = cfg.chapters.length - 1;
  const card = document.getElementById('chcard_' + idx);
  if (card) card.classList.add('open');
}

function duplicateChapter(ci) {
  const copy = JSON.parse(JSON.stringify(cfg.chapters[ci]));
  const lastCh = cfg.chapters[cfg.chapters.length - 1];
  copy.id    = copy.id + '_copy';
  copy.label = copy.label + ' (copy)';
  copy.start_ms    = lastCh.loop_end_ms || 0;
  copy.loop_end_ms = copy.start_ms + (cfg.chapters[ci].loop_end_ms - cfg.chapters[ci].start_ms);
  cfg.chapters.push(copy);
  renderChapters();
  refreshSvg();
  const card = document.getElementById('chcard_' + (cfg.chapters.length - 1));
  if (card) card.classList.add('open');
}

function moveChapter(ci, dir) {
  const ni = ci + dir;
  if (ni < 0 || ni >= cfg.chapters.length) return;
  [cfg.chapters[ci], cfg.chapters[ni]] = [cfg.chapters[ni], cfg.chapters[ci]];
  renderChapters();
  refreshSvg();
}

function moveTag(ti, dir) {
  const ni = ti + dir;
  if (ni < 0 || ni >= cfg.tags.length) return;
  [cfg.tags[ti], cfg.tags[ni]] = [cfg.tags[ni], cfg.tags[ti]];
  renderTags();
  refreshSvg();
}

function removeChapter(ci) {
  if (confirm('Remove chapter "' + (cfg.chapters[ci].label || cfg.chapters[ci].id) + '"?')) {
    cfg.chapters.splice(ci, 1);
    renderChapters();
    refreshSvg();
  }
}

function addAction(ti) {
  cfg.tags[ti].actions = cfg.tags[ti].actions || [];
  cfg.tags[ti].actions.push({do:'chapter', target: cfg.chapters?.[0]?.id||''});
  renderTags();
  document.getElementById('tgcard_'+ti).classList.add('open');
}
function removeAction(ti, ai) { cfg.tags[ti].actions.splice(ai,1); renderTags(); }
function addTag() {
  cfg.tags = cfg.tags || [];
  cfg.tags.push({uid:'NEW_UID', label:'New tag', type:'location', cooldown_ms:2000, actions:[]});
  renderTags();
  refreshSvg();
  const idx = cfg.tags.length - 1;
  const card = document.getElementById('tgcard_'+idx);
  if (card) card.classList.add('open');
}
function removeTag(ti) { if(confirm('Remove this tag?')) { cfg.tags.splice(ti,1); renderTags(); refreshSvg(); } }

// ── Utilities ─────────────────────────────────────────────────
function toggleCard(id) {
  document.getElementById(id).classList.toggle('open');
}

window.onload = loadConfig;
</script>
</body>
</html>"""

# ── HTTP handler ──────────────────────────────────────────────

class Handler(BaseHTTPRequestHandler):
    def log_message(self, *a): pass  # suppress request logs

    def do_GET(self):
        path = urlparse(self.path).path
        if path == '/':
            self.respond(200, 'text/html', HTML.encode())
        elif path == '/api/config':
            data = json.dumps(load_config(), indent=2).encode()
            self.respond(200, 'application/json', data)
        else:
            self.respond(404, 'text/plain', b'Not found')

    def do_POST(self):
        path = urlparse(self.path).path
        length = int(self.headers.get('Content-Length', 0))
        body = self.rfile.read(length)

        if path == '/api/config':
            try:
                save_config(json.loads(body))
                self.respond(200, 'application/json', b'{"ok":true}')
            except Exception as e:
                self.respond(500, 'application/json',
                             json.dumps({"error": str(e)}).encode())

        elif path == '/api/svg':
            try:
                config = json.loads(body)
                svg = build_svg(config)
                self.respond(200, 'image/svg+xml', svg.encode())
            except Exception as e:
                self.respond(500, 'text/plain', str(e).encode())
        else:
            self.respond(404, 'text/plain', b'Not found')

    def respond(self, code, ctype, body):
        self.send_response(code)
        self.send_header('Content-Type', ctype)
        self.send_header('Content-Length', len(body))
        self.send_header('Access-Control-Allow-Origin', '*')
        self.end_headers()
        self.wfile.write(body)

# ── Entry point ───────────────────────────────────────────────

if __name__ == '__main__':
    port = 8080
    server = HTTPServer(('localhost', port), Handler)
    url = f'http://localhost:{port}'

    print(f"Experience Editor")
    print(f"  Config: {CONFIG_PATH.resolve()}")
    print(f"  URL:    {url}")
    print(f"  Stop:   Ctrl+C")
    print()

    threading.Timer(0.5, lambda: webbrowser.open(url)).start()
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nStopped.")