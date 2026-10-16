// SPDX-License-Identifier: MIT
// Squatch Tuner page, the display half: the note, the cents, the needle, the in-tune seals, the
// strobe, the level meter, the reference pitch and the theme. It knows nothing about where the
// readings come from. A host module supplies them:
//
//   tuner.js   the Pi bench: Server-Sent Events from serve.py (hosts/web)
//   demo.js    the browser demo: the tuner compiled to WebAssembly in an AudioWorklet (hosts/wasm)
//
// startView(host) takes:
//   start(feed)       begin; call feed.push(reading) for each reading (about 30 a second) and
//                     feed.offline() when the source goes away
//   sendA4(hz)        the player moved the reference pitch (430..450); the source applies it and
//                     the readings then carry it back as `a4`
//   renderFooter(r)   optional: the host's own footer, 4 times a second (r is null when offline)
//   onFrame(ts, r, seq)  optional: called every animation frame after drawing
//   status, strobeMsg optional: texts for the views 'offline', 'nosignal' and 'listening'
//
// A reading is the JSON the JACK host prints (hosts/jack/tuner_live.cpp): valid, note ("C#"),
// oct, midi, cents (steady), fast (cents of the fast reading), hz, shz, unc, parts, a4,
// lvl and pk (dBFS), and samples and rate: where in the source's audio it was read. Everything
// moving is drawn every animation frame, eased toward the newest reading in between. Whether a
// let-go note stays, and the number it holds, follow the audio (hold.js), not the frame clock:
// every reading enters the hold as it arrives.
import { Hold } from './hold.js';
import { Strobe } from './strobe.js';

export const $ = (id) => document.getElementById(id);
const GATE_DB = -70;         // the core's gate: below this nothing is read
const NEEDLE_TAU = 0.09;     // s: needle and number easing
const STROBE_TAU = 0.05;     // s: strobe speed easing

const state = {
  reading: null, seq: 0, online: false,
  lastValid: null, hold: new Hold(),
  needle: 0, strobeCents: 0, prevTs: 0,
  texts: new Map(), a4HeldUntil: 0,
};

const STATUS = { offline: 'tuner offline', nosignal: 'no signal', listening: 'listening…' };
const STROBE_MSG = { offline: 'tuner offline', nosignal: 'no signal · play a string', listening: 'listening…' };
let status = STATUS, strobeMsg = STROBE_MSG;

/** Set an element's text only when it changed (the page updates 60 times a second). */
export function setText(id, text) {
  if (state.texts.get(id) === text) return;
  state.texts.set(id, text);
  $(id).textContent = text;
}

// ---------------------------------------------------------------- view ----

function viewOf(r) {
  if (!state.online || !r) return 'offline';
  if (r.valid) return 'note';
  if (state.lastValid && state.hold.holding(r)) return 'note';
  return r.lvl < GATE_DB ? 'nosignal' : 'listening';
}

export function formatCents(c) {
  const v = Math.round(c * 10) / 10;
  if (v === 0) return '0.0';
  return (v > 0 ? '+' : '−') + Math.abs(v).toFixed(1);
}

function renderNote(view, shown, fading) {
  const stage = $('stage');
  stage.dataset.view = view;
  stage.classList.toggle('fading', fading);
  setText('status', view === 'note' ? (fading ? 'let go' : `${shown.shz.toFixed(2)} Hz`) : status[view]);
  setText('strobe-msg', strobeMsg[view] ?? '');
  if (!shown) {
    setText('note-name', '·');
    setText('note-acc', '');
    setText('note-oct', '');
    return;
  }
  setText('note-name', shown.note[0]);
  setText('note-acc', shown.note.length > 1 ? '♯' : '');
  setText('note-oct', String(shown.oct));
}

function renderCents(shown, view) {
  const c = shown ? state.needle : 0;
  const known = hasCents(shown);
  setText('cents-num', known ? formatCents(shown.cents) : '--.-');
  const abs = known ? Math.abs(shown.cents) : 99;
  const live = view === 'note' && shown && shown === state.reading;
  setText('direction', direction(known ? shown : null, abs));
  seal('seal-05', live && abs <= 0.5);
  seal('seal-01', live && abs <= 0.1);
  $('stage').classList.toggle('in-tune', Boolean(live && abs <= 0.5));
  const x = 500 + Math.max(-50, Math.min(50, c)) * 9;
  $('needle').setAttribute('transform', `translate(${x.toFixed(1)} 0)`);
}

// A held note may have no number (hold.js): too little of it, or a peg still turning.
function hasCents(shown) {
  return Boolean(shown) && !Number.isNaN(shown.cents);
}

function direction(shown, abs) {
  if (!shown) return ' ';
  if (abs < 0.1) return 'in tune';
  const sharp = shown.cents > 0;
  if (abs <= 0.5) return sharp ? 'a hair sharp' : 'a hair flat';
  return sharp ? 'sharp — tune down' : 'flat — tune up';
}

function seal(id, on) {
  const el = $(id);
  if (el.classList.contains('on') !== on) el.classList.toggle('on', on);
}

function renderLevel(r) {
  const db = r ? r.pk : -120;
  const pct = Math.max(0, Math.min(100, ((db + 60) / 60) * 100));
  $('meter-fill').style.width = `${pct.toFixed(1)}%`;
  $('meter-fill').classList.toggle('hot', db > -1);
  setText('level-num', r ? `${db > -99 ? db.toFixed(0) : '−∞'} dBFS peak` : '–');
}

function renderDetail(r) {
  if (!r) return;
  const parts = `${r.parts} partial${r.parts === 1 ? '' : 's'}`;
  setText('detail', r.valid ? `${r.hz.toFixed(3)} Hz now · ${parts} · ±${r.unc.toFixed(2)}¢` : ' ');
  syncA4(r.a4);
}

