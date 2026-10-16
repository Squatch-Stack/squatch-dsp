// Render the browser rig's effect kernels over raw float32 inputs, for tools/parity.
//
//   node js_render.mjs RIG_DIR CASES.json OUT_DIR
//
// Runs the SAME kernels the browser worklets and tools/render.mjs use (web/worklets/dsp/*.js, via
// runFx for the per-quantum channel glue), 128 samples at a time (the WebAudio render quantum).
// The wet/dry Gain pair tools/render.mjs puts around each node is wet=1, dry=0, so it is left out.
// NAM goes through NamHead + nam.wasm exactly as tools/render/graph.mjs builds it.
// RIG_DIR is only read. Each case writes OUT_DIR/js/<id>.f32 (left channel; reverb also .R.f32).
import fs from 'node:fs';
import path from 'node:path';
import { pathToFileURL } from 'node:url';

const [rig, casesFile, outDir] = process.argv.slice(2);
const imp = (p) => import(pathToFileURL(path.join(rig, p)).href);
const { runFx } = await imp('web/worklets/dsp/fxrun.js');
const { Compressor } = await imp('web/worklets/dsp/compressor.js');
const { Overdrive } = await imp('web/worklets/dsp/overdrive.js');
const { Reverb } = await imp('web/worklets/dsp/reverb.js');
const { NoiseGate } = await imp('web/worklets/dsp/gate.js');
const { Limiter } = await imp('web/worklets/dsp/limiter.js');
const KINDS = { compressor: Compressor, overdrive: Overdrive, reverb: Reverb, gate: NoiseGate, limiter: Limiter };
const Q = 128;

const readF32 = (f) => { const b = fs.readFileSync(f); return new Float32Array(b.buffer, b.byteOffset, b.byteLength / 4); };
const writeF32 = (f, a) => fs.writeFileSync(f, Buffer.from(a.buffer, a.byteOffset, a.byteLength));

const cases = JSON.parse(fs.readFileSync(casesFile, 'utf8'));
fs.mkdirSync(path.join(outDir, 'js'), { recursive: true });
const sr = 48000;
let nam = null, wasmModule = null;

for (const c of cases) {
  if (!c.js) continue;
  const gain = Math.pow(10, (c.gainDb || 0) / 20);
  const src = readF32(path.join(outDir, 'in', c.input + '.f32'));
  const total = src.length + Math.round((c.tail || 0) * sr);
  const x = new Float32Array(Math.ceil(total / Q) * Q);
  for (let i = 0; i < src.length; i++) x[i] = src[i] * gain;
  const L = new Float32Array(x.length), R = new Float32Array(x.length);
  const kind = c.js.kind;
  if (kind === 'cab') {
    // input -> cab -> output through the offline renderer's graph, as tools/render.mjs builds it
    const { renderDoc } = await imp('tools/render/graph.mjs');
    const { synthCabIR } = await imp('web/js/audio/ir.js');
    const doc = { nodes: { i: { type: 'input', params: { gain: 0 } }, c: { type: 'cab', params: c.js.params }, o: { type: 'output', params: { volume: 0 } } },
                  edges: { a: { from: ['i', 0], to: ['c', 0] }, b: { from: ['c', 0], to: ['o', 0] } }, models: {} };
    const r = renderDoc({ doc, di: x.subarray(0, total), models: {}, wasmBytes: fs.readFileSync(path.join(rig, 'web/wasm/nam.wasm')), seconds: total / sr, sampleRate: sr });
    L.set(r.channels[0].subarray(0, total));
    writeF32(path.join(outDir, 'in', `ir_${c.id}.f32`), synthCabIR(sr, c.js.params.voice, c.js.params.mic));
  } else if (kind === 'nam') {
    const { NamHead } = await imp('web/worklets/dsp/namhead.js');
    if (!nam) {
      const { instantiateNam } = await imp('web/worklets/nam-wasm.js');
      wasmModule = new WebAssembly.Module(fs.readFileSync(path.join(rig, 'web/wasm/nam.wasm')));
      nam = instantiateNam(wasmModule);
    }
    const head = new NamHead(sr, { ...c.js.params });
    const model = nam.create(new Uint8Array(fs.readFileSync(path.join(rig, 'web/models', c.js.model))), sr, Q);
    head.setModel(model);
    for (let p = 0; p < x.length; p += Q) head.process(x.subarray(p, p + Q), L.subarray(p, p + Q), Q);
    head.setModel(null)?.destroy();
  } else {
    const K = KINDS[kind];
    const k = new K(sr);
    for (const [name, v] of Object.entries(c.js.params || {})) k.set(name, v);
    for (let p = 0; p < x.length; p += Q) {
      const ins = [x.subarray(p, p + Q)];
      if (kind === 'reverb') {
        const outs = [L.subarray(p, p + Q), R.subarray(p, p + Q)];
        runFx(kind, k, ins, outs, Q);
      } else {
        const outs = [L.subarray(p, p + Q)];
        runFx(kind, k, ins, outs, Q);
      }
    }
    if (kind === 'limiter') c.js.latency = k.latency;
  }
  writeF32(path.join(outDir, 'js', c.id + '.f32'), L.subarray(0, total));
  if (kind === 'reverb') writeF32(path.join(outDir, 'js', c.id + '.R.f32'), R.subarray(0, total));
  console.log(JSON.stringify({ id: c.id, kind, latency: c.js.latency ?? 0 }));
}
