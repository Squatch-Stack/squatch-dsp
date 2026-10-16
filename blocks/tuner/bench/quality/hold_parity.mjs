// SPDX-License-Identifier: MIT
// hold.js and the page's formatCents are the reference (the page and the phone both show them);
// hosts/common/hold.hpp and tuner_frames.hpp's roundTenth must decide the same, on the same audio
// positions, for every sequence: whether a let-go note is up, the held cents to the bit, and the
// number to the digit. The cases of hold.test.mjs, then 400 random sequences (notes
// changing, confidence varying, gaps, bursts) and five real let-go notes from the phone at the
// 500 ms window edge, each queried after every reading: the held cents
// exactly, and the number on screen. Then the rounding alone on every exact tie (x.x5 that a float
// can hold: x.25, x.75), on floats next to ties, on doubles such as -0.05 (10 c must be rounded
// before the half is added, as JavaScript does), and on random floats.
//
//   node hold_parity.mjs build/hold_parity
import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
import { Hold } from '../../../../hosts/web/hold.js';
import { formatCents } from '../../../../hosts/web/tuner-view.js';

// Readings are floats in every host, so the inputs are too: the two sides then hold the same values.
// Positions are samples at 48 kHz; a list gives each reading its time in ms, turned into samples.
const RATE = 48000;
const reading = (cents, midi = 40, unc = 0.1, valid = true) => ({ valid, midi, cents: Math.fround(cents), unc: Math.fround(unc) });
const toSamples = (ms) => Math.round((ms * RATE) / 1000);
const screen = (c) => (Number.isNaN(c) ? '--.-' : formatCents(c).replace('−', '-').replace('+', ''));

// hold.test.mjs's sequences, as (reading, t) lists.
const fixed = [
  [0.2, 0.1, 0.3, 0.2, 0.1, 0.2, 0.3, 0.2, 0.1, 0.2, 0.2, 0.3, 0.2, 0.1].map((c, i) => [reading(c), i * 33]).concat([[reading(-40.9, 40, 0.9), 14 * 33]]),
  [0.1, 0.2, 0.1].map((c, i) => [reading(c), i * 33]),
  [[reading(1.0), 0], [reading(1.1), 150], [reading(4.5), 300]],
  [[reading(0.2, 45), 0], [reading(0.1, 45), 200], [reading(0.3, 40), 400], [reading(0.2, 40), 700]],
];

function randomSequence(seed) {
  let s = seed >>> 0;
  const rnd = () => ((s = (s * 1664525 + 1013904223) >>> 0) / 4294967296);
  const out = [];
  let t = 0, midi = 40 + Math.floor(rnd() * 12), base = (rnd() - 0.5) * 20;
  for (let i = 0; i < 20 + Math.floor(rnd() * 60); i++) {
    const x = rnd();
    if (x < 0.02) t = Math.max(0, t - 2000);                // the source restarted
    else if (x > 0.05) t += rnd() < 0.1 ? 100 + rnd() * 1800 : 33.333;  // else the same reading again
    if (rnd() < 0.05) midi += rnd() < 0.5 ? 1 : 12;
    const cents = Math.fround(base + (rnd() - 0.5) * (rnd() < 0.2 ? 6 : 1.5));
    out.push([reading(cents, midi, Math.fround(rnd() * 0.8), rnd() > 0.15), t]);
  }
  return out;
}

