// SPDX-License-Identifier: MIT
// Squatch Tuner in the browser: the tuner core compiled to WebAssembly, running in an
// AudioWorklet on the audio thread, drawn by the same page as the Pi bench (tuner-view.js).
//
// Input is getUserMedia with echo cancellation, noise suppression and automatic gain all OFF:
// each of them alters the signal a tuner is measuring. Nothing is recorded or sent anywhere;
// the only network requests are this page's own files.
import { $, setText, startView, storage } from './tuner-view.js';

const NAMES = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B'];
// tuner_frame() layout (hosts/wasm/tuner_wasm.cpp, enum Field).
const F = { valid: 0, midi: 1, oct: 2, cents: 3, fast: 4, hz: 5, shz: 6, unc: 7, parts: 8, B: 9, a4: 10, lvl: 11, pk: 12 };
const FIELDS = 13;
const RATE_HZ = 30;          // readings per second, as on the Pi
const MIN_HZ = 38;           // bass E1 and up
const PRIVACY = 'Audio never leaves your device: the tuner runs in this page.';

const wasmBytes = fetch(new URL('tuner.wasm', import.meta.url)).then((r) => {
  if (!r.ok) throw new Error(`tuner.wasm: HTTP ${r.status}`);
  return r.arrayBuffer();
});

const audio = { ctx: null, stream: null, source: null, node: null, feed: null, moduleAdded: false, busy: false };
const prefs = {
  a4: Number(storage('squatch-tuner-a4')) || 440,
  device: storage('squatch-tuner-input') || '',
  channel: Number(storage('squatch-tuner-channel')) || 0,
};
let lastReading = null;

function toReading(f, samples, rate) {
  const midi = Math.round(f[F.midi]);
  return {
    valid: f[F.valid] > 0.5, midi, note: NAMES[((midi % 12) + 12) % 12], oct: Math.round(f[F.oct]),
    cents: f[F.cents], fast: f[F.fast], hz: f[F.hz], shz: f[F.shz], unc: f[F.unc],
    parts: Math.round(f[F.parts]), B: f[F.B], a4: f[F.a4], lvl: f[F.lvl], pk: f[F.pk], samples, rate,
  };
}

// ---------------------------------------------------------------- audio ----

function constraints(deviceId) {
  const off = { echoCancellation: false, noiseSuppression: false, autoGainControl: false };
  return deviceId ? { ...off, deviceId: { exact: deviceId } } : off;
}

async function openNode(ctx) {
  if (!audio.moduleAdded) {
    await ctx.audioWorklet.addModule(new URL('worklet.js', import.meta.url));
    audio.moduleAdded = true;
  }
  const node = new AudioWorkletNode(ctx, 'squatch-tuner', {
    numberOfInputs: 1, numberOfOutputs: 1, outputChannelCount: [1],
    channelCount: 2, channelCountMode: 'explicit', channelInterpretation: 'discrete',
    processorOptions: { wasm: await wasmBytes, a4: prefs.a4, channel: prefs.channel, minHz: MIN_HZ, rateHz: RATE_HZ },
  });
  node.port.onmessage = (e) => onMessage(e.data);
  return node;
}

function onMessage(data) {
  if (data && data.type === 'frame' && data.frame.length === FIELDS) {
    lastReading = toReading(data.frame, data.samples, data.rate);
    audio.feed.push(lastReading);
  } else if (data && data.type === 'error') {
    fail(`The tuner could not start: ${data.message}`);
  }
}

// The AudioContext is made inside the click, before any await, so the browser lets it start.
async function start(deviceId) {
  if (audio.busy) return;
  audio.busy = true;
  audio.ctx ??= new AudioContext({ latencyHint: 'interactive' });
  const resumed = audio.ctx.resume();
  try {
    await openInput(deviceId);
    await resumed;
    running(true);
  } catch (err) {
    stop();
    fail(explain(err));
  } finally {
    audio.busy = false;
  }
}

async function openInput(deviceId) {
  closeInput();
  audio.stream = await navigator.mediaDevices.getUserMedia({ audio: constraints(deviceId) });
  audio.node = await openNode(audio.ctx);
  audio.source = audio.ctx.createMediaStreamSource(audio.stream);
  audio.source.connect(audio.node);
  audio.node.connect(audio.ctx.destination);  // pulls the graph; the output is silent
  await listInputs();
  showTrack(audio.stream.getAudioTracks()[0]);
}

function closeInput() {
  audio.source?.disconnect();
  audio.node?.disconnect();
  audio.node?.port.close();
  audio.stream?.getTracks().forEach((t) => t.stop());
  audio.source = audio.node = audio.stream = null;
}

function stop() {
  closeInput();
  audio.ctx?.suspend();
  audio.feed.offline();
  running(false);
}

