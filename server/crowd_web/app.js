// The Crowd: follow live Project Arena matches and cheer from a phone. Talks JSON over a WebSocket (/ws) to the
// community relay (server/projectarena_relay.py, class Crowd). Anonymous: nothing identifies the fan; the fan score
// stays on this phone (localStorage).
'use strict';

const STATES = ['WAITING FOR PLAYERS', 'GET READY', 'LIVE', 'ROUND OVER', 'FINAL', 'FIST BUMP'];
const LIVE_STATES = [1, 2, 3, 5];
const $ = (id) => document.getElementById(id);

let ws = null, connected = false, retry = 1000;
let matches = [], watching = null, cur = null, last = null;
let pending = [0, 0], flagOn = false, shakeOn = false, side = -1;
let call = -1, wakeLock = null, toastTimer = 0;
const store = (() => { try { const s = window.localStorage; s.setItem('t', '1'); s.removeItem('t'); return s; } catch (e) { return null; } })();
let fan = { score: 0, streak: 0 };
try { fan = Object.assign(fan, JSON.parse((store && store.getItem('fan')) || '{}')); } catch (e) { /* fresh */ }

// ---------------------------------------------------------------- connection
function connect() {
  const url = (location.protocol === 'https:' ? 'wss://' : 'ws://') + location.host + '/ws';
  try { ws = new WebSocket(url); } catch (e) { return later(); }
  ws.onopen = () => {
    connected = true; retry = 1000; $('conn').classList.add('on');
    if (watching) send({ t: 'watch', id: watching });
  };
  ws.onmessage = (ev) => { let m; try { m = JSON.parse(ev.data); } catch (e) { return; } onMessage(m); };
  ws.onclose = () => { connected = false; $('conn').classList.remove('on'); later(); };
  ws.onerror = () => { try { ws.close(); } catch (e) { /* closed */ } };
}
function later() { setTimeout(connect, retry); retry = Math.min(15000, retry * 2); }
function send(o) { if (ws && connected) { try { ws.send(JSON.stringify(o)); } catch (e) { /* reconnecting */ } } }
setInterval(() => send({ t: 'ping' }), 20000);

function onMessage(m) {
  if (m.t === 'list') { matches = m.m || []; if (!watching) renderList(); }
  else if (m.t === 'm') { if (m.id === watching) renderMatch(m); }
  else if (m.t === 'gone') { if (m.id === watching) { leave(); toast('That match has ended: the host left the arena.'); } }
}

