#pragma once

// Single page served from flash. Colours follow the house palette
// (white canvas, #ff6200 accent, thin lines) but the page does not pull in
// the full lui library: 65 KB of CSS+JS per load is a poor trade for one
// image, one audio element and a meter on a microcontroller.

static const char INDEX_HTML[] PROGMEM = R"HTML(<!doctype html>
<html lang="en"><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Baby Monitor</title>
<style>
:root{--bg:#fff;--ink:#141414;--ink-soft:#6b6b6b;--line:#e4e4e4;--accent:#ff6200;--r:12px}
@media (prefers-color-scheme:dark){:root:not([data-theme=light]){--bg:#141414;--ink:#f2f2f2;--ink-soft:#9a9a9a;--line:#2c2c2c}}
*{box-sizing:border-box}
body{margin:0;padding:16px;background:var(--bg);color:var(--ink);
     font:15px/1.5 system-ui,-apple-system,Segoe UI,Roboto,sans-serif}
.wrap{max-width:640px;margin:0 auto}
h1{font-size:18px;font-weight:600;margin:0 0 14px}
.card{border:1px solid var(--line);border-radius:var(--r);padding:14px;margin-bottom:14px}
img{width:100%;display:block;border-radius:8px;background:var(--line);aspect-ratio:4/3;object-fit:cover}
audio{width:100%;margin-top:4px}
.row{display:flex;align-items:center;gap:10px}
.meter{flex:1;height:10px;border-radius:5px;background:var(--line);overflow:hidden}
.meter i{display:block;height:100%;width:0;background:var(--accent);transition:none}
.muted{color:var(--ink-soft);font-size:13px}
button{font:inherit;color:var(--ink);background:none;border:1px solid var(--line);
       border-radius:8px;padding:7px 13px;cursor:pointer}
button.on{border-color:var(--accent);color:var(--accent)}
input[type=range]{flex:1;accent-color:var(--accent)}
</style></head><body><div class="wrap">

<h1>Baby Monitor</h1>

<div class="card">
  <div class="row" style="margin-bottom:10px">
    <div class="meter"><i id="bar"></i></div>
    <span class="muted" id="lvl">--</span>
  </div>
  <audio id="au" controls autoplay></audio>
  <div class="row" style="margin-top:8px">
    <button id="listen">Listen</button>
    <span class="muted" id="astat">idle</span>
  </div>
  <div class="row" style="margin-top:10px">
    <span class="muted">Gain</span>
    <input type="range" id="gain" min="1" max="24" value="2">
    <span class="muted" id="gv">2</span>
  </div>
</div>

<div class="card">
  <img id="cam" alt="camera off">
  <div class="row" style="margin-top:10px">
    <button id="vid">Show video</button>
    <button id="lampBtn">Lamp</button>
    <input type="range" id="lamp" min="0" max="255" value="0" title="lamp brightness">
  </div>
</div>

<p class="muted" id="stat">connecting&hellip;</p>

<script>
const H = location.hostname;
const au = document.getElementById('au');
const cam = document.getElementById('cam');
const vid = document.getElementById('vid');

// Audio runs on its own port so a stalled MJPEG connection can never block it.
const astat = document.getElementById('astat');
function startAudio(){
  au.src = 'http://' + H + ':82/audio?t=' + Date.now();
  // Browsers block autoplay until the page has been interacted with. The old
  // code swallowed that rejection, so a blocked stream was indistinguishable
  // from a broken one: the connection opened, bytes flowed, nothing played.
  au.play().then(() => astat.textContent = 'playing')
           .catch(e => astat.textContent = 'blocked: press Listen (' + e.name + ')');
}
au.addEventListener('error', () => { astat.textContent = 'stream error, retrying'; setTimeout(startAudio, 1500); });
au.addEventListener('ended', () => setTimeout(startAudio, 500));
au.addEventListener('playing', () => astat.textContent = 'playing');
au.addEventListener('stalled', () => astat.textContent = 'stalled');
au.addEventListener('waiting', () => astat.textContent = 'buffering');
document.getElementById('listen').onclick = () => { au.muted = false; au.volume = 1; startAudio(); };
startAudio();

let on = false;
vid.onclick = () => {
  on = !on;
  vid.classList.toggle('on', on);
  vid.textContent = on ? 'Hide video' : 'Show video';
  cam.src = on ? 'http://' + H + ':81/stream' : '';
  if(!on) cam.removeAttribute('src');
};

// The white LED is a camera flash, so it is dimmable and starts at zero.
// Full brightness has its use (a quick look in a dark room) but must be a
// deliberate act, never the default.
const lamp = document.getElementById('lamp'), lampBtn = document.getElementById('lampBtn');
let lampLast = 40;
function setLamp(v){
  v = Math.max(0, Math.min(255, v|0));
  lamp.value = v;
  lampBtn.classList.toggle('on', v > 0);
  fetch('/set?lamp=' + v);
}
lamp.oninput = () => { if(+lamp.value > 0) lampLast = +lamp.value; setLamp(+lamp.value); };
lampBtn.onclick = () => setLamp(+lamp.value > 0 ? 0 : lampLast);

// An <audio> element has no notion of "live": it plays everything it received,
// in order, and never drops anything. One network hiccup that later arrives as
// a burst becomes permanent delay. So measure the gap between what has been
// downloaded and what is being played, and jump forward when it grows.
function lag(){
  try{
    if (!au.buffered || au.buffered.length === 0) return 0;
    return Math.max(0, au.buffered.end(au.buffered.length - 1) - au.currentTime);
  }catch(e){ return 0; }
}
const MAX_LAG = 1.5;          // seconds of drift tolerated before catching up
function catchUp(){
  const d = lag();
  if (d > MAX_LAG) {
    try{
      // Leave a fraction of a second of headroom: landing exactly on the edge
      // of the buffer makes the element stall immediately.
      au.currentTime = au.buffered.end(au.buffered.length - 1) - 0.3;
      astat.textContent = 'caught up (' + d.toFixed(1) + ' s)';
    }catch(e){ /* seeking is not always permitted; the next reload resets it */ }
  }
}
setInterval(catchUp, 2000);

let gSynced = false;
const bar = document.getElementById('bar'), lvl = document.getElementById('lvl'),
      stat = document.getElementById('stat');
async function poll(){
  try{
    const r = await fetch('/level', {cache:'no-store'});
    const j = await r.json();
    // Adopt the board's value once, instead of asserting whatever the HTML
    // happened to ship with. Otherwise the slider and the device disagree
    // from the first second and every later reading is a guess.
    if (!gSynced) { gSynced = true; g.value = j.gain; gv.textContent = j.gain; }
    // Perceptual curve: a linear RMS bar sits near zero for ordinary room noise.
    const p = Math.min(100, Math.round(Math.pow(j.level, 0.4) * 100));
    bar.style.width = p + '%';
    lvl.textContent = p + '%';
    stat.textContent = 'up ' + j.uptime + ' s · ' + j.rssi + ' dBm · ' +
                       (j.heap/1024|0) + ' kB free \u00b7 lag ' + lag().toFixed(1) + ' s';
  }catch(e){ stat.textContent = 'no connection'; }
}
// Once a second, not four times. The meter stays perfectly readable, and the
// board keeps far more socket headroom for the streams that actually matter.
setInterval(poll, 1000); poll();

const g = document.getElementById('gain'), gv = document.getElementById('gv');
g.oninput = () => { gv.textContent = g.value; gSynced = true; fetch('/set?gain=' + g.value); };
</script>
</div></body></html>)HTML";