// The last half second before five let-go notes on the phone (iPhone 14, the pack's -play runs),
// as [t ms, midi, cents, unc]. In each, the oldest reading but one is exactly 500 ms before the
// last: WINDOW_MS's edge, which the phone's page decides by its own clock (performance.now()).
const real = [
  // vibrato G3: the phone kept the -0.51 c edge reading and held --.-
  [[1604.000, 55, -0.581983924, 0.124386929], [1637.333, 55, -0.51375401, 0.128002942], [1670.667, 55, -0.715148389, 0.197751179], [1704.000, 55, -1.05161488, 0.31396389], [1737.333, 55, -1.31730342, 0.439151406], [1770.667, 55, -1.52044368, 0.561700284], [1804.000, 55, -1.8277297, 0.394085616], [1837.333, 55, -2.32136083, 0.300928712], [1870.667, 55, -2.70557714, 0.452177435], [1904.000, 55, -2.42531204, 0.338269502], [1937.333, 55, -1.90468895, 0.396705419], [1970.667, 55, -1.78083289, 0.587955832], [2004.000, 55, -2.30131531, 0.415289551], [2037.333, 55, -2.99756837, 0.511475682], [2070.667, 55, -2.97420716, 0.693812311], [2104.000, 55, -2.45104218, 0.49054572], [2137.333, 55, -2.45104218, 0.49054572]],
  // our E retune take: the phone dropped it and held -0.4
  [[25717.333, 40, -0.910784543, 0.00780754676], [25749.333, 40, -0.923483491, 0.00750290463], [25782.667, 40, -0.971079946, 0.00663917419], [25816.000, 40, -0.984605491, 0.00746425893], [25849.333, 40, -0.935459912, 0.00788910128], [25882.667, 40, -0.802691519, 0.00786044635], [25916.000, 40, -0.714528978, 0.00468509644], [25949.333, 40, -0.632771015, 0.0044103656], [25982.667, 40, -0.53016609, 0.0064010201], [26016.000, 40, -0.392061621, 0.00600740872], [26050.667, 40, -0.338082105, 0.00598103693], [26082.667, 40, -0.311970234, 0.00759476004], [26116.000, 40, -0.25644514, 0.00781932008], [26149.333, 40, -0.163460031, 0.00994050596], [26182.667, 40, -0.214441255, 0.0177907608], [26217.333, 40, -0.139105156, 0.0328351073], [26249.333, 40, 0.10009101, 0.0610415041]],
  // chromatic E: dropped, -2.4
  [[750.667, 40, -1.91357303, 0.00463445485], [785.333, 40, -1.91501927, 0.00449679187], [818.667, 40, -1.90923429, 0.00450626714], [852.000, 40, -1.91739523, 0.00502311857], [884.000, 40, -1.94962633, 0.00567460712], [918.667, 40, -2.02008247, 0.0064144237], [952.000, 40, -2.10211253, 0.00655938499], [985.333, 40, -2.17867064, 0.00620860979], [1017.333, 40, -2.26546121, 0.00569879124], [1052.000, 40, -2.37767553, 0.00503840111], [1085.333, 40, -2.46127272, 0.00453001261], [1118.667, 40, -2.51056457, 0.00471594092], [1150.667, 40, -2.50601768, 0.00604051398], [1184.000, 40, -2.37457561, 0.00778072979], [1217.333, 40, -2.41694188, 0.0101761706], [1252.000, 40, -2.54342699, 0.0115432059], [1285.333, 40, -2.55882478, 0.0112531818]],
  // free play A#2: dropped, 2.7
  [[9810.667, 46, 3.97010112, 0.0192750711], [9842.667, 46, 3.83378553, 0.0188977886], [9877.333, 46, 3.67913079, 0.0194545053], [9909.333, 46, 3.54011488, 0.0196927246], [9944.000, 46, 3.39779258, 0.0193840656], [9977.333, 46, 3.21281719, 0.0195527226], [10010.667, 46, 3.00495458, 0.0197833516], [10042.667, 46, 2.83518457, 0.0199911091], [10077.333, 46, 2.68270707, 0.0216026958], [10110.667, 46, 2.57060695, 0.0250969511], [10144.000, 46, 2.49992275, 0.0307247527], [10177.333, 46, 2.45891213, 0.0287089292], [10210.667, 46, 2.41893053, 0.0266145431], [10242.667, 46, 2.3249507, 0.027218774], [10276.000, 46, 2.19633818, 0.0284156241], [10310.667, 46, 1.96670794, 0.0317982547], [10342.667, 46, 3.37657666, 0.0567546003]],
  // pickups E4: dropped, 3.4
  [[1817.333, 64, 7.3149066, 0.0663645118], [1850.667, 64, 5.03245831, 0.0407336466], [1885.333, 64, 4.20831823, 0.0225385446], [1917.333, 64, 4.45926571, 0.0102362242], [1952.000, 64, 4.29807901, 0.00644259946], [1984.000, 64, 4.14593649, 0.00638273545], [2017.333, 64, 4.0668745, 0.00817143917], [2052.000, 64, 3.76521206, 0.00800487399], [2084.000, 64, 3.65874243, 0.00745388912], [2117.333, 64, 3.36442423, 0.00791190658], [2152.000, 64, 3.04018354, 0.00649870466], [2185.333, 64, 2.91347861, 0.00491238525], [2217.333, 64, 2.83085752, 0.00444012182], [2250.667, 64, 2.79871488, 0.00412220228], [2285.333, 64, 2.79974508, 0.00462654838], [2318.667, 64, 2.82220387, 0.00499398215], [2350.667, 64, 3.12691474, 0.00693910476]],
];
// Each at its logged positions, and with the edge reading one sample inside and outside the window.
// A let-go follows: invalid readings for two seconds, 30 a second.
const edge = (seq, shift) => seq.map(([t, m, c, u], i) => [i === 1 ? t + shift : t, m, c, u]);
const letGo = (seq) => [...seq, ...Array.from({ length: 60 }, (_, k) => [reading(0, 0, 0, false), seq[seq.length - 1][1] + 33.333 * (k + 1)])];
const realSequences = real.flatMap((s) => [0, 1000 / RATE, -1000 / RATE].map((d) => letGo(edge(s, d).map(([t, m, c, u]) => [reading(c, m, u), t]))));