function explain(err) {
  const name = err && err.name;
  if (name === 'NotAllowedError') return 'Microphone access was refused. Allow it for this page, then press Start.';
  if (name === 'NotFoundError' || name === 'OverconstrainedError') return 'No audio input was found. Plug one in, then press Start.';
  if (!window.isSecureContext) return 'The microphone needs HTTPS: open this page over https://.';
  return `The tuner could not start: ${err && err.message ? err.message : err}`;
}

// ---------------------------------------------------------------- input picker ----

async function listInputs() {
  const devices = await navigator.mediaDevices.enumerateDevices();
  const inputs = devices.filter((d) => d.kind === 'audioinput');
  const select = $('input-select');
  const current = audio.stream?.getAudioTracks()[0]?.getSettings().deviceId ?? prefs.device;
  select.replaceChildren(...inputs.map((d, i) => new Option(d.label || `Input ${i + 1}`, d.deviceId, false, d.deviceId === current)));
  select.disabled = inputs.length === 0;
}

function showTrack(track) {
  if (!track) return;
  const settings = track.getSettings();
  $('channel-pick').hidden = !(settings.channelCount >= 2);
  if (settings.deviceId) storage('squatch-tuner-input', (prefs.device = settings.deviceId));
}

function wireSource() {
  $('start').addEventListener('click', () => (audio.node ? stop() : start(prefs.device)));
  $('input-select').addEventListener('change', (e) => {
    prefs.device = e.target.value;
    storage('squatch-tuner-input', prefs.device);
    if (audio.node) start(prefs.device);
  });
  $('channel-select').value = String(prefs.channel);
  $('channel-select').addEventListener('change', (e) => {
    prefs.channel = Number(e.target.value);
    storage('squatch-tuner-channel', String(prefs.channel));
    audio.node?.port.postMessage({ channel: prefs.channel });
  });
  navigator.mediaDevices?.addEventListener?.('devicechange', () => audio.stream && listInputs());
}

function running(on) {
  $('start').textContent = on ? 'Stop' : 'Start tuner';
  $('start').classList.toggle('on', on);
  setText('source-msg', on ? `Listening. ${PRIVACY}` : PRIVACY);
  if (audio.ctx) setText('audio', `${(audio.ctx.sampleRate / 1000).toFixed(1).replace(/\.0$/, '')} kHz · 128-frame blocks`);
}

function fail(message) {
  setText('source-msg', message);
  $('source-msg').classList.add('warn');
  setTimeout(() => $('source-msg').classList.remove('warn'), 6000);
}

// ---------------------------------------------------------------- cost ----

// What the core costs on this device: one second of a synthetic A2 through a second instance,
// on the main thread, timed. Shown in the footer as a share of one core at 48 kHz.
async function measureCost() {
  const { instance } = await WebAssembly.instantiate(await wasmBytes, {});
  const e = instance.exports;
  e._initialize();
  e.tuner_configure(48000, MIN_HZ);
  const x = new Float32Array(48000);
  for (let i = 0; i < x.length; i++) {
    const p = (2 * Math.PI * 110 * i) / 48000;
    x[i] = 0.3 * Math.sin(p) + 0.15 * Math.sin(2 * p) + 0.1 * Math.sin(3 * p);
  }
  const input = new Float32Array(e.memory.buffer, e.tuner_input(), 128);
  const t0 = performance.now();
  for (let i = 0; i + 128 <= x.length; i += 128) {
    input.set(x.subarray(i, i + 128));
    e.tuner_process(128);
  }
  const ms = performance.now() - t0;
  setText('dsp', `DSP ${((ms / 1000) * 100).toFixed(1)}% of one core here`);
}

// ---------------------------------------------------------------- start ----

// Served from a local files host (files.<domain>/p/<id>/), the page links back to the index
// beside it (home.<domain>). Anywhere else, GitHub Pages included, that link stays hidden.
function wireHome() {
  const labels = location.hostname.split('.');
  if (location.protocol !== 'https:' || labels[0] !== 'files' || labels.length < 3) return;
  $('home-back').href = `https://home.${labels.slice(1).join('.')}/`;
  $('home-back').hidden = false;
}

startView({
  status: { offline: 'press Start' },
  strobeMsg: { offline: 'press Start and allow the microphone' },
  start(feed) {
    audio.feed = feed;
    wireHome();
    wireSource();
    $('a4-range').value = String(prefs.a4);
    setText('a4-out', String(prefs.a4));
    if (!navigator.mediaDevices?.getUserMedia) fail(explain(new Error('this browser has no microphone access here')));
    setTimeout(() => measureCost().catch(() => {}), 800);
  },
  sendA4(hz) {
    prefs.a4 = hz;
    storage('squatch-tuner-a4', String(hz));
    audio.node?.port.postMessage({ a4: hz });
  },
});

// For tests and the console: the newest reading, as the page received it.
window.squatchTuner = { reading: () => lastReading };
