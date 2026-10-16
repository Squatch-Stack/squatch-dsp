// SPDX-License-Identifier: MIT
// Regenerates the stored browser renders the parity tests compare against (test_parity_*.cpp).
//
//   node tests/golden-parity/gen.mjs RIG_DIR
//
// RIG_DIR is a squatch-rig checkout (read only). The renders come from the SAME code the browser
// runs: web/worklets/dsp/reverb.js, namhead.js with nam.wasm, and tools/render/graph.mjs (the cab
// through the offline WebAudio emulation). Nothing here plays audio. Files (raw little-endian
// float32, N = 24064 samples per channel):
//   hall-<case>.f32   inL, inR, outL, outR
//   cab-<case>.f32    ir (4096 taps), in, out
//   nam-<case>.f32    in, out
//   gate-<case>.f32   inL, inR, outL, outR (a mono case has inR = inL and runs the kernel with one channel)
//   lim-<case>.f32    inL, inR, outL, outR (same)
//   od-<case>.f32     in, out
// The case tables below are copied by hand into the tests; change both together.
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const rig = process.argv[2];
if (!rig) { console.error('usage: node gen.mjs RIG_DIR'); process.exit(2); }
const imp = (p) => import(pathToFileURL(path.join(rig, p)).href);
const SR = 48000, N = 24064, Q = 128;

function lcg(seed) { let s = seed >>> 0; return () => { s = (Math.imul(s, 1664525) + 1013904223) >>> 0; return s / 2147483648 - 1; }; }
// Guitar-ish test signal: decaying plucks of a few partials over a little noise, `lead` samples of silence first.
function signal(seed, lead, amp = 0.5) {
  const r = lcg(seed), x = new Float32Array(N);
  const plucks = [[0, 110], [5200, 146.8], [9800, 196], [14500, 82.4]];
  for (const [t0, f] of plucks) {
    for (let i = lead + t0; i < N; i++) {
      const t = (i - lead - t0) / SR, env = Math.exp(-t * 6);
      if (env < 1e-4) break;
      x[i] += amp * env * (0.6 * Math.sin(2 * Math.PI * f * t) + 0.3 * Math.sin(2 * Math.PI * 2 * f * t) + 0.15 * Math.sin(2 * Math.PI * 3.01 * f * t));
    }
  }
  for (let i = lead; i < N; i++) x[i] += 0.01 * r();
  return x;
}
const write = (name, ...arrs) => {
  const total = arrs.reduce((n, a) => n + a.length, 0), out = new Float32Array(total);
  let o = 0; for (const a of arrs) { out.set(a, o); o += a.length; }
  fs.writeFileSync(path.join(here, name), Buffer.from(out.buffer));
  console.log(name, total);
};

// ---- Hall: browser Reverb, both channels --------------------------------------------------------
const { Reverb } = await imp('web/worklets/dsp/reverb.js');
const HALL = {
  A: { params: {}, stereoIn: false },
  B: { params: { decay: 1.2, size: 1.6, damp: 3000, predelay: 5, mix: 0.5, width: 0.4 }, stereoIn: false },
  C: { params: { decay: 4, size: 0.3, damp: 12000, predelay: 40, mix: 1, width: 0 }, stereoIn: true },
};
for (const [id, c] of Object.entries(HALL)) {
  const inL = signal(11, 0), inR = c.stereoIn ? signal(23, 0, 0.4) : inL;
  const k = new Reverb(SR);
  for (const [n, v] of Object.entries(c.params)) k.set(n, v);
  const L = new Float32Array(N), R = new Float32Array(N);
  for (let p = 0; p < N; p += Q) k.process([inL.subarray(p, p + Q), inR.subarray(p, p + Q)], [L.subarray(p, p + Q), R.subarray(p, p + Q)], Q);
  write(`hall-${id}.f32`, inL, inR, L, R);
}

// ---- Cab: browser Cab through the offline renderer (synthetic IR, 4096 taps, low/high cut, level) ----
const { renderDoc } = await imp('tools/render/graph.mjs');
const { synthCabIR } = await imp('web/js/audio/ir.js');
const wasmBytes = fs.readFileSync(path.join(rig, 'web/wasm/nam.wasm'));
const CAB = {
  default: { voice: 0, mic: 0.5, lowcut: 70, highcut: 9000, level: 0 },
  B: { voice: 1, mic: 0.8, lowcut: 150, highcut: 5000, level: -6 },
  open: { voice: 0, mic: 0.5, lowcut: 20, highcut: 16000, level: 0 },
};
for (const [id, params] of Object.entries(CAB)) {
  const lead = 9600, x = signal(31, lead);
  const doc = { nodes: { i: { type: 'input', params: { gain: 0 } }, c: { type: 'cab', params }, o: { type: 'output', params: { volume: 0 } } },
                edges: { a: { from: ['i', 0], to: ['c', 0] }, b: { from: ['c', 0], to: ['o', 0] } }, models: {} };
  const r = renderDoc({ doc, di: x, models: {}, wasmBytes, seconds: N / SR, sampleRate: SR });
  write(`cab-${id}.f32`, synthCabIR(SR, params.voice, params.mic), x, r.channels[0].subarray(0, N));
}

