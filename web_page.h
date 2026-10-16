// ============================================================================
//  web_page.h — the single-page web UI, served from flash (PROGMEM).
//  It polls GET /api/data (JSON) and renders one card per battery.
//  Control buttons POST to /api/control and only appear if the firmware was
//  built with ENABLE_CONTROL = 1.
// ============================================================================
#pragma once
#include <Arduino.h>

static const char INDEX_HTML[] PROGMEM = R"HTML(<!doctype html>
<html lang="de">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Akku-Monitor</title>
<style>
  :root{color-scheme:dark light}
  *{box-sizing:border-box}
  body{margin:0;font:15px/1.4 system-ui,sans-serif;background:#0f1216;color:#e7edf3}
  header{padding:14px 18px;background:#151a21;border-bottom:1px solid #222c38;
         display:flex;align-items:center;gap:10px;position:sticky;top:0}
  header h1{font-size:16px;margin:0;font-weight:600}
  #age{margin-left:auto;font-size:12px;color:#8b98a8}
  main{max-width:900px;margin:0 auto;padding:16px;display:grid;gap:16px;
       grid-template-columns:repeat(auto-fit,minmax(320px,1fr))}
  .card{background:#151a21;border:1px solid #222c38;border-radius:12px;padding:16px}
  .card h2{margin:0 0 2px;font-size:17px}
  .sub{font-size:12px;color:#8b98a8;margin-bottom:12px}
  .dot{display:inline-block;width:9px;height:9px;border-radius:50%;margin-right:6px;vertical-align:1px}
  .ok{background:#35c26b}.bad{background:#e3534f}
  .soc{height:10px;border-radius:6px;background:#222c38;overflow:hidden;margin:6px 0 14px}
  .soc > i{display:block;height:100%;background:linear-gradient(90deg,#2f9e57,#43c97a)}
  .grid{display:grid;grid-template-columns:1fr 1fr;gap:8px 14px;margin-bottom:12px}
  .kv{display:flex;justify-content:space-between;border-bottom:1px solid #1d2630;padding:3px 0}
  .kv b{font-weight:600}
  .k{color:#8b98a8}
  .cells{display:flex;flex-wrap:wrap;gap:6px;margin:6px 0 10px}
  .cell{background:#1b222c;border:1px solid #2a3542;border-radius:7px;
        padding:4px 8px;font-size:12px;font-variant-numeric:tabular-nums}
  .badges{display:flex;flex-wrap:wrap;gap:6px}
  .badge{font-size:11px;padding:3px 8px;border-radius:20px;background:#1b222c;border:1px solid #2a3542}
  .badge.on{background:#163a26;border-color:#235b3b;color:#5fe39a}
  .badge.warn{background:#3a1d1b;border-color:#5b2a26;color:#ff9b96}
  .ctl{display:flex;gap:8px;margin-top:12px}
  .ctl button{flex:1;padding:9px;border-radius:9px;border:1px solid #2a3542;background:#1b222c;
              color:#e7edf3;font-size:13px;cursor:pointer}
  .ctl button:active{transform:translateY(1px)}
  .warnbox{background:#3a1d1b;border:1px solid #5b2a26;color:#ffb4af;font-size:12px;
           padding:8px 10px;border-radius:8px;margin-top:10px}
  .num{font-variant-numeric:tabular-nums}
</style>
</head>
<body>
<header><h1>🔋 Akku-Monitor</h1><span id="age">…</span></header>
<main id="app"></main>
<script>
const CTRL = __ENABLE_CONTROL__;  // replaced by firmware (0/1)
const f = (n,d=2)=> (n===null||n===undefined||Number.isNaN(n))?'–':Number(n).toFixed(d);

async function tick(){
  let j;
  try{ j = await (await fetch('/api/data',{cache:'no-store'})).json(); }
  catch(e){ document.getElementById('age').textContent='offline'; return; }
  document.getElementById('age').textContent = new Date().toLocaleTimeString();
  const app = document.getElementById('app');
  app.innerHTML = j.batteries.map(render).join('');
}

function render(b,idx){
  const live = b.connected && b.valid;
  const cells = (b.cells||[]).map((v,i)=>`<span class="cell">#${i+1} ${f(v,3)} V</span>`).join('');
  const temps = (b.temps||[]).map((t,i)=>`<div class="kv"><span class="k">Temp ${i+1}</span><b class="num">${f(t,1)} °C</b></div>`).join('');
  const prot = b.protection||0, realFault = prot & ~0x1000;  // 0x1000 = MOSFET software lock
  const protBadge = prot===0
    ? '<span class="badge on">OK</span>'
    : realFault
      ? `<span class="badge warn">Schutz 0x${(prot>>>0).toString(16)}</span>`
      : '<span class="badge">MOSFET per Software aus</span>';
  const badges = [
    `<span class="badge ${b.charging_fet?'on':''}">Laden ${b.charging_fet?'an':'aus'}</span>`,
    `<span class="badge ${b.discharging_fet?'on':''}">Entladen ${b.discharging_fet?'an':'aus'}</span>`,
    b.balancing?'<span class="badge on">Balancing</span>':'',
    protBadge,
  ].join('');
  let ctl='';
  if(CTRL){
    ctl = `<div class="ctl">
      <button onclick="ctl(${idx},'charge',${b.charging_fet?0:1})">Laden ${b.charging_fet?'AUS':'AN'}</button>
      <button onclick="ctl(${idx},'discharge',${b.discharging_fet?0:1})">Entladen ${b.discharging_fet?'AUS':'AN'}</button>
    </div><div class="warnbox">⚠️ Schaltet den MOSFET (bis 300 A). Unter Last mit Bedacht!</div>`;
  }
  return `<div class="card">
    <h2>${b.name}</h2>
    <div class="sub"><span class="dot ${live?'ok':'bad'}"></span>${live?('aktualisiert vor '+b.age_s+' s'):(b.connected?'verbunden, warte auf Daten':'nicht verbunden')}</div>
    <div class="soc"><i style="width:${live?b.soc:0}%"></i></div>
    <div class="grid">
      <div class="kv"><span class="k">SOC</span><b class="num">${live?b.soc+' %':'–'}</b></div>
      <div class="kv"><span class="k">Spannung</span><b class="num">${live?f(b.voltage)+' V':'–'}</b></div>
      <div class="kv"><span class="k">Strom</span><b class="num">${live?f(b.current)+' A':'–'}</b></div>
      <div class="kv"><span class="k">Leistung</span><b class="num">${live?f(b.power,0)+' W':'–'}</b></div>
      <div class="kv"><span class="k">Rest</span><b class="num">${live?f(b.remaining_ah)+' Ah':'–'}</b></div>
      <div class="kv"><span class="k">Kapazität</span><b class="num">${live?f(b.nominal_ah)+' Ah':'–'}</b></div>
      <div class="kv"><span class="k">Zyklen</span><b class="num">${live?b.cycles:'–'}</b></div>
      <div class="kv"><span class="k">Zell-Δ</span><b class="num">${live?f(b.cell_delta,3)+' V':'–'}</b></div>
      ${temps}
    </div>
    <div class="cells">${cells}</div>
    <div class="badges">${badges}</div>
    ${ctl}
  </div>`;
}

async function ctl(idx,what,on){
  if(!confirm('Wirklich '+what+' '+(on?'AN':'AUS')+' schalten?')) return;
  await fetch(`/api/control?bat=${idx}&what=${what}&on=${on}`,{method:'POST'});
  setTimeout(tick,500);
}

tick(); setInterval(tick,3000);
</script>
</body>
</html>)HTML";
