// ============================================================================
//  web_page.h — the single-page web UI, served from flash (PROGMEM).
//  It polls GET /api/data (JSON) and renders one card per battery.
//  Control buttons POST to /api/control and only appear if the firmware was
//  built with ENABLE_CONTROL = 1.
//  With ENABLE_TRUMA = 1 a Truma heater card is added (GET/POST /api/truma).
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
  .card.wide{grid-column:1/-1}
  .stale{opacity:.4;pointer-events:none}
  .ctl button.sel{background:#163a26;border-color:#235b3b;color:#5fe39a}
  .ctl button:disabled{opacity:.4;cursor:default}
  .big{font-size:28px;font-weight:600;font-variant-numeric:tabular-nums}
  .rng{width:100%;height:28px;margin:4px 0 0;accent-color:#43c97a}
  .lbl{font-size:12px;color:#8b98a8;margin-top:12px}
  .small{font-size:11px;color:#8b98a8;margin-top:10px}
  .small a{color:#8b98a8}
</style>
</head>
<body>
<header><h1>🔋 Akku-Monitor</h1><span id="age">…</span></header>
<main id="app"></main>
<script>
let CTRL = 0, TRUMA = 0;  // build flags, sent with /api/data
const f = (n,d=2)=> (n===null||n===undefined||Number.isNaN(n))?'–':Number(n).toFixed(d);

// While a slider is being dragged, the 3 s refresh must not re-render it.
let slidingSince = 0;
const sliding = () => slidingSince && Date.now() - slidingSince < 15000;

async function tick(){
  if(sliding()) return;
  let j;
  try{ j = await (await fetch('/api/data',{cache:'no-store'})).json(); }
  catch(e){ document.getElementById('age').textContent='offline'; return; }
  CTRL = j.control || 0; TRUMA = j.truma || 0;
  let t = null;
  if(TRUMA){ try{ t = await (await fetch('/api/truma',{cache:'no-store'})).json(); }catch(e){} }
  document.getElementById('age').textContent = new Date().toLocaleTimeString();
  const app = document.getElementById('app');
  if(sliding()) return;  // started dragging while we were fetching
  app.innerHTML = (TRUMA ? renderTruma(t) : '') + j.batteries.map(render).join('');
}

// Range input: the number in #<id> follows while dragging, release sets it.
function slider(what, id, val, min, max, step, dec, dis){
  return `<input type="range" class="rng" ${dis} min="${min}" max="${max}" step="${step}" value="${val===null?min:val}"
    onpointerdown="slidingSince=Date.now()" ontouchstart="slidingSince=Date.now()"
    oninput="slidingSince=Date.now();document.getElementById('${id}').textContent=Number(this.value).toFixed(${dec})"
    onchange="slidingSince=0;tr('${what}',this.value)">`;
}

// ---- Truma iNet X card ------------------------------------------------------
const T_STATUS = {
  starting:'Bluetooth startet', not_paired:'Nicht gekoppelt', searching:'Suche Panel',
  waiting:'Warte auf neuen Verbindungsversuch', rejected:'Panel lehnt den ESP ab: neu koppeln',
  pairing_connecting:'Kopplung: verbinde', connecting:'Verbinde', pairing_bonding:'Kopplung: tausche Schlüssel',
  encrypting:'Verschlüssele', subscribing:'Abonniere', connected:'Verbunden'};
const T_MODE = {0:'Aus',1:'Auto',2:'Kühlen',3:'Heizen',4:'Heizen',5:'Lüften',6:'Entfeuchten'};
const T_WATER = ['Eco (40 °C)','Comfort (60 °C)','Hot (70 °C)'];
// EnergySrc.ElectricLevel: 0 off, 1 = 900 W, 2 = 1800 W. The heater keeps at
// least one source on by itself (switching off the last one turns the other on).
const T_ELECTRIC = ['Aus','900 W','1800 W'];
// AirHeating.Active / WaterHeating.Active: 0 off, 1 heating now, 2 on but target reached
const T_ACTIVE = ['aus','heizt','bereit'];
const T_DEV = {257:'Panel',513:'Heizung',1537:'Bluetooth-Modul',1029:'Stromverteilung'};
const T_SYMBOL = ['Automatik','Heizen','Heizen (Klima)','Kühlen','Lüften','Entfeuchten','Warmwasser'];
const T_DAYS = ['Mo','Di','Mi','Do','Fr','Sa','So'];
// PowerMgmt.PwrMode of the panel
const T_PWR = {0:'aus',1:'Standby',3:'an',4:'schaltet aus',5:'geht in Standby'};
let devOpen = false;
const days = m => m===127 ? 'täglich' : T_DAYS.filter((d,i)=>m&(1<<i)).join(' ') || '–';
const ago = s => s<60 ? s+' s' : s<3600 ? Math.round(s/60)+' min' : Math.round(s/3600)+' h';

function renderTruma(t){
  if(!t) return `<div class="card wide"><h2>Truma Heizung</h2><div class="sub"><span class="dot bad"></span>keine Daten</div></div>`;
  let st = T_STATUS[t.status] || t.status;
  if(t.status==='pairing') st = `Kopplungsmodus (noch ${t.pairing_left_s} s): Panel auf „Neues Gerät hinzufügen“ stellen`;
  if(t.status==='session_starting') st = `Starte Sitzung (${t.session})`;
  const on = t.connected, dis = on ? '' : 'disabled';
  const m = t.mode, heat = (m===3||m===4);
  const sel = c => c ? 'sel' : '';
  const water = t.water_active!==null && t.water_active!==0;  // 1 active, 2 idle
  const boost = t.water_boost===1;
  const isErr = e => e && e!=='0' && e!=='[]';
  const errList = e => { try { const a = JSON.parse(e); return Array.isArray(a) ? a : null; } catch(_) { return null; } };
  // ErrCode: [{"sev":1,"code":255,"resettable":0}, ...]; anything else is shown as is
  const errText = e => { const a = errList(e); return a ? a.map(x=>'Code '+x.code+(x.resettable?'':' (nicht quittierbar)')).join(', ') : e; };
  const errReset = e => { const a = errList(e); return !a || a.some(x=>x.resettable); };
  const errSrc = [isErr(t.error)?t.error:'', isErr(t.panel_error)?t.panel_error:''];
  const errs = [errSrc[0]?'Heizung: '+errText(errSrc[0]):'', errSrc[1]?'Panel: '+errText(errSrc[1]):''].filter(x=>x);
  const canReset = errSrc.some(e=>e && errReset(e));
  const err = errs.length ? `<div class="warnbox">Fehler – ${errs.join(' · ')}
      ${canReset?`<div class="ctl"><button ${dis} onclick="tr('error_reset',1)">Fehler quittieren</button></div>`:''}</div>` : '';
  return `<div class="card wide">
    <h2>Truma Heizung</h2>
    <div class="sub"><span class="dot ${on?'ok':'bad'}"></span>${st}${t.panel_name?' · '+t.panel_name:''}${!on&&t.values_age_s>0?' · Stand vor '+ago(t.values_age_s):''}</div>
    ${t.restored_age_s>=0&&t.restored_age_s<43200?`<div class="small" style="margin:-6px 0 10px">Einstellungen nach Stromausfall wiederhergestellt vor ${ago(t.restored_age_s)}</div>`:''}
    <div class="${on?'':'stale'}">
    <div class="grid">
      <div><div class="k">Raum</div><div class="big">${f(t.room_temp,1)} °C</div></div>
      <div><div class="k">Soll</div><div class="big"><span id="tgtv">${f(t.target_temp,1)}</span> °C</div></div>
    </div>
    ${slider('target','tgtv',t.target_temp,5,30,0.5,1,dis)}
    <div class="badges">
      <span class="badge ${heat?'on':''}">Modus ${m===null?'–':(T_MODE[m]||m)}</span>
      <span class="badge ${t.heating_active>0?'on':''}">Heizung ${T_ACTIVE[t.heating_active]||'–'}</span>
      <span class="badge ${water?'on':''}">Warmwasser ${T_ACTIVE[t.water_active]||'–'}${t.water_temp!==null?' · '+f(t.water_temp,0)+' °C':''}</span>
      ${t.line_power!==null?`<span class="badge ${t.line_power===1?'on':''}">${t.line_power===1?'230 V angeschlossen':'kein 230 V'}</span>`:''}
      ${boost?`<span class="badge on">Boost${t.water_boost_time?' · noch '+Math.ceil(t.water_boost_time/60)+' min':''}</span>`:''}
    </div>
    <div class="lbl">Heizung</div>
    <div class="ctl" style="margin-top:4px">
      <button ${dis} class="${sel(m===0)}" onclick="tr('mode',0)">Aus</button>
      <button ${dis} class="${sel(heat)}" onclick="${boost?"if(confirm('Heizen beendet den Boost – fortfahren?'))":''}tr('mode',3)">Heizen</button>
      <button ${dis} class="${sel(m===5)}" onclick="tr('mode',5)">Lüften</button>
    </div>
    ${heat ? `<div class="lbl">Heizmodus</div>
    <div class="ctl" style="margin-top:4px">
      <button ${dis} class="${sel(t.heating_mode===0)}" onclick="tr('heat_mode',0)">Schnell</button>
      <button ${dis} class="${sel(t.heating_mode===1)}" onclick="tr('heat_mode',1)">Komfort</button>
    </div>` : ''}
    ${m===5 ? `<div class="lbl">Lüfterstufe <b id="fanv">${t.fan_level===null?'–':t.fan_level}</b></div>
    ${slider('fan','fanv',t.fan_level,1,10,1,0,dis)}` : ''}
    <div class="lbl">Warmwasser</div>
    <div class="ctl" style="margin-top:4px">
      <button ${dis} class="${sel(t.water_active===0)}" onclick="tr('water',0)">Aus</button>
      ${T_WATER.map((n,i)=>`<button ${dis} class="${sel(water&&t.water_mode===i)}" onclick="trWater(${i})">${n.split(' ')[0]}</button>`).join('')}
      <button ${on&&(heat||boost)?'':'disabled'} title="nur bei eingeschalteter Heizung" class="${sel(boost)}" onclick="tr('boost',${boost?0:1})">Boost</button>
    </div>
    ${t.diesel_level!==null ? `<div class="lbl">Energiequelle Diesel</div>
    <div class="ctl" style="margin-top:4px">
      <button ${dis} class="${sel(t.diesel_level===0)}" onclick="tr('diesel',0)">Aus</button>
      <button ${dis} class="${sel(t.diesel_level===1)}" onclick="tr('diesel',1)">An</button>
    </div>` : ''}
    ${t.electric_level!==null ? `<div class="lbl">Energiequelle Strom</div>
    <div class="ctl" style="margin-top:4px">
      ${T_ELECTRIC.map((n,i)=>`<button ${dis} class="${sel(t.electric_level===i)}" onclick="tr('electric',${i})">${n}</button>`).join('')}
    </div>` : ''}
    ${err}
    ${(t.timers||[]).some(x=>x.defined) ? `<div class="lbl">Timer</div>
    ${t.timers.map((x,i)=>x.defined?`<div class="kv" style="align-items:center">
      <span>${x.name||('Timer '+(i+1))} <span class="k">· ${x.start}–${x.end} · ${days(x.weekdays)} · ${T_SYMBOL[x.symbol]||''}</span></span>
      <span class="ctl" style="margin:0;flex:none"><button ${dis} class="${sel(x.enabled===1)}" onclick="tr('timer',${x.enabled===1?0:1},${i+1})">${x.enabled===1?'An':'Aus'}</button></span>
    </div>`:'').join('')}` : ''}
    </div>
    <details ${devOpen?'open':''} ontoggle="devOpen=this.open" style="margin-top:12px">
      <summary class="k" style="cursor:pointer">Geräte &amp; Kopplung (${(t.devices||[]).length})</summary>
      ${(t.devices||[]).map(d=>`<div class="kv"><span>${T_DEV[d.addr]||('0x'+d.addr.toString(16).padStart(4,'0'))}${d.name?' <span class="k">· '+d.name+'</span>':''}</span>
        <span class="k num">${d.firmware?'FW '+d.firmware+' · ':''}${d.serial?d.serial+' · ':''}vor ${ago(d.age_s)}</span></div>`).join('')}
      ${t.pwr_mode!==null ? `<div class="lbl">Display <span class="k">· ${T_PWR[t.pwr_mode]||t.pwr_mode}${t.pwr_mode===3&&t.screensaver===1?', abgedunkelt':''}${t.panel_idle_s!==null?' · zuletzt bedient vor '+ago(t.panel_idle_s):''}</span></div>
      <div class="ctl" style="margin-top:4px">
        <button ${dis} class="${sel(t.pwr_mode===3)}" onclick="tr('pwr_mode',3)">An</button>
        <button ${dis} class="${sel(t.pwr_mode===1||t.pwr_mode===5)}" onclick="tr('pwr_mode',1)">Standby</button>
      </div>
      <div class="lbl">Helligkeit <b id="brv">${t.brightness===null?'–':t.brightness}</b> %</div>
      ${slider('brightness','brv',t.brightness,10,100,5,0,dis)}
      <div class="lbl">Helligkeit abgedunkelt <b id="dbrv">${t.dark_brightness===null?'–':t.dark_brightness}</b> %</div>
      ${slider('dark_brightness','dbrv',t.dark_brightness,0,100,1,0,dis)}
      <div class="lbl">Abdunkeln nach <b id="dtov">${t.display_timeout===null?'–':t.display_timeout}</b> s</div>
      ${slider('display_timeout','dtov',t.display_timeout,10,600,10,0,dis)}` : ''}
      <div class="small">Nach Stromausfall wiederherstellen: <a href="#" onclick="tr('restore',${t.restore_enabled?0:1});return false">${t.restore_enabled?'an':'aus'}</a></div>
      <div class="small"><a href="#" onclick="trPair();return false">Koppeln</a>${t.paired?` · <a href="#" onclick="trAction('unpair','Kopplung wirklich entfernen?');return false">Entkoppeln</a> · <a href="#" onclick="trAction('reconnect');return false">Neu verbinden</a>`:''}</div>
    </details>
    <div class="small">${t.link_params?'Funk: '+t.link_params+'<br>':''}${t.last_disconnect?'Letzte Trennung: '+t.last_disconnect+'<br>':''}<a href="/api/truma/log" target="_blank">Log</a> · <a href="/api/truma/bus" target="_blank">Bus-Werte</a></div>
  </div>`;
}

async function tr(what,value,n){
  await fetch(`/api/truma?what=${what}&value=${value}${n?'&n='+n:''}`,{method:'POST'});
  setTimeout(tick,1500);  // the panel echoes the new value
}
// Hot water level: set the mode, then make sure hot water is on.
async function trWater(mode){
  await fetch(`/api/truma?what=water_mode&value=${mode}`,{method:'POST'});
  await fetch(`/api/truma?what=water&value=1`,{method:'POST'});
  setTimeout(tick,1500);
}
function trPair(){
  if(!confirm('Kopplungsmodus starten? Eine bestehende Kopplung wird entfernt. Danach am Panel „Neues Gerät hinzufügen“ wählen.')) return;
  tr('pair',0);
}
function trAction(what,question){
  if(question && !confirm(question)) return;
  tr(what,0);
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