// ---- NAM: browser NamHead + nam.wasm, after 0.5 s of silence so the de-zipper has arrived -------------
const { NamHead } = await imp('web/worklets/dsp/namhead.js');
const { instantiateNam } = await imp('web/worklets/nam-wasm.js');
const nam = instantiateNam(new WebAssembly.Module(wasmBytes));
const NAMCASES = {
  A1_default: { model: 'namcore-example-A1.nam', p: {} },
  A1_gain_bass: { model: 'namcore-example-A1.nam', p: { input: 6, output: -3, bass: -6 } },
  A1_nodc: { model: 'namcore-example-A1.nam', p: { dcblock: 0 } },
  A2_default: { model: 'namcore-example-A2.nam', p: {} },
  A2_nonorm: { model: 'namcore-example-A2.nam', p: { normalize: 0, dcblock: 0, bass: -12 } },
};
for (const [id, c] of Object.entries(NAMCASES)) {
  const x = signal(41, 0, 0.3), L = new Float32Array(N), warm = new Float32Array(Q), sink = new Float32Array(Q);
  const head = new NamHead(SR, { ...c.p }, Q);
  head.setModel(nam.create(new Uint8Array(fs.readFileSync(path.join(rig, 'web/models', c.model))), SR, Q));
  for (let i = 0; i < Math.ceil(0.5 * SR / Q); i++) head.process(warm, sink, Q);
  for (let p = 0; p < N; p += Q) head.process(x.subarray(p, p + Q), L.subarray(p, p + Q), Q);
  head.setModel(null)?.destroy();
  write(`nam-${id}.f32`, x, L);
}

// ---- Gate: browser NoiseGate, after 0.5 s of silence (its gain starts at 0, the C++ block's at the floor) ----
const { NoiseGate } = await imp('web/worklets/dsp/gate.js');
// Short plucks (about 25 ms) with long gaps over a faint hiss: the gate's 20 ms envelope fall, hold and release
// need 100-300 ms to close, and the render is half a second.
function gateSignal(seed, hiss, amp = 0.5) {
  const r = lcg(seed), x = new Float32Array(N);
  for (const [t0, f] of [[0, 110], [14000, 146.8], [21000, 196]]) {
    for (let i = t0; i < N; i++) {
      const t = (i - t0) / SR, env = Math.exp(-t * 120);
      if (env < 1e-4) break;
      x[i] += amp * env * (0.6 * Math.sin(2 * Math.PI * f * t) + 0.3 * Math.sin(2 * Math.PI * 2 * f * t) + 0.15 * Math.sin(2 * Math.PI * 3.01 * f * t));
    }
  }
  for (let i = 0; i < N; i++) x[i] += hiss * r();
  return x;
}
const GATE = {
  default: { params: {}, hiss: 0.0005, stereoIn: false },
  A: { params: { threshold: -45, hysteresis: 3, attack: 5, hold: 100, release: 300, range: -30 }, hiss: 0.002, stereoIn: false },
  B: { params: { threshold: -35, hysteresis: 0, attack: 0.1, hold: 0, release: 5, range: -90 }, hiss: 0.002, stereoIn: false },
  C: { params: { threshold: -50, hysteresis: 12, attack: 50, hold: 150, release: 1000, range: -10 }, hiss: 0.001, stereoIn: false },
  stereo: { params: { threshold: -40, hysteresis: 6, attack: 2, hold: 60, release: 150, range: -60 }, hiss: 0.001, stereoIn: true },
};
for (const [id, c] of Object.entries(GATE)) {
  const inL = gateSignal(51, c.hiss), inR = c.stereoIn ? gateSignal(52, c.hiss, 0.15) : inL;
  const k = new NoiseGate(SR);
  for (const [n, v] of Object.entries(c.params)) k.set(n, v);
  const chans = c.stereoIn ? 2 : 1, z = new Float32Array(Q);
  for (let i = 0; i < Math.ceil(0.5 * SR / Q); i++) k.process(Array(chans).fill(z), Array.from({ length: chans }, () => new Float32Array(Q)), Q);
  const L = new Float32Array(N), R = new Float32Array(N);
  for (let p = 0; p < N; p += Q) {
    const ins = [inL.subarray(p, p + Q)], outs = [L.subarray(p, p + Q)];
    if (c.stereoIn) ins.push(inR.subarray(p, p + Q)), outs.push(R.subarray(p, p + Q));
    k.process(ins, outs, Q);
  }
  write(`gate-${id}.f32`, inL, inR, L, c.stereoIn ? R : L);
}