// ---------------------------------------------------------------- frame ----

function ease(current, target, dt, tau) {
  return current + (target - current) * (1 - Math.exp(-dt / tau));
}

// While the tuner reads, the newest reading; after it lets go, the hold's summary of the note.
function shownReading(view, live) {
  if (view !== 'note') return null;
  return live ? state.lastValid : state.hold.held(state.lastValid);
}

function frame(ts, strobe, host) {
  const dt = Math.min(0.1, state.prevTs ? (ts - state.prevTs) / 1000 : 0);
  state.prevTs = ts;
  const r = state.reading;
  const view = viewOf(r);
  const live = Boolean(r && r.valid && state.online);
  const shown = shownReading(view, live);
  if (hasCents(shown)) state.needle = ease(state.needle, shown.cents, dt, NEEDLE_TAU);
  state.strobeCents = ease(state.strobeCents, live ? r.fast : 0, dt, STROBE_TAU);
  renderNote(view, shown, view === 'note' && !live);
  renderCents(shown, view);
  renderLevel(state.online ? r : null);
  strobe.draw(dt, state.strobeCents, live);
  host.onFrame?.(ts, r, state.seq);
}

// ---------------------------------------------------------------- reference pitch ----

function syncA4(sourceA4) {
  if (performance.now() < state.a4HeldUntil || sourceA4 == null) return;
  const v = Math.round(sourceA4);
  if ($('a4-range').value !== String(v)) $('a4-range').value = String(v);
  setText('a4-out', String(v));
}

let a4Timer = 0;
function setA4(hz, host) {
  const v = Math.max(430, Math.min(450, Math.round(hz)));
  $('a4-range').value = String(v);
  setText('a4-out', String(v));
  state.a4HeldUntil = performance.now() + 1500;  // the readings catch up once the source has it
  clearTimeout(a4Timer);
  a4Timer = setTimeout(() => host.sendA4(v), 120);
}

function wireControls(host) {
  $('a4-range').addEventListener('input', (e) => setA4(Number(e.target.value), host));
  $('a4-down').addEventListener('click', () => setA4(Number($('a4-range').value) - 1, host));
  $('a4-up').addEventListener('click', () => setA4(Number($('a4-range').value) + 1, host));
}

// ---------------------------------------------------------------- theme, grain ----

const THEMES = ['auto', 'dark', 'light'];

/** localStorage, or nothing: private mode and blocked storage must not break the page. */
export function storage(key, value) {
  try {
    if (value === undefined) return localStorage.getItem(key);
    localStorage.setItem(key, value);
  } catch { /* the page works without it */ }
  return null;
}

function applyTheme(theme, strobe) {
  const root = document.documentElement;
  root.dataset.theme = theme;
  // The Squatch Ink tokens (tokens.css) follow data-sq-mode; "auto" leaves it to the system setting.
  if (theme === 'auto') delete root.dataset.sqMode;
  else root.dataset.sqMode = theme;
  setText('theme-label', theme);
  $('theme').setAttribute('aria-label', `Colour theme: ${theme}`);
  const css = getComputedStyle(document.documentElement);
  const token = (n) => css.getPropertyValue(n).trim();
  strobe.setColors({ ink: token('--ink'), row: token('--paper-2'), label: token('--ink-2') });
}

function wireTheme(strobe) {
  const asked = new URLSearchParams(location.search).get('theme');
  let theme = THEMES.includes(asked) ? asked : storage('squatch-tuner-theme') || 'auto';
  if (!THEMES.includes(theme)) theme = 'auto';
  applyTheme(theme, strobe);
  $('theme').addEventListener('click', () => {
    theme = THEMES[(THEMES.indexOf(theme) + 1) % THEMES.length];
    storage('squatch-tuner-theme', theme);
    applyTheme(theme, strobe);
  });
  matchMedia('(prefers-color-scheme: dark)').addEventListener('change', () => applyTheme(theme, strobe));
}

// Paper grain: one 256 px noise tile, made here so the page needs no image files.
function makeGrain() {
  const c = document.createElement('canvas');
  c.width = c.height = 256;
  const g = c.getContext('2d'), img = g.createImageData(256, 256);
  for (let i = 0; i < img.data.length; i += 4) {
    const v = 128 + (Math.random() - 0.5) * 120;
    img.data[i] = img.data[i + 1] = img.data[i + 2] = v;
    img.data[i + 3] = 255;
  }
  g.putImageData(img, 0, 0);
  document.body.style.setProperty('--grain', `url(${c.toDataURL('image/png')})`);
}

// ---------------------------------------------------------------- start ----

const feed = {
  push(r) {
    state.hold.add(r);
    if (r && r.valid) state.lastValid = r;
    state.reading = r;
    state.seq += 1;
    state.online = true;
  },
  offline() {
    state.online = false;
    state.hold.forget();  // what comes back is a new stream (hold.js)
    state.lastValid = null;
  },
};

export function startView(host) {
  status = { ...STATUS, ...host.status };
  strobeMsg = { ...STROBE_MSG, ...host.strobeMsg };
  const strobe = new Strobe($('strobe'));
  makeGrain();
  wireTheme(strobe);
  wireControls(host);
  host.start(feed);
  setInterval(() => {
    const r = state.online ? state.reading : null;
    renderDetail(r);
    host.renderFooter?.(r);
  }, 250);
  const loop = (ts) => {
    frame(ts, strobe, host);
    requestAnimationFrame(loop);
  };
  requestAnimationFrame(loop);
}
