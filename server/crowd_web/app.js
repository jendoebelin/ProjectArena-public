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
  if (m.t === 'hello') { notifyHello(m.push || null); }
  else if (m.t === 'push') { notifyReply(m); }
  else if (m.t === 'list') { matches = m.m || []; if (!watching) { renderList(); openWanted(); } }
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
// FOUR PLAYERS (FOUR CORNERS, FIREBALL BLITZ; hosts from 0.3.24 send all four). The two sides fans cheer for: two
// players, the players; four in teams (2 v 2, 3 v 1), the two teams; four each for themselves (free-for-all, FIREBALL
// BLITZ): none, so the cheers and calls for a side are left out and the four-player board shows everyone
const four = (m) => m.np === 4 && Array.isArray(m.p) && m.p.length === 4;
// the teams the match plays: 0 each for themselves, 1 2 v 2, 2 3 v 1. FIREBALL BLITZ plays in teams only from hosts on
// 0.3.24, which add 128 to say so (an older host sends its TEAMS setting there too, which FIREBALL BLITZ never played)
const tmOf = (m) => (m.room === 3 ? ((m.tm & 128) ? m.tm & 3 : 0) : (m.tm & 3));
const teams = (m) => four(m) && (tmOf(m) === 1 || tmOf(m) === 2);
const pname = (q) => ((q.n === 'OPEN' || !q.n) ? 'OPEN' : q.n);
function sides(m) {
  if (!four(m)) return [m.p[0], m.p[1]];
  if (!teams(m)) return null;
  return [0, 1].map((t) => {
    const names = m.p.filter((q) => q.team === t).map(pname);
    const n = names.length > 1 ? names.slice(0, -1).join(', ') + ' & ' + names[names.length - 1] : (names[0] || 'OPEN');
    const first = m.p.find((q) => q.team === t);
    return { n, nat: 0, c: first ? first.c : '#888888', team: t };
  });
}
function label(m, k) { const s = sides(m); return s ? pname(s[k]) : ''; }
function modeText(m) {
  const tm = tmOf(m), t = tm === 1 ? '2 V 2' : tm === 2 ? '3 V 1' : '';
  if (m.room === 3) return 'FIREBALL BLITZ' + (t ? ' · ' + t : '');
  return 'FOUR CORNERS · ' + (t || 'FREE-FOR-ALL');
}
function teamWord(m, k) { return tmOf(m) === 2 ? (k ? 'THE ONE' : 'THE THREE') : (k ? 'TEAM TWO' : 'TEAM ONE'); }
// a player's points, or in FIREBALL BLITZ their health (OUT once they have none)
function fourValue(m, q) { return m.room === 3 ? (q.out ? 'OUT' : '♥ ' + q.hp) : String(q.pts); }
function mmss(c) { const s = Math.ceil(c), mm = Math.floor(s / 60), ss = s % 60; return mm + ':' + (ss < 10 ? '0' : '') + ss; }
function clockText(m) {
  if (m.ot) return m.adv && m.rnd > 0 ? 'ROUND ' + m.rnd + ' · SUDDEN DEATH' : 'SUDDEN DEATH';
  if (m.free) return 'FREE PLAY';
  if (m.adv) {
    const r = m.rnd > 0 ? 'ROUND ' + m.rnd : '';
    return m.rt && m.clock > 0 ? (r ? r + ' · ' : '') + mmss(m.clock) + ' LEFT' : r;
  }
  if (!(m.clock > 0)) return '';
  return mmss(m.clock) + ' LEFT';
}
// the rules with the host's MATCH OPTIONS: "ADVANCED", "BASIC · HOTDOG", ...
function rulesText(m) { return four(m) ? modeText(m) : (m.adv ? 'ADVANCED' : 'BASIC') + (m.hotdog ? ' · HOTDOG \uD83C\uDF2D' : ''); }
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
// four players: one column each (their colour over their name), each row's best lit; careers stay one-on-one
function renderStats4(m) {
  $('sh0').textContent = ''; $('sh1').textContent = '';
  $('carblock').classList.add('hidden');
  const box = $('statrows'); box.textContent = '';
  const s = m.p.map((q) => q.m);
  if (s.some((x) => !x)) { box.appendChild(el('div', 'call-msg', 'No statistics for this match yet.')); return; }
  const grid = el('div', 's4');
  grid.appendChild(el('span', 'k'));
  for (const q of m.p) {
    const h = el('span', 'h', pname(q));
    h.style.setProperty('--pc', colorOf(q));
    grid.appendChild(h);
  }
  const rows = [
    ['THROWS', (x) => x.throws, (x) => x.throws],
    ['ACCURACY', (x) => pct(x.on, x.throws) + '%', (x) => pct(x.on, x.throws)],
    ['HITS', (x) => x.hits, (x) => x.hits],
    ['STRIKES', (x) => x.strikes, (x) => x.strikes],
    ['BLOCKS', (x) => x.blocks, (x) => pct(x.blocks, x.faced)],
    ['CATCHES', (x) => x.catches, (x) => x.catches],
    ['TOP KM/H', (x) => x.top, (x) => x.top],
  ];
  for (const [k, show, rank] of rows) {
    grid.appendChild(el('span', 'k', k));
    const r = s.map(rank), top = Math.max(...r);
    s.forEach((x, i) => grid.appendChild(el('span', 'v' + (top > 0 && r[i] === top ? ' lead' : ''), String(show(x)))));
  }
  box.appendChild(grid);
}
function renderStats(m) {
  if (four(m)) { renderStats4(m); return; }
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
    const hd = [(a.modes || {}).hotdog, (b.modes || {}).hotdog];
    if (hd[0] || hd[1]) {   // the hotdog record next to the overall one
      const h0 = hd[0] || { matches: 0, wins: 0 }, h1 = hd[1] || { matches: 0, wins: 0 };
      cb.appendChild(statRow('HOTDOG \uD83C\uDF2D', h0.matches ? rec(h0) : '—', h1.matches ? rec(h1) : '—',
        pct(h0.wins, h0.matches), pct(h1.wins, h1.matches)));
    }
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
    const sd = sides(m);
    card.style.setProperty('--c0', colorOf(sd ? sd[0] : m.p[0]));
    card.style.setProperty('--c1', colorOf(sd ? sd[1] : m.p[3]));
    const top = el('div', 'card-top');
    const live = LIVE_STATES.includes(m.st);
    top.appendChild(el('span', 'live-dot' + (live ? '' : ' idle'), (live ? '● ' : '') + (STATES[m.st] || 'LIVE')));
    const ck = clockText(m);
    const what = four(m) ? modeText(m) : (m.hotdog ? 'HOTDOG \uD83C\uDF2D · ' : '') + (ck || (m.adv ? 'ADVANCED' : 'BASIC'));
    top.appendChild(el('span', '', m.host + "'S ARENA · " + what + ' · ' + m.fans + (m.fans === 1 ? ' FAN' : ' FANS')));
    card.appendChild(top);
    if (!sd) {   // four for themselves: the four, each with their points (or health)
      const grid = el('div', 'card-four');
      for (const q of m.p) {
        const p = el('div', 'card-p' + (q.out ? ' out' : ''));
        const chip = el('div', 'chip'); chipStyle(chip, q);
        p.appendChild(chip); p.appendChild(el('span', 'n', pname(q))); p.appendChild(el('span', 'fv', fourValue(m, q)));
        grid.appendChild(p);
      }
      card.appendChild(grid);
    } else {
      const row = el('div', 'card-row');
      for (let k = 0; k < 2; ++k) {
        const p = el('div', 'card-p' + (k ? ' right' : ''));
        const chip = el('div', 'chip'); chipStyle(chip, sd[k]);
        p.appendChild(chip); p.appendChild(el('span', 'n' + (sd[k].team != null ? ' team' : ''), label(m, k)));
        row.appendChild(p);
        if (k === 0) row.appendChild(el('div', 'card-score', m.s[0] + ' – ' + m.s[1]));
      }
      card.appendChild(row);
    }
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

// the four players: colour (or flag), name, country, and points (FIREBALL BLITZ: health, or OUT); teams together
function renderFour(m) {
  const box = $('four');
  box.textContent = '';
  const ck = clockText(m);
  box.appendChild(el('div', 'four-mode', (m.room === 3 ? 'HEALTH' : 'POINTS') + (ck ? ' · ' + ck : '')));
  const order = [0, 1, 2, 3];
  if (teams(m)) order.sort((a, b) => (m.p[a].team - m.p[b].team) || (a - b));
  for (const i of order) {
    const q = m.p[i], N = nation(q.nat);
    const row = el('div', 'frow' + (q.out ? ' out' : ''));
    const chip = el('div', 'chip'); chipStyle(chip, q);
    const who = el('div', 'fwho');
    who.appendChild(el('span', 'n', pname(q)));
    if (N) who.appendChild(el('span', 'fnat', N[1]));
    row.appendChild(chip); row.appendChild(who); row.appendChild(el('span', 'fv', fourValue(m, q)));
    box.appendChild(row);
  }
}
function renderMatch(m) {
  const prev = cur;
  cur = m;
  const root = document.documentElement;
  const sd = sides(m);
  root.style.setProperty('--c0', colorOf(sd ? sd[0] : m.p[0]));
  root.style.setProperty('--c1', colorOf(sd ? sd[1] : m.p[3]));
  const live = LIVE_STATES.includes(m.st);
  $('state').textContent = (live ? '● ' : '') + (m.free && m.st === 2 ? 'FREE PLAY' : (STATES[m.st] || 'LIVE')) + ' · ' + rulesText(m);
  $('state').classList.toggle('idle', !live);
  $('clock').textContent = clockText(m);
  // four players: their board; the head-to-head board, hype, cheers, flags and calls only with two sides
  $('four').classList.toggle('hidden', !four(m));
  if (four(m)) renderFour(m);
  for (const id of ['board', 'hypebox', 'cheerbox', 'toolsbox', 'callbox']) $(id).classList.toggle('hidden', !sd);
  if (!sd) {
    if (flagOn) setFlag(false);
    side = -1; call = -1;
  }
  for (let k = 0; k < 2 && sd; ++k) {
    const p = sd[k], N = nation(p.nat);
    chipStyle($('flag' + k), p);
    $('name' + k).textContent = label(m, k);
    $('nat' + k).textContent = N ? N[1] : p.team != null ? teamWord(m, k) : (k ? 'ORANGE END' : 'BLUE END');
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
  if (flagOn) { send({ t: 'flag', side }); if (cur && sides(cur)) $('wave').style.setProperty('--pick', colorOf(sides(cur)[side])); }
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

// ---------------------------------------------------------------- notifications (the bell)
// Web Push: the browser makes an anonymous push address for this device, the relay keeps it with the fan's choices and
// sends a notification when people are playing (someone hosts, or a match starts; no following a player by name, the
// owner 2026-11-21). Off deletes it on the server and in the browser. Choices are kept on the phone too, and sent again
// each time the page opens (so a server that lost them gets them back). Quiet hours are on to start: 10 PM to 8 AM.
const NOTIFY_DEFAULT = { when: 'any', gap: 60, quiet: [22, 8] };
let vapidKey = null, pushSub = null, notifyBusy = false;
let notifyPrefs = Object.assign({}, NOTIFY_DEFAULT);
try {
  const s = JSON.parse((store && store.getItem('notify')) || '{}');
  if (s.when === 'any' || s.when === 'match') notifyPrefs.when = s.when;
  if ([30, 60, 180].includes(s.gap)) notifyPrefs.gap = s.gap;
  if (s.quiet === null || (Array.isArray(s.quiet) && s.quiet.length === 2 && s.quiet[0] !== s.quiet[1]
      && s.quiet.every((h) => Number.isInteger(h) && h >= 0 && h < 24))) notifyPrefs.quiet = s.quiet;
} catch (e) { /* defaults */ }
const isIOS = /iPad|iPhone|iPod/.test(navigator.userAgent) || (navigator.platform === 'MacIntel' && navigator.maxTouchPoints > 1);
const isStandalone = (window.matchMedia && window.matchMedia('(display-mode: standalone)').matches) || navigator.standalone === true;
const pushSupported = () => 'serviceWorker' in navigator && 'PushManager' in window && 'Notification' in window;

function keyBytes(b64) {
  const s = atob(b64.replace(/-/g, '+').replace(/_/g, '/') + '='.repeat((4 - b64.length % 4) % 4));
  return Uint8Array.from(s, (c) => c.charCodeAt(0));
}
function saveNotify() { try { if (store) store.setItem('notify', JSON.stringify(notifyPrefs)); } catch (e) { /* private mode */ } }
function prefsForServer() { return Object.assign({}, notifyPrefs, { tz: -new Date().getTimezoneOffset() }); }
function sendSub() { if (pushSub) send({ t: 'push', sub: pushSub.toJSON(), prefs: prefsForServer() }); }

async function currentSub() {
  if (!pushSupported() || Notification.permission !== 'granted') return null;
  try { const reg = await navigator.serviceWorker.ready; return await reg.pushManager.getSubscription(); } catch (e) { return null; }
}
// the subscription was made for this server key? (a server that had to make a new key: its old subscriptions get nothing)
function sameKey(sub, key) {
  const k = sub && sub.options && sub.options.applicationServerKey;
  if (!k) return true;   // (a browser that doesn't say: as it was)
  const a = new Uint8Array(k), b = keyBytes(key);
  return a.length === b.length && a.every((x, i) => x === b[i]);
}
async function notifyHello(key) {
  vapidKey = key;
  $('bell').classList.toggle('hidden', !key);
  if (!key) { renderNotify(); return; }
  pushSub = await currentSub();
  if (pushSub && !sameKey(pushSub, key)) {   // the server's key changed: the same notifications under the new one
    const old = pushSub;
    send({ t: 'unpush', endpoint: old.endpoint });
    try { await old.unsubscribe(); } catch (e) { /* gone */ }
    try {
      const reg = await navigator.serviceWorker.ready;
      pushSub = await reg.pushManager.subscribe({ userVisibleOnly: true, applicationServerKey: keyBytes(key) });
    } catch (e) {
      pushSub = null;
      toast('Notifications need turning on again: tap the bell.');
    }
  }
  if (pushSub) sendSub();
  renderNotify();
}
function notifyReply(m) {
  if (m.off) return;
  if (!m.ok) { toast('The server could not save that. Try again in a minute.'); }
}

function helpText() {
  if (isIOS && !isStandalone) {
    return 'On iPhone and iPad, notifications work from the home screen: tap <b>Share</b>, then <b>Add to Home Screen</b>. ' +
      'Open The Crowd from your home screen and tap the bell again.';
  }
  if (!pushSupported()) return 'This browser can\'t show notifications from websites. Chrome, Firefox, Edge and Safari can.';
  if (Notification.permission === 'denied') {
    return 'Notifications are blocked for this site. Allow them in your browser\'s settings for this site ' +
      '(on Android: tap the icon left of the address, then <b>Permissions</b>), then turn this on again.';
  }
  return '';
}
function showHelp(text) {
  const h = $('notify-help');
  h.innerHTML = text;   // (only the fixed texts above)
  h.classList.toggle('hidden', !text);
}

function renderNotify() {
  const on = !!pushSub;
  $('bell').classList.toggle('on', on);
  $('bell').setAttribute('aria-label', on ? 'Notifications: on' : 'Notifications: off');
  $('notify-on').checked = on;
  $('notify-status').textContent = !on ? 'Off'
    : notifyPrefs.when === 'match' ? 'On: when a match starts'
    : 'On: when someone hosts or a match starts';
  $('notify-opts').classList.toggle('hidden', !on || !vapidKey);
  for (const r of document.querySelectorAll('input[name="when"]')) r.checked = r.value === notifyPrefs.when;
  $('notify-gap').value = String(notifyPrefs.gap);
  $('notify-quiet').checked = !!notifyPrefs.quiet;
  $('quiet-row').classList.toggle('hidden', !notifyPrefs.quiet);
  const q = notifyPrefs.quiet || [22, 8];
  $('quiet-from').value = String(q[0]);
  $('quiet-to').value = String(q[1]);
}

async function notifyOn() {
  if (notifyBusy) return;
  const help = helpText();
  if (help && (isIOS && !isStandalone || !pushSupported() || Notification.permission === 'denied')) {
    showHelp(help); renderNotify(); return;
  }
  notifyBusy = true;
  try {
    const perm = Notification.permission === 'default' ? await Notification.requestPermission() : Notification.permission;
    if (perm !== 'granted') { showHelp(helpText() || 'Notifications were not allowed. Tap the switch to ask again.'); return; }
    const reg = await navigator.serviceWorker.ready;
    pushSub = (await reg.pushManager.getSubscription()) ||
      await reg.pushManager.subscribe({ userVisibleOnly: true, applicationServerKey: keyBytes(vapidKey) });
    showHelp('');
    sendSub();
    if (!connected) toast('Notifications are on. They\'ll be saved when the connection is back.');
    reg.showNotification('Notifications are on', {
      body: 'We\'ll let you know when people are playing Project Arena. Change or turn them off with the bell.',
      icon: 'icon-192.png', tag: 'setup',
    });
  } catch (e) {
    pushSub = null;
    showHelp('Notifications could not be turned on here (' + (e && e.name ? e.name : 'error') + '). Try again, or another browser.');
  } finally { notifyBusy = false; renderNotify(); }
}
async function notifyOff() {
  if (notifyBusy) return;
  notifyBusy = true;
  try {
    const sub = pushSub || await currentSub();
    // the server deletes it now; if the connection is down, the push service tells the server it's gone next time
    if (sub) { send({ t: 'unpush', endpoint: sub.endpoint }); try { await sub.unsubscribe(); } catch (e) { /* gone */ } }
    pushSub = null;
    toast('Notifications are off.');
  } finally { notifyBusy = false; renderNotify(); }
}
function notifyChanged() {
  saveNotify();
  sendSub();
  renderNotify();
  if (pushSub) toast('Saved.');
}

function openNotify() { showHelp(pushSub || !vapidKey ? '' : helpText()); renderNotify(); $('notify').classList.remove('hidden'); }
function closeNotify() { $('notify').classList.add('hidden'); }
(function setupNotify() {
  for (const id of ['quiet-from', 'quiet-to']) {
    for (let h = 0; h < 24; ++h) {
      const o = document.createElement('option');
      o.value = String(h);
      o.textContent = (h % 12 || 12) + (h < 12 ? ' AM' : ' PM');
      $(id).appendChild(o);
    }
  }
  $('bell').onclick = openNotify;
  $('notify-close').onclick = closeNotify;
  $('notify').addEventListener('click', (e) => { if (e.target === $('notify')) closeNotify(); });
  document.addEventListener('keydown', (e) => { if (e.key === 'Escape') closeNotify(); });
  $('notify-on').onchange = () => { if ($('notify-on').checked) notifyOn(); else notifyOff(); };
  $('notify-off').onclick = notifyOff;
  for (const r of document.querySelectorAll('input[name="when"]')) {
    r.onchange = () => { notifyPrefs.when = r.value; notifyChanged(); };
  }
  $('notify-gap').onchange = () => { notifyPrefs.gap = parseInt($('notify-gap').value, 10) || 60; notifyChanged(); };
  $('notify-quiet').onchange = () => { notifyPrefs.quiet = $('notify-quiet').checked ? [22, 8] : null; notifyChanged(); };
  const quiet = () => {
    const a = parseInt($('quiet-from').value, 10), b = parseInt($('quiet-to').value, 10);
    if (a === b) { toast('Pick two different hours.'); renderNotify(); return; }
    notifyPrefs.quiet = [a, b];
    notifyChanged();
  };
  $('quiet-from').onchange = quiet;
  $('quiet-to').onchange = quiet;
})();

// a notification was tapped: show that match, or the notification settings
let wanted = (() => { const q = new URLSearchParams(location.search); return { id: q.get('watch'), settings: q.has('settings') }; })();
if (wanted.id || wanted.settings) history.replaceState(null, '', location.pathname);
function openWanted() {
  if (wanted.settings && !$('bell').classList.contains('hidden')) { wanted.settings = false; openNotify(); }
  if (wanted.id) {
    const id = wanted.id;
    wanted.id = null;
    if (matches.some((m) => m.id === id)) watch(id); else toast('That match has ended. Here is what\'s live now.');
  }
}
if ('serviceWorker' in navigator) {
  navigator.serviceWorker.addEventListener('message', (e) => {
    const d = e.data || {};
    if (d.t !== 'open') return;
    if (d.settings) { openNotify(); return; }
    if (d.id) { if (watching) leave(); wanted.id = d.id; openWanted(); }
  });
}

if ('serviceWorker' in navigator) navigator.serviceWorker.register('sw.js').catch(() => {});
renderList();
connect();