// ---- Limiter: browser Limiter (sample-peak, 2 ms lookahead; the C++ block's test-only sample-peak mode) ----
const { Limiter } = await imp('web/worklets/dsp/limiter.js');
const LIM = {
  default: { params: {}, amp: 1.2, stereoIn: false },
  A: { params: { gain: 12, ceiling: -3, release: 120 }, amp: 0.5, stereoIn: false },
  B: { params: { gain: 18, ceiling: -1, release: 10 }, amp: 0.5, stereoIn: false },
  C: { params: { gain: 6, ceiling: -12, release: 1000 }, amp: 0.8, stereoIn: false },
  stereo: { params: { gain: 9, ceiling: -6, release: 60 }, amp: 0.5, stereoIn: true },
};
for (const [id, c] of Object.entries(LIM)) {
  const inL = signal(61, 0, c.amp), inR = c.stereoIn ? signal(62, 0, c.amp * 0.4) : inL;
  const k = new Limiter(SR);
  for (const [n, v] of Object.entries(c.params)) k.set(n, v);
  const L = new Float32Array(N), R = new Float32Array(N);
  for (let p = 0; p < N; p += Q) {
    const ins = [inL.subarray(p, p + Q)], outs = [L.subarray(p, p + Q)];
    if (c.stereoIn) ins.push(inR.subarray(p, p + Q)), outs.push(R.subarray(p, p + Q));
    k.process(ins, outs, Q);
  }
  write(`lim-${id}.f32`, inL, inR, L, c.stereoIn ? R : L);
}
{ // a 12 kHz sine at 45 degrees: its samples peak at 0.707 of its true peak, so a sample-peak limiter overshoots
  const x = new Float32Array(N);
  for (let i = 0; i < N; i++) x[i] = 0.9 * Math.sin(2 * Math.PI * 12000 * i / SR + Math.PI / 4);
  const k = new Limiter(SR);
  k.set('ceiling', -3);
  const y = new Float32Array(N);
  for (let p = 0; p < N; p += Q) k.process([x.subarray(p, p + Q)], [y.subarray(p, p + Q)], Q);
  write('lim-tone12k.f32', x, x, y, y);
}

// ---- Trail OD: browser Overdrive (4x oversampled, 129-tap Kaiser FIR pair, 32 samples of delay) ----
const { Overdrive } = await imp('web/worklets/dsp/overdrive.js');
const OD = {
  default: { p: {}, amp: 0.5 },
  A: { p: { drive: 0.2, tone: 0.3, level: -6 }, amp: 0.5 },
  B: { p: { drive: 0.9, tone: 0.7, level: -10 }, amp: 0.5 },
  C: { p: { drive: 1, tone: 1, level: 12 }, amp: 1 },
  D: { p: { drive: 0, tone: 0, level: -30 }, amp: 0.25 },
};
const odRun = (p, x) => {
  const k = new Overdrive(SR);
  for (const [n, v] of Object.entries(p)) k.set(n, v);
  const y = new Float32Array(N);
  for (let q = 0; q < N; q += Q) k.process([x.subarray(q, q + Q)], [y.subarray(q, q + Q)], Q);
  return y;
};
for (const [id, c] of Object.entries(OD)) {
  const x = signal(71, 0, c.amp);
  write(`od-${id}.f32`, x, odRun(c.p, x));
}
{ // a bin-centred sine (550 cycles in 8192 samples = 3222.66 Hz) at -12 dBFS, for the aliasing measure
  const x = new Float32Array(N);
  for (let i = 0; i < N; i++) x[i] = Math.pow(10, -12 / 20) * Math.sin(2 * Math.PI * 550 * i / 8192);
  write('od-sine.f32', x, odRun({ drive: 0.8, tone: 1, level: 0 }, x));
}
{ // wide-band noise at Drive 0.7, Tone open: every frequency of the 4x filter pair reaches the clipper and the output
  const r = lcg(81), x = new Float32Array(N);
  for (let i = 0; i < N; i++) x[i] = 0.25 * r();
  write('od-noise.f32', x, odRun({ drive: 0.7, tone: 1, level: 0 }, x));
}