// ---------------------------------------------------------------- helpers
function nation(n) { return (typeof NATIONS !== 'undefined' && n > 0 && n < NATIONS.length) ? NATIONS[n] : null; }
function colorOf(p) { return /^#[0-9a-f]{6}$/i.test(p.c || '') ? p.c : '#888888'; }
function chipStyle(el, p) {
  const N = nation(p.nat);
  if (N) { el.style.backgroundImage = 'url(flags/' + N[0].toLowerCase() + '.png)'; el.style.backgroundColor = ''; }
  else { el.style.backgroundImage = ''; el.style.backgroundColor = colorOf(p); }
}
function label(m, k) { const p = m.p[k]; return (p.n === 'OPEN' || !p.n) ? 'OPEN' : p.n; }
function clockText(m) {
  if (m.ot) return 'OVERTIME';
  if (m.adv) return m.rnd > 0 ? 'ROUND ' + m.rnd : '';
  if (!(m.clock > 0)) return '';
  const s = Math.ceil(m.clock), mm = Math.floor(s / 60), ss = s % 60;
  return mm + ':' + (ss < 10 ? '0' : '') + ss + ' LEFT';
}
const pct = (a, b) => (b > 0 ? Math.round(100 * a / b) : 0);

// one row of the stats table: the two values, the label, and a bar split by the raw numbers (a, b)
function statRow(label, l, r, a, b, small) {
  const row = el('div', 'srow'), top = el('div', 'srow-top');
  const lv = el('span', 'v' + (a > b ? ' lead' : '')), rv = el('span', 'v r' + (b > a ? ' lead' : ''));
  lv.textContent = l; rv.textContent = r;
  if (small) { lv.appendChild(el('small', '', small[0])); rv.insertBefore(el('small', '', small[1]), rv.firstChild); }
  top.appendChild(lv); top.appendChild(el('span', 'k', label)); top.appendChild(rv);
  row.appendChild(top);
  const sp = el('div', 'split'), x = el('i'), y = el('i');
  const tot = a + b;
  x.style.flexGrow = tot > 0 ? a : 1; y.style.flexGrow = tot > 0 ? b : 1;
  if (tot <= 0) { x.style.opacity = y.style.opacity = 0.25; }
  sp.appendChild(x); sp.appendChild(y); row.appendChild(sp);
  return row;
}
function renderStats(m) {
  $('sh0').textContent = label(m, 0); $('sh1').textContent = label(m, 1);
  const box = $('statrows'); box.textContent = '';
  const s = [m.p[0].m, m.p[1].m];
  if (!s[0] || !s[1]) { box.appendChild(el('div', 'call-msg', (m.old ? 'This arena runs an older Project Arena: live stats, flags and cheers heard in the arena come with 0.3.7.' : 'No statistics for this match yet.'))); }
  else {
    const [a, b] = s;
    box.appendChild(statRow('THROWS', a.throws, b.throws, a.throws, b.throws));
    box.appendChild(statRow('ACCURACY', pct(a.on, a.throws) + '%', pct(b.on, b.throws) + '%', pct(a.on, a.throws), pct(b.on, b.throws),
      [a.on + '/' + a.throws, b.on + '/' + b.throws]));
    box.appendChild(statRow('HITS', a.hits, b.hits, a.hits, b.hits));
    box.appendChild(statRow('STRIKES', a.strikes, b.strikes, a.strikes, b.strikes));
    box.appendChild(statRow('BLOCKS', a.blocks, b.blocks, pct(a.blocks, a.faced), pct(b.blocks, b.faced),
      [pct(a.blocks, a.faced) + '%', pct(b.blocks, b.faced) + '%']));
    box.appendChild(statRow('CATCHES', a.catches, b.catches, a.catches, b.catches));
    box.appendChild(statRow('TOP SPEED', a.top, b.top, a.top, b.top, ['km/h', 'km/h']));
  }
  const c = [m.p[0].car, m.p[1].car];
  $('carblock').classList.toggle('hidden', !c[0] && !c[1]);
  const cb = $('carrows'); cb.textContent = '';
  if (c[0] || c[1]) {
    const z = { matches: 0, wins: 0, throws: 0, on: 0, blocks: 0, faced: 0, top: 0 };
    const [a, b] = [c[0] || z, c[1] || z];
    // a player with no online matches yet (or none on record): a dash, not zeros
    const has = [!!(c[0] && c[0].matches), !!(c[1] && c[1].matches)];
    const v = (k, text) => (has[k] ? text : '—');
    const sm = (k, text) => (has[k] ? text : '');
    const rec = (x) => x.wins + '-' + (x.matches - x.wins);
    cb.appendChild(statRow('RECORD', v(0, rec(a)), v(1, rec(b)), pct(a.wins, a.matches), pct(b.wins, b.matches),
      [sm(0, pct(a.wins, a.matches) + '%'), sm(1, pct(b.wins, b.matches) + '%')]));
    cb.appendChild(statRow('ACCURACY', v(0, pct(a.on, a.throws) + '%'), v(1, pct(b.on, b.throws) + '%'), pct(a.on, a.throws), pct(b.on, b.throws)));
    cb.appendChild(statRow('BLOCKS', v(0, pct(a.blocks, a.faced) + '%'), v(1, pct(b.blocks, b.faced) + '%'), pct(a.blocks, a.faced), pct(b.blocks, b.faced)));
    cb.appendChild(statRow('TOP SPEED', v(0, a.top), v(1, b.top), a.top, b.top, [sm(0, 'km/h'), sm(1, 'km/h')]));
  }
}
function toast(text) {
  const t = $('toast'); t.textContent = text; t.classList.remove('hidden');
  clearTimeout(toastTimer); toastTimer = setTimeout(() => t.classList.add('hidden'), 3500);
}
function el(tag, cls, text) { const e = document.createElement(tag); if (cls) e.className = cls; if (text != null) e.textContent = text; return e; }

// ---------------------------------------------------------------- the list
function renderList() {
  const box = $('matches');
  box.textContent = '';
  const sorted = matches.slice().sort((a, b) => (LIVE_STATES.includes(b.st) - LIVE_STATES.includes(a.st)) || (b.fans - a.fans));
  for (const m of sorted) {
    const card = el('button', 'card');
    card.style.setProperty('--c0', colorOf(m.p[0]));
    card.style.setProperty('--c1', colorOf(m.p[1]));
    const top = el('div', 'card-top');
    const live = LIVE_STATES.includes(m.st);
    top.appendChild(el('span', 'live-dot' + (live ? '' : ' idle'), (live ? '● ' : '') + (STATES[m.st] || 'LIVE')));
    const ck = clockText(m);
    top.appendChild(el('span', '', m.host + "'S ARENA · " + (ck || (m.adv ? 'ADVANCED' : 'BASIC')) + ' · ' + m.fans + (m.fans === 1 ? ' FAN' : ' FANS')));
    card.appendChild(top);
    const row = el('div', 'card-row');
    for (let k = 0; k < 2; ++k) {
      const p = el('div', 'card-p' + (k ? ' right' : ''));
      const chip = el('div', 'chip'); chipStyle(chip, m.p[k]);
      p.appendChild(chip); p.appendChild(el('span', 'n', label(m, k)));
      row.appendChild(p);
      if (k === 0) row.appendChild(el('div', 'card-score', m.s[0] + ' – ' + m.s[1]));
    }
    card.appendChild(row);
    card.onclick = () => watch(m.id);
    box.appendChild(card);
  }
  $('empty').classList.toggle('hidden', matches.length > 0);
}

// ---------------------------------------------------------------- one match
function watch(id) {
  watching = id; last = null; cur = null; call = -1; side = -1;
  setFlag(false);
  $('list-view').classList.add('hidden');
  $('match-view').classList.remove('hidden');
  $('back').classList.remove('hidden');
  $('callmsg').textContent = 'Pick a side. A right call scores you points.';
  $('callmsg').classList.remove('good');
  send({ t: 'watch', id });
  keepAwake(true);
  history.pushState({ watching: id }, '');
  window.scrollTo(0, 0);
}
function leave(fromHistory) {
  watching = null; cur = null;
  setFlag(false);
  send({ t: 'watch', id: null });
  $('match-view').classList.add('hidden');
  $('list-view').classList.remove('hidden');
  $('back').classList.add('hidden');
  keepAwake(false);
  renderList();
  if (!fromHistory && history.state && history.state.watching) history.back();
}
$('back').onclick = () => leave();
window.addEventListener('popstate', () => { if (watching) leave(true); });

function renderMatch(m) {
  const prev = cur;
  cur = m;
  const root = document.documentElement;
  root.style.setProperty('--c0', colorOf(m.p[0]));
  root.style.setProperty('--c1', colorOf(m.p[1]));
  const live = LIVE_STATES.includes(m.st);
  $('state').textContent = (live ? '● ' : '') + (STATES[m.st] || 'LIVE') + ' · ' + (m.adv ? 'ADVANCED' : 'BASIC');
  $('state').classList.toggle('idle', !live);
  $('clock').textContent = clockText(m);
  for (let k = 0; k < 2; ++k) {
    const p = m.p[k], N = nation(p.nat);
    chipStyle($('flag' + k), p);
    $('name' + k).textContent = label(m, k);
    $('nat' + k).textContent = N ? N[1] : (k ? 'ORANGE END' : 'BLUE END');
    $('cheername' + k).textContent = N ? N[1].toUpperCase() : label(m, k);
    $('call' + k).textContent = label(m, k);
    const pts = $('pts' + k);
    if (pts.textContent !== String(m.s[k])) {
      pts.textContent = m.s[k];
      if (prev) { pts.classList.remove('bump'); void pts.offsetWidth; pts.classList.add('bump'); }
    }
    const r = $('rnd' + k);
    r.textContent = '';
    if (m.adv) for (let i = 0; i < 2; ++i) r.appendChild(el('i', i < m.s[2 + k] ? 'won' : ''));
    $('hype' + k).style.width = Math.max(0, Math.min(100, (m.h && m.h[k]) || 0)) + '%';
  }
  renderStats(m);
  $('fans').textContent = m.fans + (m.fans === 1 ? ' FAN' : ' FANS');
  const w = m.w || [0, 0];
  $('waves').textContent = (w[0] + w[1]) ? '⚑ ' + w[0] + ' · ' + w[1] : '';
  // calls: who scored since the last update?
  if (last && (m.s[0] !== last.s[0] || m.s[1] !== last.s[1])) {
    const scored = m.s[0] > last.s[0] ? 0 : m.s[1] > last.s[1] ? 1 : -1;
    if (scored >= 0 && call >= 0) {
      if (scored === call) {
        fan.streak += 1;
        const pts = 10 * fan.streak;
        fan.score += pts;
        $('callmsg').textContent = 'Called it! +' + pts + (fan.streak > 1 ? ' (streak x' + fan.streak + ')' : '');
        $('callmsg').classList.add('good');
        buzz([30, 40, 30]);
      } else {
        fan.streak = 0;
        $('callmsg').textContent = 'Not this time. Call the next one!';
        $('callmsg').classList.remove('good');
      }
      saveFan();
      call = -1;
      for (let k = 0; k < 2; ++k) $('call' + k).classList.remove('on');
    }
  }
  last = m;
  $('fanscore').textContent = fan.score;
  $('streak').textContent = fan.streak;
}
function saveFan() { try { if (store) store.setItem('fan', JSON.stringify(fan)); } catch (e) { /* private mode */ } }

// ---------------------------------------------------------------- cheering
function buzz(p) { try { if (navigator.vibrate) navigator.vibrate(p); } catch (e) { /* no vibration */ } }
function cheer(k, x, y) {
  if (!cur) return;
  const changed = side !== k;
  side = k;
  pending[k] += 1;
  buzz(12);
  const b = $('cheer' + k);
  if (x != null) {
    const r = b.getBoundingClientRect(), d = el('span', 'ripple');
    d.style.left = (x - r.left - 10) + 'px'; d.style.top = (y - r.top - 10) + 'px';
    b.appendChild(d); setTimeout(() => d.remove(), 600);
  }
  if (flagOn && changed) send({ t: 'flag', side: k });
}
for (let k = 0; k < 2; ++k) {
  const b = $('cheer' + k);
  b.addEventListener('pointerdown', (e) => { e.preventDefault(); cheer(k, e.clientX, e.clientY); });
  $('call' + k).onclick = () => {
    if (!cur) return;
    call = k;
    for (let j = 0; j < 2; ++j) $('call' + j).classList.toggle('on', j === k);
    $('callmsg').textContent = 'You called ' + label(cur, k) + ' to score next.';
    $('callmsg').classList.remove('good');
  };
}
setInterval(() => {   // cheers go out four times a second (the server counts at most 8 a second per phone)
  for (let k = 0; k < 2; ++k) if (pending[k]) { send({ t: 'cheer', side: k, n: Math.min(pending[k], 8) }); pending[k] = 0; }
}, 250);

// wave the flag of the end you cheer for (counted by the server while it is refreshed)
function setFlag(on) {
  flagOn = on;
  $('wave').classList.toggle('on', on);
  if (!on) send({ t: 'flag', side: -1 });
}
$('wave').onclick = () => {
  if (side < 0 && !flagOn) { toast('Cheer for a side first: your flag waves for them.'); return; }
  setFlag(!flagOn);
  if (flagOn) { send({ t: 'flag', side }); if (cur) $('wave').style.setProperty('--pick', colorOf(cur.p[side])); }
};
setInterval(() => { if (flagOn && side >= 0) send({ t: 'flag', side }); }, 2000);

// shake the phone to cheer (iPhone asks for motion permission once)
let lastShake = 0;
function onMotion(e) {
  const a = e.accelerationIncludingGravity || e.acceleration;
  if (!a || side < 0) return;
  const g = Math.sqrt((a.x || 0) ** 2 + (a.y || 0) ** 2 + (a.z || 0) ** 2);
  const now = performance.now();
  if (g > 22 && now - lastShake > 160) { lastShake = now; cheer(side); }
}
$('shake').onclick = async () => {
  if (shakeOn) { shakeOn = false; window.removeEventListener('devicemotion', onMotion); $('shake').classList.remove('on'); return; }
  if (side < 0) { toast('Cheer for a side first: shaking cheers for them.'); return; }
  try {
    if (typeof DeviceMotionEvent !== 'undefined' && typeof DeviceMotionEvent.requestPermission === 'function') {
      if (await DeviceMotionEvent.requestPermission() !== 'granted') { toast('Motion access was not allowed.'); return; }
    }
  } catch (e) { toast('Motion access is not available.'); return; }
  shakeOn = true;
  window.addEventListener('devicemotion', onMotion);
  $('shake').classList.add('on');
  toast('Shake your phone to cheer!');
};

// keep the screen on while watching
async function keepAwake(on) {
  try {
    if (on && 'wakeLock' in navigator && !wakeLock) { wakeLock = await navigator.wakeLock.request('screen'); wakeLock.onrelease = () => { wakeLock = null; }; }
    if (!on && wakeLock) { await wakeLock.release(); wakeLock = null; }
  } catch (e) { /* not allowed now */ }
}
document.addEventListener('visibilitychange', () => { if (document.visibilityState === 'visible' && watching) keepAwake(true); });

if ('serviceWorker' in navigator) navigator.serviceWorker.register('sw.js').catch(() => {});
renderList();
connect();
