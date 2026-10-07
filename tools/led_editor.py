#!/usr/bin/env python3
"""
led_editor.py — LED ring animation editor
  • Frame-by-frame bitmap editor for custom animations
  • Parametric generators (chase, wave, breathe, rotate, wipe, gradient, sparkle)
  • Live JavaScript preview of all 14 built-in C++ animations
  • Frame operations: rotate, mirror, shift, reverse

Usage:
    python3 tools/led_editor.py                      # ./led_animations.json
    python3 tools/led_editor.py /Volumes/SDCARD/     # SD card root
"""

import json, sys, webbrowser, threading
from pathlib import Path
from http.server import HTTPServer, BaseHTTPRequestHandler
from urllib.parse import urlparse

BASE      = Path(sys.argv[1]) if len(sys.argv) > 1 else Path('.')
ANIM_PATH = (BASE if BASE.is_dir() else BASE.parent) / 'led_animations.json'
BUILTIN   = ['breathe','solid','sparkle','fire','ocean','rainbow',
             'pulse_ring','spin','flash','confetti','comet','off',
             'level','heartbeat']

def load_anims():
    if not ANIM_PATH.exists():
        return {"version":1,"animations":[]}
    try:    return json.loads(ANIM_PATH.read_text())
    except: return {"version":1,"animations":[]}

def save_anims(data):
    ANIM_PATH.write_text(json.dumps(data, indent=2))

