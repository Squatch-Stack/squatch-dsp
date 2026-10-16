// Is js_render.mjs the same DSP as the rig's own offline renderer?  node check_harness.mjs RIG_DIR OUT_DIR
// Renders three cases a second time as a one-node document through tools/render/graph.mjs (the real
// renderDoc, wet/dry gains and all) and compares with js_render.mjs's direct kernel output.
import fs from 'node:fs';
import path from 'node:path';
import { pathToFileURL } from 'node:url';
const [rig, out] = process.argv.slice(2);
const { renderDoc } = await import(pathToFileURL(path.join(rig, 'tools/render/graph.mjs')).href);
const f32 = (f) => { const b = fs.readFileSync(f); return new Float32Array(b.buffer, b.byteOffset, b.byteLength / 4); };
const cases = JSON.parse(fs.readFileSync(path.join(out, 'cases.json'), 'utf8'));
const wasmBytes = fs.readFileSync(path.join(rig, 'web/wasm/nam.wasm'));
let worst = 0;
for (const id of ['gate-A', 'comp-A', 'limiter-A', 'od-A']) {
  const c = cases.find((x) => x.id === id);
  const di = f32(path.join(out, 'in', c.input + '.f32'));
  const doc = { nodes: { i: { type: 'input', params: { gain: 0 } }, f: { type: c.js.kind, params: c.js.params }, o: { type: 'output', params: { volume: 0 } } },
                edges: { a: { from: ['i', 0], to: ['f', 0] }, b: { from: ['f', 0], to: ['o', 0] } }, models: {} };
  const r = renderDoc({ doc, di, models: {}, wasmBytes, seconds: di.length / 48000, sampleRate: 48000, inputGainDb: c.gainDb || 0 });
  const mine = f32(path.join(out, 'js', id + '.f32')).map((v) => (process.env.PARITY_PERTURB ? v * 1.01 : v)); // PARITY_PERTURB: the negative control
  let num = 0, den = 0;
  for (let k = 0; k < mine.length; k++) { const d = r.channels[0][k] - mine[k]; num += d * d; den += mine[k] * mine[k]; }
  const esr = num / den;
  worst = Math.max(worst, esr);
  console.log(`harness check ${id}: ESR of the rig renderDoc vs js_render = ${esr.toExponential(2)}`);
}
if (!(worst < 1e-8)) { console.error('FAIL: js_render.mjs does not match the rig renderer'); process.exit(1); }