const sequences = [...fixed, ...realSequences, ...Array.from({ length: 400 }, (_, k) => randomSequence(k + 1))];
const ops = [], expected = [], asked = [];
for (const seq of sequences) {
  const h = new Hold();
  ops.push('reset');
  let last = null;
  for (const [r, t] of seq) {
    const at = { ...r, samples: toSamples(t), rate: RATE };
    h.add(at);
    if (at.valid) last = at;
    ops.push(`add ${at.samples} ${RATE} ${at.valid ? 1 : 0} ${at.midi} ${at.cents} ${at.unc}`, `holding ${at.samples}`);
    expected.push(h.holding(at) ? '1' : '0');
    asked.push(`holding ${at.samples}`);
    if (!last) continue;
    const held = h.held(last).cents;
    ops.push(`held ${last.midi}`, `shown ${last.midi}`);
    expected.push(Number.isNaN(held) ? 'nan' : held, screen(held));
    asked.push(`held ${last.midi}`, `shown ${last.midi}`);
  }
}

// The rounding alone: every exact tie in +-50 c, the floats either side of each, random floats.
const values = [];
for (let k = -200; k <= 200; k++) {
  const tie = k / 4;  // multiples of 0.25: x.25 and x.75 are exact ties at one decimal
  values.push(tie, Math.fround(tie + 2 ** -20), Math.fround(tie - 2 ** -20), Math.fround(k / 20), Math.fround(k / 20 + 0.05), k / 20, k / 20 + 0.05);
}
let s = 7;
for (let i = 0; i < 2000; i++) values.push(Math.fround(((s = (s * 1664525 + 1013904223) >>> 0) / 4294967296 - 0.5) * 100));
for (const v of values) {
  ops.push(`round ${v}`);
  asked.push(`round ${v}`);
  expected.push(screen(v));
}

const got = execFileSync(process.argv[2], { input: ops.join('\n') + '\n' }).toString().trim().split('\n');
assert.equal(got.length, expected.length);
let bad = 0;
got.forEach((g, i) => {
  const e = expected[i];
  const same = typeof e === 'number' ? Number(g) === e : g === e;
  if (!same && bad++ < 5) console.log(`differs at query ${i} (${asked[i]}): js ${e}, c++ ${g}`);
});
console.log(`hold parity: ${expected.length} queries (held cents exactly, and the number on screen) over ` +
            `${sequences.length} sequences and ${values.length} roundings, ${bad} differ`);
process.exit(bad ? 1 : 0);