# ─────────────────────────────────────────────────────────────
HTML = r"""<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<title>LED Animation Editor</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
:root{
  --bg:#0d0d1a;--panel:#13132a;--card:#1a1a35;--border:#252545;
  --accent:#5566ff;--green:#44ff88;--red:#ff4466;--text:#dde;--muted:#778;
  --font:'JetBrains Mono',Consolas,monospace}
body{background:var(--bg);color:var(--text);font-family:var(--font);
     font-size:13px;display:flex;flex-direction:column;height:100vh;overflow:hidden}
header{background:var(--panel);border-bottom:1px solid var(--border);
       padding:8px 14px;display:flex;align-items:center;gap:10px;flex-shrink:0}
header h1{font-size:13px;font-weight:600;color:#eef}
.spacer{flex:1}
.saved{color:var(--green);font-size:11px;opacity:0;transition:opacity .4s}
.saved.show{opacity:1}
.btn{background:var(--accent);color:#fff;border:none;padding:5px 12px;border-radius:4px;
     cursor:pointer;font-family:var(--font);font-size:12px;font-weight:600}
.btn:hover{filter:brightness(1.15)}
.btn.sm{padding:3px 8px;font-size:11px}
.btn.sec{background:transparent;border:1px solid var(--border);color:var(--muted)}
.btn.sec:hover{border-color:var(--accent);color:var(--accent)}
.btn.danger{background:#c03}
.btn:disabled{opacity:.4;cursor:default;filter:none}

.layout{display:flex;flex:1;overflow:hidden}

/* Sidebar */
.sidebar{width:210px;min-width:210px;border-right:1px solid var(--border);
         display:flex;flex-direction:column;background:var(--panel)}
.sh{padding:8px 12px 4px;font-size:10px;color:var(--muted);text-transform:uppercase;letter-spacing:.07em}
.anim-list{flex:1;overflow-y:auto;padding:4px 6px}
.ai{padding:6px 9px;border-radius:4px;cursor:pointer;font-size:11px;margin-bottom:1px;
    display:flex;align-items:center;gap:6px}
.ai:hover{background:var(--card)}
.ai.active{background:var(--accent);color:#fff}
.badge{font-size:8px;padding:1px 5px;border-radius:8px;flex-shrink:0}
.bb{background:#1a2a1a;color:#4a9f4a}
.bc{background:#1a1a4a;color:#6688ff}
.sf{padding:8px;border-top:1px solid var(--border)}

/* Main */
.main{flex:1;display:flex;flex-direction:column;overflow:hidden}
.rings-area{flex:1;display:flex;align-items:center;justify-content:center;
            gap:32px;padding:16px;min-height:0}
.rp{display:flex;flex-direction:column;align-items:center;gap:8px}
.rl{font-size:10px;color:var(--muted);text-transform:uppercase;letter-spacing:.07em}
.ring-svg{cursor:crosshair}
.lc{transition:filter .1s}
.lc:hover{filter:brightness(1.6)}

/* Builtin preview */
.bp-wrap{display:flex;flex-direction:column;align-items:center;gap:16px;padding:20px}
.bp-canvases{display:flex;gap:24px}
.bp-label{font-size:10px;color:var(--muted);text-align:center;margin-top:4px}
.bp-controls{display:flex;flex-direction:column;gap:8px;align-items:center}
.bp-notice{font-size:11px;color:var(--muted);text-align:center;line-height:1.6}

/* Tools */
.tools{display:flex;align-items:center;gap:7px;padding:7px 12px;
       border-top:1px solid var(--border);border-bottom:1px solid var(--border);
       background:var(--panel);flex-shrink:0;flex-wrap:wrap}
.tsep{width:1px;height:18px;background:var(--border)}
.tlabel{font-size:10px;color:var(--muted)}
.color-sw{width:26px;height:26px;border-radius:4px;border:1px solid #444;cursor:pointer}
input[type=color]{width:0;height:0;opacity:0;position:absolute}
#ledInfo{font-size:10px;color:var(--muted)}

/* Timeline */
.timeline{height:86px;border-top:1px solid var(--border);display:flex;align-items:center;
          padding:0 8px;gap:5px;overflow-x:auto;background:var(--panel);flex-shrink:0}
.ft{flex-shrink:0;width:54px;height:70px;border:2px solid var(--border);border-radius:4px;
    cursor:pointer;display:flex;flex-direction:column;align-items:center;
    justify-content:center;gap:2px;position:relative;background:var(--card)}
.ft.active{border-color:var(--accent)}
.ft canvas{border-radius:2px}
.ft .fnum{font-size:8px;color:var(--muted)}
.ft .fms{font-size:8px;color:var(--accent)}
.ft .fdel{position:absolute;top:1px;right:2px;background:none;border:none;
          color:#c03;cursor:pointer;font-size:10px;display:none}
.ft:hover .fdel{display:block}
.af{flex-shrink:0;width:42px;height:70px;border:1px dashed var(--border);
    border-radius:4px;cursor:pointer;background:none;color:var(--muted);
    font-size:20px;display:flex;align-items:center;justify-content:center}
.af:hover{border-color:var(--accent);color:var(--accent)}

/* Right panel */
.props{width:230px;min-width:230px;border-left:1px solid var(--border);
       background:var(--panel);overflow-y:auto;padding:12px}
.pg{margin-bottom:12px}
.pl{font-size:10px;color:var(--muted);text-transform:uppercase;letter-spacing:.07em;
    margin-bottom:4px;display:block}
.pi{width:100%;background:#111122;border:1px solid var(--border);color:var(--text);
    padding:5px 7px;border-radius:4px;font-family:var(--font);font-size:12px}
.pi:focus{outline:none;border-color:var(--accent)}
.row2{display:grid;grid-template-columns:1fr 1fr;gap:6px;margin-top:4px}

/* Generator */
.gen-section{background:var(--card);border-radius:5px;padding:10px;margin-bottom:10px}
.gen-title{font-size:11px;font-weight:600;color:#eef;margin-bottom:8px}
.gen-row{display:flex;gap:5px;align-items:center;margin-bottom:5px;font-size:11px}
.gen-row label{color:var(--muted);width:70px;flex-shrink:0}
.gen-row input,.gen-row select{flex:1;background:#111122;border:1px solid var(--border);
  color:var(--text);padding:3px 5px;border-radius:3px;font-family:var(--font);font-size:11px}

/* Frame ops */
.ops{display:flex;flex-direction:column;gap:4px;margin-top:4px}

/* Modal */
.modal-bg{position:fixed;inset:0;background:#0007;z-index:100;display:none;
          align-items:center;justify-content:center}
.modal-bg.show{display:flex}
.modal{background:var(--panel);border:1px solid var(--border);border-radius:8px;
       padding:20px;min-width:340px;max-height:80vh;overflow-y:auto}
.modal h2{font-size:13px;color:#eef;margin-bottom:14px}
.mclose{float:right;background:none;border:none;color:var(--muted);
        cursor:pointer;font-size:16px;margin-top:-2px}

/* Range */
.slrow{display:flex;gap:6px;align-items:center}
.slrow input[type=range]{flex:1;accent-color:var(--accent)}
.slval{font-size:11px;color:var(--accent);min-width:30px;text-align:right}
</style>
</head>
<body>

<header>
  <h1>⬡ LED Ring Editor</h1>
  <span style="font-size:10px;color:var(--muted)" id="animPath"></span>
  <div class="spacer"></div>
  <span class="saved" id="savedMsg">✓ Saved</span>
  <button class="btn sec" onclick="duplicateAnim()">⧉ Duplicate</button>
  <button class="btn danger sm" id="btnDelete" onclick="deleteAnim()" style="display:none">Remove</button>
  <button class="btn" onclick="saveAnims()">Save</button>
</header>

<div class="layout">
<!-- Sidebar -->
<div class="sidebar">
  <div class="sh">Animations</div>
  <div class="anim-list" id="animList"></div>
  <div class="sf"><button class="btn" style="width:100%" onclick="newAnim()">+ New animation</button></div>
</div>

<!-- Main -->
<div class="main">
  <!-- Rings / builtin preview -->
  <div class="rings-area" id="ringsArea">
    <!-- Custom editor rings -->
    <div id="editorRings" style="display:none;align-items:center;gap:32px">
      <div class="rp"><div class="rl" id="labelNfc">NFC Ring · 24 LEDs</div>
        <svg id="svgNfc" class="ring-svg"></svg></div>
      <div class="rp"><div class="rl" id="labelBeat">Beat Ring · 16 LEDs</div>
        <svg id="svgBeat" class="ring-svg"></svg></div>
    </div>
    <!-- Builtin preview -->
    <div id="builtinPreview" style="display:none">
      <div class="bp-wrap">
        <div class="bp-canvases">
          <div class="rp"><canvas id="bpNfc" width="200" height="200"></canvas>
            <div class="bp-label">NFC ring (24 LEDs)</div></div>
          <div class="rp"><canvas id="bpBeat" width="160" height="160"></canvas>
            <div class="bp-label">Beat ring (16 LEDs)</div></div>
        </div>
        <div class="bp-controls">
          <div class="slrow" style="width:260px">
            <span class="tlabel">Color</span>
            <input type="color" id="bpColor" value="#0044ff" style="width:28px;height:28px;padding:0;border:none;cursor:pointer;position:static"
                   oninput="bpPreviewColor=this.value">
            <span class="tlabel" style="margin-left:8px">Intensity</span>
            <input type="range" min="0.1" max="1" step="0.05" value="0.7" id="bpIntensity"
                   oninput="bpPreviewIntensity=parseFloat(this.value);this.nextElementSibling.textContent=this.value">
            <span class="slval">0.70</span>
          </div>
          <button class="btn sec" onclick="bakeBuiltinToFrames()"
            style="margin-top:4px">Bake to editable frames →</button>
        </div>
        <div class="bp-notice" id="bpName"></div>
      </div>
    </div>
    <!-- Empty state -->
    <div id="emptyState" style="color:var(--muted);text-align:center">
      Select an animation from the list<br>or create a new one
    </div>
  </div>

  <!-- Tools (only for custom) -->
  <div class="tools" id="toolsBar" style="display:none">
    <span class="tlabel">Color:</span>
    <div class="color-sw" id="colorSwatch" style="background:#ff2200"
         onclick="document.getElementById('colorPicker').click()"></div>
    <input type="color" id="colorPicker" value="#ff2200"
           oninput="currentColor=this.value;document.getElementById('colorSwatch').style.background=this.value">
    <div class="tsep"></div>
    <button class="btn sec sm" onclick="paintAll('nfc')">Fill NFC</button>
    <button class="btn sec sm" onclick="paintAll('beat')">Fill Beat</button>
    <button class="btn sec sm" onclick="paintAll('all')">Fill All</button>
    <button class="btn sec sm" onclick="clearFrame()">Clear</button>
    <div class="tsep"></div>
    <button class="btn sec sm" onclick="copyFromPrev()">← Copy prev</button>
    <div class="tsep"></div>
    <button class="btn sec sm" onclick="rotateFrame(1)">↻ Rotate+1</button>
    <button class="btn sec sm" onclick="rotateFrame(-1)">↺ Rotate-1</button>
    <button class="btn sec sm" onclick="mirrorFrame()">⟺ Mirror</button>
    <div class="tsep"></div>
    <button class="btn sm" id="btnPlayTools" onclick="togglePlay()">Play</button>
    <input type="range" min="0.25" max="4" step="0.25" value="1" title="Playback speed" style="width:70px;accent-color:var(--accent)" oninput="playSpeed=parseFloat(this.value);document.getElementById('spdLbl').textContent=this.value+'x'">
    <span id="spdLbl" style="font-size:10px;color:var(--accent);min-width:28px">1x</span>
    <div class="spacer"></div>
    <span id="ledInfo"></span>
  </div>

  <!-- Timeline -->
  <div class="timeline" id="timeline" style="display:none">
    <button class="af" onclick="addFrame()">+</button>
  </div>
</div>

<!-- Right props panel -->
<div class="props" id="propsPanel">
  <div style="color:var(--muted);font-size:11px;margin-top:20px;text-align:center">
    Select an animation
  </div>
</div>
</div>

<!-- Generator modal -->
<div class="modal-bg" id="genModal">
  <div class="modal">
    <button class="mclose" onclick="closeGen()">✕</button>
    <h2>Generate Frames</h2>
    <div class="pg">
      <span class="pl">Algorithm</span>
      <select class="pi" id="genAlgo" onchange="renderGenParams()">
        <option value="chase">Chase / Comet</option>
        <option value="breathe">Breathe / Pulse</option>
        <option value="wave">Wave / Ripple</option>
        <option value="rotate">Rotate (from current frame)</option>
        <option value="wipe">Wipe In</option>
        <option value="gradient">Colour Gradient Arc</option>
        <option value="sparkle">Sparkle Burst</option>
        <option value="strobe">Strobe Flash</option>
      </select>
    </div>
    <div id="genParams"></div>
    <div style="display:flex;gap:8px;margin-top:14px">
      <button class="btn sec" style="flex:1" onclick="closeGen()">Cancel</button>
      <button class="btn" style="flex:1" onclick="runGenerator()">Generate</button>
    </div>
  </div>
</div>

<script>
// ── Constants ─────────────────────────────────────────────────
const BUILTIN=['breathe','solid','sparkle','fire','ocean','rainbow',
               'pulse_ring','spin','flash','confetti','comet','off',
               'level','heartbeat'];
const NFC_N=24,NFC_R=95,NFC_CX=110,NFC_CY=110,NFC_LR=9;
const BEAT_N=16,BEAT_R=65,BEAT_CX=80,BEAT_CY=80,BEAT_LR=8;

let data={version:1,animations:[]};
let currentId=null,currentFrame=0,currentColor='#ff2200';
let playInterval=null;
let bpPhase=0,bpStep=0,bpRaf=null;
let playSpeed=1.0;  // playback speed multiplier
let bakeFrames=24,bakeFps=25; // bake controls
let bpPreviewColor='#0044ff',bpPreviewIntensity=0.7;

// ── API ───────────────────────────────────────────────────────
async function loadAnims(){
  const r=await fetch('/api/anims'); data=await r.json();
  document.getElementById('animPath').textContent=await(await fetch('/api/path')).text();
  renderSidebar();
}
async function saveAnims(){
  await fetch('/api/anims',{method:'POST',headers:{'Content-Type':'application/json'},
                             body:JSON.stringify(data,null,2)});
  const el=document.getElementById('savedMsg');
  el.classList.add('show'); setTimeout(()=>el.classList.remove('show'),2000);
}

// ── Sidebar ───────────────────────────────────────────────────
function renderSidebar(){
  let h='';
  BUILTIN.forEach(n=>{
    h+=`<div class="ai${currentId===n?' active':''}" onclick="selectAnim('${n}')">
      <span class="badge bb">C++</span>${n}</div>`;
  });
  if(data.animations.length){
    h+=`<div style="font-size:9px;color:var(--muted);padding:7px 4px 3px;
        text-transform:uppercase;letter-spacing:.07em">Custom</div>`;
    data.animations.forEach(a=>{
      const r={nfc:'24',beat:'16',both:'40'}[a.ring||'nfc'];
      h+=`<div class="ai${currentId===a.id?' active':''}" onclick="selectAnim('${a.id}')">
        <span class="badge bc">${r}</span>${a.id||'?'}</div>`;
    });
  }
  document.getElementById('animList').innerHTML=h;
}

// ── Select ────────────────────────────────────────────────────
function selectAnim(id){
  stopPlay(); stopBuiltinPreview();
  currentId=id; currentFrame=0;
  renderSidebar();
  const isB=BUILTIN.includes(id);
  show('editorRings',!isB);
  show('builtinPreview',isB);
  show('emptyState',false);
  show('toolsBar',!isB);
  show('timeline',!isB);
  document.getElementById('btnDelete').style.display=isB?'none':'';

  if(isB){ startBuiltinPreview(id); renderBuiltinProps(id); }
  else   { rebuildSVGs(); renderTimeline(); showFrame(0); renderCustomProps(); }
}

function show(id,v){ document.getElementById(id).style.display=v?'flex':'none'; }
function getAnim(id){ return data.animations.find(a=>a.id===id)||null; }
function getAnimIdx(id){ return data.animations.findIndex(a=>a.id===id); }
function ledCount(ring){ return ring==='beat'?16:ring==='both'?40:24; }

// ── New / Delete / Duplicate ──────────────────────────────────
function newAnim(){
  const id='anim_'+Date.now().toString(36);
  const n=24;
  data.animations.push({id,label:'New Animation',ring:'nfc',loop:true,
    frames:[{ms:100,pixels:Array(n).fill('#000000')}]});
  renderSidebar(); selectAnim(id);
}
function deleteAnim(){
  if(!confirm('Remove "'+currentId+'"?')) return;
  data.animations.splice(getAnimIdx(currentId),1);
  currentId=null; renderSidebar();
  show('editorRings',false); show('builtinPreview',false); show('emptyState',true);
  show('toolsBar',false); show('timeline',false);
  document.getElementById('propsPanel').innerHTML='<div style="color:var(--muted);font-size:11px;margin-top:20px;text-align:center">Select an animation</div>';
}
function duplicateAnim(){
  const isB=BUILTIN.includes(currentId);
  const src=isB?null:getAnim(currentId);
  const newId=(currentId||'anim')+'_copy';
  const n=24;
  const copy=src
    ?JSON.parse(JSON.stringify(src))
    :{id:newId,label:currentId+' (copy)',ring:'nfc',loop:true,
      frames:[{ms:100,pixels:Array(n).fill('#000000')}]};
  copy.id=newId; copy.label=(copy.label||currentId)+' (copy)';
  data.animations.push(copy); renderSidebar(); selectAnim(newId);
}

// ── Builtin preview ───────────────────────────────────────────
function startBuiltinPreview(name){
  document.getElementById('bpName').textContent=
    'Built-in C++ animation "'+name+'" — previewed in real-time. '+
    'Click "Bake to editable frames" to convert to a frame sequence you can modify.';
  bpPhase=0; bpStep=0;
  function loop(){
    renderBuiltinFrame(name);
    bpPhase+=0.12; bpStep++;
    bpRaf=requestAnimationFrame(loop);
  }
  loop();
}
function stopBuiltinPreview(){ if(bpRaf){ cancelAnimationFrame(bpRaf); bpRaf=null; } }

function renderBuiltinFrame(name){
  const nfc=builtinPixels(name,'nfc',NFC_N,bpPhase,bpStep,bpPreviewColor,bpPreviewIntensity);
  const beat=builtinPixels(name,'beat',BEAT_N,bpPhase,bpStep,bpPreviewColor,bpPreviewIntensity);
  drawCanvasRing('bpNfc', nfc, NFC_N, 90, 100, 100, 8);
  drawCanvasRing('bpBeat',beat,BEAT_N, 65, 80,  80,  7);
}

function drawCanvasRing(canvasId,pixels,n,r,cx,cy,lr){
  const cv=document.getElementById(canvasId); if(!cv) return;
  const ctx=cv.getContext('2d');
  ctx.fillStyle='#0d0d1a'; ctx.fillRect(0,0,cv.width,cv.height);
  for(let i=0;i<n;i++){
    const a=(i/n)*Math.PI*2-Math.PI/2;
    const x=cx+r*Math.cos(a), y=cy+r*Math.sin(a);
    ctx.fillStyle=pixels[i]||'#000';
    ctx.beginPath(); ctx.arc(x,y,lr,0,Math.PI*2); ctx.fill();
    ctx.fillStyle='#333'; ctx.font=`${lr-3}px monospace`;
    ctx.textAlign='center'; ctx.textBaseline='middle';
    ctx.fillText(i, x, y);
  }
}

// ── Builtin animation implementations ─────────────────────────
function builtinPixels(name,ring,n,phase,step,color,intensity){
  const px=Array(n).fill('#000000');
  const c=hexToRgb(color);
  const sc=(v,i)=>rgbToHex(Math.round(v[0]*i),Math.round(v[1]*i),Math.round(v[2]*i));
  const dim=(col,b)=>sc(hexToRgb(col),Math.max(0,Math.min(1,b)));

  switch(name){
    case 'breathe':{
      const b=0.25+0.75*(Math.sin(phase)+1)/2;
      for(let i=0;i<n;i++) px[i]=sc(c,intensity*b);
      break;}
    case 'solid':
      for(let i=0;i<n;i++) px[i]=sc(c,intensity);
      break;
    case 'sparkle':{
      for(let i=0;i<n;i++) px[i]=sc(c,intensity*0.1);
      const cnt=Math.max(2,Math.round(intensity*4));
      for(let k=0;k<cnt;k++) px[Math.floor(Math.random()*n)]='#ffffff';
      break;}
    case 'fire':{
      // Simplified heat ring
      if(!window._fire||window._fire.length!==n) window._fire=Array(n).fill(0);
      const h=window._fire;
      for(let i=0;i<n;i++) h[i]=Math.max(0,h[i]-8-Math.random()*20);
      for(let k=0;k<3;k++){const p=Math.floor(Math.random()*n);h[p]=Math.min(255,h[p]+100+Math.random()*100);}
      for(let i=0;i<n;i++) h[i]=(h[i]+(h[(i+1)%n]+h[(i+n-1)%n])/2)/2;
      for(let i=0;i<n;i++){
        const v=Math.floor(h[i]*intensity);
        px[i]=rgbToHex(Math.min(255,v),Math.min(255,Math.floor(v*v/255/3)),0);}
      break;}
    case 'ocean':{
      for(let i=0;i<n;i++){
        const wave=(Math.sin(phase+i*0.7)+1)/2;
        const b=intensity*(0.25+wave*0.75);
        px[i]=rgbToHex(0,Math.floor(wave*b*160),Math.floor(b*200));}
      break;}
    case 'rainbow':{
      for(let i=0;i<n;i++){
        const hue=((phase*40/360+i/n)%1)*360;
        px[i]=hslToHex(hue,0.85,0.5*intensity);}
      break;}
    case 'pulse_ring':{
      const ring2=Math.floor(step/5)%4;
      const starts=[0,1,7,19],sizes=[1,6,12,18];
      for(let r=0;r<=Math.min(ring2,3);r++){
        const b=(r+1)/(ring2+1);
        for(let i=0;i<sizes[r];i++) px[starts[r]+i%(n)]=sc(c,intensity*b);}
      break;}
    case 'spin':{
      for(let i=0;i<n;i++){const old=hexToRgb(px[i]);px[i]=rgbToHex(Math.floor(old[0]*.7),Math.floor(old[1]*.7),Math.floor(old[2]*.7));}
      const head=step%n;
      for(let t=0;t<6;t++) px[(head+n-t)%n]=sc(c,intensity*(6-t)/6);
      break;}
    case 'flash':{
      const f=step%24;
      const on=(f<3)||(f>=6&&f<9)||(f>=12&&f<15);
      for(let i=0;i<n;i++) px[i]=sc(c,intensity*(on?1:0.08));
      break;}
    case 'confetti':{
      for(let i=0;i<n;i++) px[i]=dim(px[i],0.78);
      const cnt=Math.max(1,Math.round(intensity*3));
      for(let k=0;k<cnt;k++){
        const hue=Math.random()*360;
        px[Math.floor(Math.random()*n)]=hslToHex(hue,0.85,0.5);}
      break;}
    case 'comet':{
      for(let i=0;i<n;i++){const old=hexToRgb(px[i]);px[i]=rgbToHex(Math.floor(old[0]*.75),Math.floor(old[1]*.75),Math.floor(old[2]*.75));}
      const pos=step%n;
      px[pos]=sc(c,intensity);
      px[(pos+n-1)%n]=sc(c,intensity*0.5);
      px[(pos+n-2)%n]=sc(c,intensity*0.2);
      break;}
    case 'heartbeat':{
      const t=(phase%(Math.PI*2))/(Math.PI*2);
      let b;
      if(t<0.06)      b=t/0.06;
      else if(t<0.14) b=1-(t-0.06)/0.08*0.4;
      else if(t<0.20) b=0.6+(t-0.14)/0.06*0.4;
      else if(t<0.30) b=1-(t-0.20)/0.10*0.8;
      else            b=0.2*(1-(t-0.30)/0.70);
      for(let i=0;i<n;i++) px[i]=sc(c,intensity*Math.max(b,0.04));
      break;}
    case 'level':{
      const lv=Math.abs(Math.sin(phase))*intensity;
      for(let i=0;i<n;i++) px[i]=sc(c,Math.max(lv,0.04));
      break;}
    default:
      break;
  }
  return px;
}

function bakeBuiltinToFrames(){
  const srcName=currentId;
  const newId=srcName+'_baked_'+Date.now().toString(36).slice(-4);
  const ring=document.getElementById('bakeRing')?.value||'nfc';
  const n=ledCount(ring==='both'?'both':ring);
  const nfcN=ring!=='beat'?24:0;
  const beatN=ring!=='nfc'?16:0;
  const ms=Math.round(1000/bakeFrames);  // note: bakeFrames here is actually fps
  const frames=[];
  for(let fi=0;fi<bakeFrames;fi++){
    const phase=fi/bakeFrames*Math.PI*2;
    const pxNfc=nfcN?builtinPixels(srcName,'nfc',nfcN,phase,fi,bpPreviewColor,bpPreviewIntensity):[];
    const pxBeat=beatN?builtinPixels(srcName,'beat',beatN,phase,fi,bpPreviewColor,bpPreviewIntensity):[];
    frames.push({ms:Math.round(1000/bakeFps), pixels:[...pxNfc,...pxBeat]});
  }
  data.animations.push({id:newId,label:srcName+' (baked)',ring,loop:true,frames});
  renderSidebar(); selectAnim(newId);
}

// ── Colour helpers ────────────────────────────────────────────
function hexToRgb(h){
  h=h.replace('#','');
  return [parseInt(h.slice(0,2),16),parseInt(h.slice(2,4),16),parseInt(h.slice(4,6),16)];
}
function rgbToHex(r,g,b){
  return '#'+[r,g,b].map(v=>Math.max(0,Math.min(255,v||0)).toString(16).padStart(2,'0')).join('');
}
function hslToHex(h,s,l){
  const a=s*Math.min(l,1-l);
  const f=n=>{const k=(n+h/30)%12;return l-a*Math.max(-1,Math.min(k-3,9-k,1));};
  return rgbToHex(Math.round(f(0)*255),Math.round(f(8)*255),Math.round(f(4)*255));
}
function dimColor(col,b){const rgb=hexToRgb(col);return rgbToHex(Math.round(rgb[0]*b),Math.round(rgb[1]*b),Math.round(rgb[2]*b));}

// ── SVG rings (custom editor) ─────────────────────────────────
function ledPos(i,n,cx,cy,r){
  const a=(i/n)*Math.PI*2-Math.PI/2;
  return{x:cx+r*Math.cos(a),y:cy+r*Math.sin(a)};
}
function rebuildSVGs(){
  const anim=getAnim(currentId); if(!anim) return;
  const ring=anim.ring||'nfc';
  buildSVG('svgNfc', NFC_N, NFC_R, NFC_CX, NFC_CY, NFC_LR, ring!=='beat', 0);
  buildSVG('svgBeat',BEAT_N,BEAT_R,BEAT_CX,BEAT_CY,BEAT_LR, ring!=='nfc', ring==='both'?24:0);
}
function buildSVG(id,n,r,cx,cy,lr,active,offset){
  const svg=document.getElementById(id);
  const W=cx*2,H=cy*2;
  svg.setAttribute('width',W); svg.setAttribute('height',H);
  svg.setAttribute('viewBox',`0 0 ${W} ${H}`);
  let h=`<circle cx="${cx}" cy="${cy}" r="${r+lr+4}" fill="none" stroke="#1a1a35" stroke-width="2"/>`;
  for(let i=0;i<n;i++){
    const{x,y}=ledPos(i,n,cx,cy,r);
    h+=`<circle class="lc" id="led_${id}_${i}" cx="${x.toFixed(1)}" cy="${y.toFixed(1)}" r="${lr}"
      fill="#111" stroke="#333" stroke-width="1" onclick="paintLed(${offset+i})"
      onmouseenter="document.getElementById('ledInfo').textContent='LED ${i}  '+(getFrame()?.pixels[${offset+i}]||'#000')"/>`;
    h+=`<text x="${x.toFixed(1)}" y="${(y+3.5).toFixed(1)}" text-anchor="middle"
      fill="#333" font-size="7" pointer-events="none">${i}</text>`;
  }
  if(!active){
    h+=`<rect x="0" y="0" width="${W}" height="${H}" fill="#0d0d1a" opacity=".65" rx="8"/>
        <text x="${cx}" y="${cy+4}" text-anchor="middle" fill="#445" font-size="11">not used</text>`;
  }
  svg.innerHTML=h;
}
function getFrame(){
  const a=getAnim(currentId);
  return a&&a.frames[currentFrame]||null;
}
function updateLedVisual(gi,color){
  const anim=getAnim(currentId); if(!anim) return;
  const ring=anim.ring||'nfc';
  let svgId,li;
  if(ring==='both'){ svgId=gi<24?'svgNfc':'svgBeat'; li=gi<24?gi:gi-24; }
  else if(ring==='nfc'){ svgId='svgNfc'; li=gi; }
  else { svgId='svgBeat'; li=gi; }
  const el=document.getElementById(`led_${svgId}_${li}`);
  if(el){ el.setAttribute('fill',color); el.setAttribute('stroke',color==='#000000'?'#333':'#555'); }
}
function paintLed(gi){
  const f=getFrame(); if(!f) return;
  f.pixels[gi]=currentColor;
  updateLedVisual(gi,currentColor);
  updateThumb(currentFrame);
}
function paintAll(which){
  const anim=getAnim(currentId); if(!anim) return;
  const f=getFrame(); if(!f) return;
  const ring=anim.ring||'nfc';
  const total=ledCount(ring);
  const nfc=ring==='beat'?0:24;
  for(let i=0;i<total;i++){
    if(which==='all'||(which==='nfc'&&i<nfc)||(which==='beat'&&i>=nfc)){
      f.pixels[i]=currentColor; updateLedVisual(i,currentColor);
    }
  }
  updateThumb(currentFrame);
}
function clearFrame(){
  const anim=getAnim(currentId); if(!anim) return;
  const f=getFrame(); if(!f) return;
  const n=ledCount(anim.ring||'nfc');
  f.pixels=Array(n).fill('#000000');
  showFrame(currentFrame);
}
function copyFromPrev(){
  const anim=getAnim(currentId);
  if(!anim||currentFrame===0) return;
  anim.frames[currentFrame].pixels=[...anim.frames[currentFrame-1].pixels];
  showFrame(currentFrame); updateThumb(currentFrame);
}

// ── Frame ops ─────────────────────────────────────────────────
function rotateFrame(by){
  const f=getFrame(); if(!f) return;
  const n=f.pixels.length;
  const p=[...f.pixels];
  for(let i=0;i<n;i++) f.pixels[(i+by+n)%n]=p[i];
  showFrame(currentFrame); updateThumb(currentFrame);
}
function mirrorFrame(){
  const f=getFrame(); if(!f) return;
  f.pixels=[...f.pixels].reverse();
  showFrame(currentFrame); updateThumb(currentFrame);
}

// ── Show frame ────────────────────────────────────────────────
function showFrame(idx){
  const anim=getAnim(currentId); if(!anim||!anim.frames.length) return;
  currentFrame=Math.min(idx,anim.frames.length-1);
  const f=anim.frames[currentFrame];
  const total=ledCount(anim.ring||'nfc');
  for(let i=0;i<total;i++) updateLedVisual(i,f.pixels[i]||'#000000');
  const msEl=document.getElementById('propFrameMs'); if(msEl) msEl.value=f.ms||100;
  const tl=document.getElementById('timeline');
  if(tl) tl.querySelectorAll('.ft').forEach((el,fi)=>el.classList.toggle('active',fi===currentFrame));
}

// ── Timeline ──────────────────────────────────────────────────
function renderTimeline(){
  const anim=getAnim(currentId);
  const tl=document.getElementById('timeline');
  if(!anim){tl.innerHTML='<button class="af" onclick="addFrame()">+</button>';return;}
  let h='';
  anim.frames.forEach((f,fi)=>{
    h+=`<div class="ft${fi===currentFrame?' active':''}" onclick="showFrame(${fi})">
      <button class="fdel" onclick="event.stopPropagation();deleteFrame(${fi})">✕</button>
      <canvas id="thumb_${fi}" width="44" height="44"></canvas>
      <span class="fnum">F${fi+1}</span>
      <span class="fms">${f.ms||100}ms</span></div>`;
  });
  h+='<button class="af" onclick="addFrame()">+</button>';
  tl.innerHTML=h;
  anim.frames.forEach((_,fi)=>drawThumb(fi,anim));
}
function drawThumb(fi,anim){
  const cv=document.getElementById(`thumb_${fi}`); if(!cv) return;
  const ctx=cv.getContext('2d');
  const W=cv.width,H=cv.height;
  ctx.fillStyle='#0d0d1a'; ctx.fillRect(0,0,W,H);
  const f=anim.frames[fi];
  const ring=anim.ring||'nfc';
  const nfcN=ring!=='beat'?24:0, beatN=ring!=='nfc'?16:0;
  if(nfcN) for(let i=0;i<nfcN;i++){
    const a=(i/nfcN)*Math.PI*2-Math.PI/2;
    const x=W/2+15*Math.cos(a),y=H/2+15*Math.sin(a);
    ctx.fillStyle=f.pixels[i]||'#000'; ctx.beginPath(); ctx.arc(x,y,3,0,Math.PI*2); ctx.fill();
  }
  if(beatN) for(let i=0;i<beatN;i++){
    const a=(i/beatN)*Math.PI*2-Math.PI/2;
    const rr=ring==='both'?8:15;
    const x=W/2+rr*Math.cos(a),y=H/2+rr*Math.sin(a);
    ctx.fillStyle=f.pixels[nfcN+i]||'#000';
    ctx.beginPath(); ctx.arc(x,y,ring==='both'?2:3,0,Math.PI*2); ctx.fill();
  }
}
function updateThumb(fi){ const a=getAnim(currentId); if(a) drawThumb(fi,a); }

function addFrame(){
  const anim=getAnim(currentId); if(!anim) return;
  const n=ledCount(anim.ring||'nfc');
  const prev=anim.frames[anim.frames.length-1];
  anim.frames.push({ms:prev?prev.ms:100,
    pixels:prev?[...prev.pixels]:Array(n).fill('#000000')});
  renderTimeline(); showFrame(anim.frames.length-1);
}
function deleteFrame(fi){
  const anim=getAnim(currentId);
  if(!anim||anim.frames.length<=1) return;
  anim.frames.splice(fi,1);
  renderTimeline(); showFrame(Math.min(currentFrame,anim.frames.length-1));
}
function duplicateFrame(){
  const anim=getAnim(currentId); if(!anim) return;
  const copy=JSON.parse(JSON.stringify(anim.frames[currentFrame]));
  anim.frames.splice(currentFrame+1,0,copy);
  renderTimeline(); showFrame(currentFrame+1);
}

// ── Props panels ──────────────────────────────────────────────
function renderCustomProps(){
  const anim=getAnim(currentId); if(!anim) return;
  document.getElementById('propsPanel').innerHTML=`
  <div class="pg">
    <span class="pl">Animation ID</span>
    <input class="pi" id="propId" value="${esc(anim.id||'')}"
      oninput="updateAnimProp('id',this.value)">
    <span class="pl" style="margin-top:7px">Label</span>
    <input class="pi" id="propLabel" value="${esc(anim.label||'')}"
      oninput="updateAnimProp('label',this.value)">
    <span class="pl" style="margin-top:7px">Ring target</span>
    <select class="pi" id="propRing" onchange="updateAnimProp('ring',this.value);rebuildSVGs()">
      <option value="nfc"${(anim.ring||'nfc')==='nfc'?' selected':''}>NFC ring (24 LEDs)</option>
      <option value="beat"${anim.ring==='beat'?' selected':''}>Beat ring (16 LEDs)</option>
      <option value="both"${anim.ring==='both'?' selected':''}>Both rings (40 LEDs)</option>
    </select>
    <span class="pl" style="margin-top:7px">Loop</span>
    <select class="pi" onchange="updateAnimProp('loop',this.value==='true')">
      <option value="true"${anim.loop!==false?' selected':''}>Loop forever</option>
      <option value="false"${anim.loop===false?' selected':''}>Play once</option>
    </select>
  </div>

  <div class="gen-section">
    <div class="gen-title">Frame duration</div>
    <input class="pi" type="number" id="propFrameMs" min="10" max="5000" step="10" value="100"
      oninput="updateFrameProp('ms',parseInt(this.value)||100)">
    <div class="ops" style="margin-top:8px">
      <button class="btn sec" onclick="duplicateFrame()">⧉ Duplicate frame</button>
    </div>
  </div>

  <div class="gen-section">
    <div class="gen-title">Generate frames</div>
    <p style="font-size:10px;color:var(--muted);margin-bottom:8px;line-height:1.5">
      Auto-generate frame sequences from mathematical patterns — much faster than painting manually.
    </p>
    <button class="btn" style="width:100%" onclick="openGen()">⚙ Open generator…</button>
  </div>

  <div class="gen-section">
    <div class="gen-title">Playback</div>
    <div style="display:flex;gap:6px;margin-bottom:8px">
      <button class="btn" style="flex:1" id="btnPlay" onclick="togglePlay()">▶ Play</button>
    </div>
    <div class="gen-row" style="margin-bottom:0"><label>Speed</label>
      <div class="slrow" style="flex:1">
        <input type="range" min="0.25" max="4" step="0.25" value="${playSpeed}"
          oninput="playSpeed=parseFloat(this.value);this.nextElementSibling.textContent=this.value+'×'">
        <span class="slval">${playSpeed}×</span>
      </div>
    </div>
    <p style="font-size:9px;color:var(--muted);margin-top:4px;line-height:1.4">
      Speed scales frame durations during playback only — stored ms values unchanged.
    </p>
  </div>

  <div style="margin-top:12px;font-size:10px;color:var(--muted);line-height:1.6">
    Use in config.json:<br>
    <span style="color:var(--accent)">"animation": "${esc(anim.id||'')}"</span>
  </div>`;
}

function renderBuiltinProps(name){
  document.getElementById('propsPanel').innerHTML=`
  <div class="pg">
    <span class="pl">Built-in C++: ${name}</span>
    <p style="font-size:10px;color:var(--green);line-height:1.5;margin-top:4px;
       background:#0a2a0a;padding:6px;border-radius:4px;border:1px solid #1a4a1a">
      ✓ Already parameterised in config.json — no baking needed.<br>
      Just set color, intensity, speed in your chapter's led_background or led_beat.
    </p>
    <div style="margin-top:6px;font-size:10px;color:var(--muted);background:#111122;
         padding:7px;border-radius:4px;line-height:1.7">
      "animation": "${name}",<br>
      "color": "0x${bpPreviewColor.slice(1).toUpperCase()}",<br>
      "intensity": ${bpPreviewIntensity.toFixed(2)},<br>
      "speed": 0.40
    </div>
  </div>
  <div class="gen-section">
    <div class="gen-title">Live preview parameters</div>
    <div class="gen-row"><label>Color</label>
      <input type="color" value="${bpPreviewColor}" style="height:26px;padding:0;cursor:pointer;border:none;background:none"
        oninput="bpPreviewColor=this.value;renderBuiltinProps('${name}')"></div>
    <div class="gen-row"><label>Intensity</label>
      <div class="slrow"><input type="range" min="0.05" max="1" step="0.05" value="${bpPreviewIntensity}"
        oninput="bpPreviewIntensity=parseFloat(this.value);renderBuiltinProps('${name}')">
        <span class="slval">${bpPreviewIntensity.toFixed(2)}</span></div></div>
  </div>
  <div class="gen-section">
    <div class="gen-title">Bake to editable frames</div>
    <p style="font-size:10px;color:var(--muted);line-height:1.5;margin-bottom:8px">
      Only needed if you want to modify individual pixels. Otherwise use the animation
      name directly in config.json — it stays live and parameterised.
    </p>
    <div class="gen-row"><label>Ring</label>
      <select id="bakeRing" style="flex:1;background:#111122;border:1px solid var(--border);color:var(--text);font-family:var(--font);font-size:11px;padding:3px">
        <option value="nfc">NFC (24 LEDs)</option>
        <option value="beat">Beat (16 LEDs)</option>
        <option value="both">Both (40 LEDs)</option>
      </select></div>
    <div class="gen-row"><label>Frames</label>
      <input type="number" min="4" max="120" value="${bakeFrames}"
        oninput="bakeFrames=parseInt(this.value)||24"
        style="flex:1;background:#111122;border:1px solid var(--border);color:var(--text);font-family:var(--font);font-size:11px;padding:3px"></div>
    <div class="gen-row"><label>FPS</label>
      <input type="number" min="1" max="60" value="${bakeFps}"
        oninput="bakeFps=parseInt(this.value)||25"
        style="flex:1;background:#111122;border:1px solid var(--border);color:var(--text);font-family:var(--font);font-size:11px;padding:3px"></div>
    <button class="btn" style="width:100%;margin-top:8px" onclick="bakeBuiltinToFrames()">Bake →</button>
  </div>`;
}

function updateAnimProp(key,val){
  const idx=getAnimIdx(currentId); if(idx<0) return;
  data.animations[idx][key]=val;
  if(key==='id'){ currentId=val; renderSidebar(); renderCustomProps(); }
}
function updateFrameProp(key,val){
  const f=getFrame(); if(!f) return;
  f[key]=val;
  const thumb=document.querySelectorAll('.ft')[currentFrame];
  if(thumb&&key==='ms'){const ms=thumb.querySelector('.fms');if(ms)ms.textContent=val+'ms';}
}
function esc(s){return String(s||'').replace(/"/g,'&quot;').replace(/</g,'&lt;');}

// ── Playback ──────────────────────────────────────────────────
function togglePlay(){
  if(playInterval) stopPlay(); else startPlay();
}
function startPlay(){
  const anim=getAnim(currentId); if(!anim||!anim.frames.length) return;
  syncPlayBtn();
  let fi=0;
  function tick(){
    showFrame(fi);
    const ms=Math.max(10, Math.round((anim.frames[fi].ms||100) / playSpeed));
    fi=(fi+1)%anim.frames.length;
    playInterval=setTimeout(tick,ms);
  }
  tick();
}
function syncPlayBtn(){const lbl=playInterval?'Stop':'Play';['btnPlay','btnPlayTools'].forEach(id=>{const b=document.getElementById(id);if(b)b.textContent=lbl;});}
function stopPlay(){
  if(playInterval){clearTimeout(playInterval);playInterval=null;}
  syncPlayBtn();
}

// ── Generator ─────────────────────────────────────────────────
const GEN_PARAMS={
  chase:[
    {id:'frames',label:'Frames',type:'number',default:24,min:4,max:64},
    {id:'ms',label:'ms per frame',type:'number',default:50,min:10,max:500},
    {id:'tail',label:'Tail length',type:'number',default:3,min:1,max:10},
    {id:'color',label:'Color',type:'color',default:'#ff4400'}
  ],
  breathe:[
    {id:'frames',label:'Frames per cycle',type:'number',default:20,min:4,max:60},
    {id:'ms',label:'ms per frame',type:'number',default:80,min:10,max:500},
    {id:'minB',label:'Min brightness',type:'range',default:0.08,min:0,max:1,step:0.01},
    {id:'color',label:'Color',type:'color',default:'#0044ff'}
  ],
  wave:[
    {id:'frames',label:'Frames',type:'number',default:16,min:4,max:64},
    {id:'ms',label:'ms per frame',type:'number',default:60,min:10,max:500},
    {id:'waves',label:'Wave count',type:'number',default:2,min:1,max:6},
    {id:'color',label:'Base color',type:'color',default:'#0066aa'},
    {id:'color2',label:'Peak color',type:'color',default:'#00ccff'}
  ],
  rotate:[
    {id:'steps',label:'Steps per frame',type:'number',default:1,min:1,max:6},
    {id:'copies',label:'Total frames',type:'number',default:24,min:2,max:64},
    {id:'ms',label:'ms per frame',type:'number',default:50,min:10,max:500}
  ],
  wipe:[
    {id:'ms',label:'ms per frame',type:'number',default:40,min:10,max:500},
    {id:'color',label:'Color',type:'color',default:'#ffffff'},
    {id:'dir',label:'Direction',type:'select',options:['Forward','Backward','Both'],default:'Forward'}
  ],
  gradient:[
    {id:'color',label:'Start color',type:'color',default:'#ff0000'},
    {id:'color2',label:'End color',type:'color',default:'#0000ff'},
    {id:'ms',label:'ms per frame',type:'number',default:100,min:10,max:500},
    {id:'rotate',label:'Rotation frames',type:'number',default:0,min:0,max:48}
  ],
  sparkle:[
    {id:'frames',label:'Frames',type:'number',default:16,min:4,max:64},
    {id:'ms',label:'ms per frame',type:'number',default:60,min:10,max:500},
    {id:'count',label:'Sparks per frame',type:'number',default:3,min:1,max:10},
    {id:'color',label:'Color',type:'color',default:'#ffffff'},
    {id:'bg',label:'Background',type:'color',default:'#000000'}
  ],
  strobe:[
    {id:'count',label:'Flash count',type:'number',default:3,min:1,max:8},
    {id:'onMs',label:'On time (ms)',type:'number',default:40,min:10,max:500},
    {id:'offMs',label:'Off time (ms)',type:'number',default:60,min:10,max:500},
    {id:'color',label:'Color',type:'color',default:'#ffffff'},
    {id:'tail',label:'Tail frames (0=none)',type:'number',default:0,min:0,max:8}
  ]
};

let genState={};
function openGen(){ document.getElementById('genModal').classList.add('show'); renderGenParams(); }
function closeGen(){ document.getElementById('genModal').classList.remove('show'); }

function renderGenParams(){
  const algo=document.getElementById('genAlgo').value;
  const params=GEN_PARAMS[algo]||[];
  let h='';
  params.forEach(p=>{
    const val=genState[p.id]??p.default;
    h+=`<div class="gen-row"><label>${p.label}</label>`;
    if(p.type==='number')
      h+=`<input type="number" value="${val}" min="${p.min||0}" max="${p.max||9999}"
            oninput="genState['${p.id}']=parseFloat(this.value)">`;
    else if(p.type==='color')
      h+=`<input type="color" value="${val}" style="height:26px;padding:0;border:none;cursor:pointer"
            oninput="genState['${p.id}']=this.value">`;
    else if(p.type==='range')
      h+=`<div class="slrow" style="flex:1">
            <input type="range" min="${p.min}" max="${p.max}" step="${p.step}" value="${val}"
              oninput="genState['${p.id}']=parseFloat(this.value);this.nextElementSibling.textContent=parseFloat(this.value).toFixed(2)">
            <span class="slval">${parseFloat(val).toFixed(2)}</span></div>`;
    else if(p.type==='select')
      h+=`<select oninput="genState['${p.id}']=this.value">${p.options.map(o=>
            `<option${o===val?' selected':''}>${o}</option>`).join('')}</select>`;
    h+='</div>';
  });
  document.getElementById('genParams').innerHTML=h;
  // init genState defaults
  params.forEach(p=>{ if(genState[p.id]==null) genState[p.id]=p.default; });
}

function runGenerator(){
  const anim=getAnim(currentId); if(!anim) return;
  const n=ledCount(anim.ring||'nfc');
  const algo=document.getElementById('genAlgo').value;
  const p=genState;
  let frames=[];

  if(algo==='chase'){
    const fc=Math.round(p.frames)||24;
    for(let fi=0;fi<fc;fi++){
      const px=Array(n).fill('#000000');
      const head=fi%n;
      for(let t=0;t<(p.tail||3);t++){
        const idx=(head-t+n)%n;
        px[idx]=dimColor(p.color||'#ff4400',(p.tail-t)/p.tail);
      }
      frames.push({ms:Math.round(p.ms)||50,pixels:px});
    }
  } else if(algo==='breathe'){
    const fc=Math.round(p.frames)||20;
    for(let fi=0;fi<fc;fi++){
      const phase=fi/fc*Math.PI*2;
      const b=(p.minB||0.08)+(1-(p.minB||0.08))*((Math.sin(phase)+1)/2);
      frames.push({ms:Math.round(p.ms)||80,
        pixels:Array(n).fill(dimColor(p.color||'#0044ff',b))});
    }
  } else if(algo==='wave'){
    const fc=Math.round(p.frames)||16;
    const wc=Math.round(p.waves)||2;
    for(let fi=0;fi<fc;fi++){
      const px=[];
      for(let i=0;i<n;i++){
        const wave=(Math.sin(fi/fc*Math.PI*2+i/n*Math.PI*2*wc)+1)/2;
        const c1=hexToRgb(p.color||'#0066aa');
        const c2=hexToRgb(p.color2||'#00ccff');
        px.push(rgbToHex(
          Math.round(c1[0]+(c2[0]-c1[0])*wave),
          Math.round(c1[1]+(c2[1]-c1[1])*wave),
          Math.round(c1[2]+(c2[2]-c1[2])*wave)));
      }
      frames.push({ms:Math.round(p.ms)||60,pixels:px});
    }
  } else if(algo==='rotate'){
    const src=getFrame();
    if(!src){ alert('Paint a source frame first, then rotate.'); return; }
    const steps=Math.round(p.steps)||1;
    const copies=Math.round(p.copies)||n;
    let cur=[...src.pixels];
    for(let fi=0;fi<copies;fi++){
      frames.push({ms:Math.round(p.ms)||50,pixels:[...cur]});
      const next=Array(n);
      for(let i=0;i<n;i++) next[(i+steps)%n]=cur[i];
      cur=next;
    }
  } else if(algo==='wipe'){
    const dir=p.dir||'Forward';
    const seq=dir==='Backward'?Array.from({length:n},(_,i)=>n-1-i):Array.from({length:n},(_,i)=>i);
    const px=Array(n).fill('#000000');
    seq.forEach(i=>{
      px[i]=p.color||'#ffffff';
      frames.push({ms:Math.round(p.ms)||40,pixels:[...px]});
    });
    if(dir==='Both'){
      const px2=[...px];
      seq.forEach(i=>{
        px2[i]='#000000';
        frames.push({ms:Math.round(p.ms)||40,pixels:[...px2]});
      });
    }
  } else if(algo==='gradient'){
    const c1=hexToRgb(p.color||'#ff0000');
    const c2=hexToRgb(p.color2||'#0000ff');
    const rot=Math.round(p.rotate)||0;
    const px=Array.from({length:n},(_,i)=>rgbToHex(
      Math.round(c1[0]+(c2[0]-c1[0])*i/n),
      Math.round(c1[1]+(c2[1]-c1[1])*i/n),
      Math.round(c1[2]+(c2[2]-c1[2])*i/n)));
    if(rot===0){ frames.push({ms:Math.round(p.ms)||100,pixels:px}); }
    else{
      let cur=[...px];
      for(let fi=0;fi<rot;fi++){
        frames.push({ms:Math.round(p.ms)||100,pixels:[...cur]});
        const next=Array(n);
        for(let i=0;i<n;i++) next[(i+1)%n]=cur[i];
        cur=next;
      }
    }
  } else if(algo==='sparkle'){
    const fc=Math.round(p.frames)||16;
    for(let fi=0;fi<fc;fi++){
      const px=Array(n).fill(p.bg||'#000000');
      const cnt=Math.round(p.count)||3;
      for(let k=0;k<cnt;k++){
        const i=Math.floor(Math.random()*n);
        px[i]=p.color||'#ffffff';
      }
      frames.push({ms:Math.round(p.ms)||60,pixels:px});
    }
  } else if(algo==='strobe'){
    const cnt=Math.round(p.count)||3;
    for(let k=0;k<cnt;k++){
      frames.push({ms:Math.round(p.onMs)||40, pixels:Array(n).fill(p.color||'#ffffff')});
      frames.push({ms:Math.round(p.offMs)||60,pixels:Array(n).fill('#000000')});
    }
    const tail=Math.round(p.tail)||0;
    if(tail>0){
      for(let t=0;t<tail;t++){
        const b=(tail-t)/tail;
        frames.push({ms:Math.round(p.offMs)||60,
          pixels:Array(n).fill(dimColor(p.color||'#ffffff',b*0.3))});
      }
    }
  }

  if(!frames.length){ alert('No frames generated.'); return; }
  anim.frames=frames;
  closeGen();
  renderTimeline(); showFrame(0);
  alert(`Generated ${frames.length} frames.`);
}

window.onload=loadAnims;
</script>
</body>
</html>"""

