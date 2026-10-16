// SPDX-License-Identifier: MIT
// The Pi bench's display, replayed: squatch-tuner-live --file prints one JSON line per display
// read (with "t", ms into the file); this turns them into the quality suite's frame rows, using
// the web page's own code where it is importable (hold.js, formatCents from tuner-view.js) and
// tuner-view.js's view rules otherwise (a note stays 1.5 s of audio after the tuner lets go; while
// live the number is the newest reading, after it the hold's median or --.-).
//
//   squatch-tuner-live --file take.wav | node live_frames.mjs > frames.csv
//
// The live host prints cents and the uncertainty at full float precision (%.9g), so the page rounds
// and holds the same values as every other display. (At three decimals, +0.0496 printed +0.050 and
// showed +0.1 where the others show 0.0: 21 of 5,730 frames before the fix.)
import { createInterface } from 'node:readline';
import { Hold } from '../../../../hosts/web/hold.js';
import { formatCents } from '../../../../hosts/web/tuner-view.js';

const state = { lastValid: null, hold: new Hold() };

// tuner-view.js feed.push(): every reading enters the hold as it arrives, on its audio position
// (samples, rate); the view then asks the hold whether a let-go note is still up (hold.js).
function remember(r) {
  state.hold.add(r);
  if (r.valid) state.lastValid = r;
}

// tuner-view.js viewOf() and shownReading(), reduced to what a frame row needs.
function shown(r) {
  if (r.valid) return state.lastValid;
  if (!state.lastValid || !state.hold.holding(r)) return null;
  return state.hold.held(state.lastValid);
}

function row(r) {
  remember(r);
  const s = shown(r);
  const note = s ? `${s.note},${s.oct}` : ',';
  const live = r.valid ? `${r.cents.toFixed(4)},${r.shz.toFixed(4)},1` : ',,0';
  const text = s && !Number.isNaN(s.cents) ? formatCents(s.cents).replace('−', '-').replace('+', '') : '';
  return `${r.t.toFixed(3)},${note},${live},${text}`;
}

console.log('t_ms,note,octave,cents,hz,valid,shown_cents');
const lines = createInterface({ input: process.stdin });
lines.on('line', (line) => {
  if (line.trim()) console.log(row(JSON.parse(line)));
});