class Handler(BaseHTTPRequestHandler):
    def log_message(self,*a): pass
    def do_GET(self):
        p=urlparse(self.path).path
        if p=='/':            self.respond(200,'text/html',HTML.encode())
        elif p=='/api/anims': self.respond(200,'application/json',json.dumps(load_anims()).encode())
        elif p=='/api/path':  self.respond(200,'text/plain',str(ANIM_PATH.resolve()).encode())
        else:                 self.respond(404,'text/plain',b'Not found')
    def do_POST(self):
        p=urlparse(self.path).path
        n=int(self.headers.get('Content-Length',0))
        body=self.rfile.read(n)
        if p=='/api/anims':
            try: save_anims(json.loads(body)); self.respond(200,'application/json',b'{"ok":true}')
            except Exception as e: self.respond(500,'application/json',json.dumps({"error":str(e)}).encode())
        else: self.respond(404,'text/plain',b'Not found')
    def respond(self,code,ctype,body):
        self.send_response(code)
        self.send_header('Content-Type',ctype)
        self.send_header('Content-Length',len(body))
        self.send_header('Access-Control-Allow-Origin','*')
        self.end_headers()
        self.wfile.write(body)

if __name__=='__main__':
    port=8081
    server=HTTPServer(('localhost',port),Handler)
    url=f'http://localhost:{port}'
    print(f"LED Animation Editor\n  Animations: {ANIM_PATH.resolve()}\n  URL: {url}\n  Stop: Ctrl+C")
    threading.Timer(0.5,lambda:webbrowser.open(url)).start()
    try: server.serve_forever()
    except KeyboardInterrupt: print("\nStopped.")