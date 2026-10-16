// SPDX-License-Identifier: MIT
// The let-go hold (hold.js):  node hosts/web/hold.test.mjs
// Positions are given at a 1 kHz rate, so samples read as milliseconds of audio.
import assert from 'node:assert/strict';
import { HOLD_MS, Hold, MIN_SPAN_MS, WINDOW_MS } from './hold.js';

const reading = (cents, midi = 40, unc = 0.1, samples = 0) =>
  ({ valid: true, midi, note: 'E', oct: 2, cents, unc, shz: 82.4, samples, rate: 1000 });
const silent = (samples) => ({ valid: false, midi: 0, cents: 0, unc: 0, samples, rate: 1000 });

function feed(list, stepMs = 33) {
  const h = new Hold();
  const placed = list.map((r, i) => ({ ...r, samples: i * stepMs }));
  placed.forEach((r) => h.add(r));
  return { h, last: placed[placed.length - 1] };
}

// A settled note whose last frame slipped: hold the median, not the slip.
{
  const settled = [0.2, 0.1, 0.3, 0.2, 0.1, 0.2, 0.3, 0.2, 0.1, 0.2, 0.2, 0.3, 0.2, 0.1].map((c) => reading(c));
  const { h, last } = feed([...settled, reading(-40.9, 40, 0.9)]);
  assert.equal(h.held(last).cents, 0.2, 'median of the confident readings');
}

// A one-frame blip: too short to hold a number.
{
  const { h, last } = feed([reading(3.0)]);
  assert.ok(Number.isNaN(h.held(last).cents), 'a single frame holds the note name only');
}

// Readings that disagree by more than 2 cents (a peg still turning): no number.
{
  const { h, last } = feed([0, 1, 2, 3, 4, 5, 6, 7, 8, 9].map((c) => reading(c)));
  assert.ok(Number.isNaN(h.held(last).cents), 'a moving pitch holds no number');
}

// A note change forgets the old note.
{
  const a = Array.from({ length: 12 }, () => reading(5, 40));
  const b = Array.from({ length: 12 }, () => reading(-1, 45));
  const { h, last } = feed([...a, ...b]);
  assert.equal(h.held(last).cents, -1, 'only the current note counts');
  assert.equal(h.held(last).midi, 45);
}

// Old readings leave the window.
{
  const h = new Hold();
  for (let i = 0; i < 10; ++i) h.add(reading(9, 40, 0.1, i * 33));
  for (let i = 0; i < 20; ++i) h.add(reading(1, 40, 0.1, 2000 + i * 33));
  assert.equal(h.held(reading(1)).cents, 1, 'readings older than half a second are gone');
}

// The window's edge: a reading exactly WINDOW_MS older than the newest is kept, one ms more is not.
{
  const at = (older) => {
    const h = new Hold();
    h.add(reading(-3, 40, 0.1, 0));
    for (let s = older; s <= older + WINDOW_MS; s += 50) h.add(reading(0, 40, 0.1, s));
    return h.held(reading(0)).cents;
  };
  assert.ok(Number.isNaN(at(0)), 'exactly 500 ms old: kept, and 3 c from the rest, so no number');
  assert.equal(at(1), 0, '501 ms old: gone');
}

// MIN_SPAN: readings spanning exactly 250 ms hold a number; 249 ms do not.
{
  const span = (ms) => {
    const h = new Hold();
    h.add(reading(0.5, 40, 0.1, 0));
    h.add(reading(0.5, 40, 0.1, ms));
    return h.held(reading(0.5)).cents;
  };
  assert.equal(span(MIN_SPAN_MS), 0.5, 'a 250 ms span holds');
  assert.ok(Number.isNaN(span(MIN_SPAN_MS - 1)), 'a 249 ms span does not');
}

// HOLD_MS on the audio clock: the let-go note stays while less than 1500 ms of audio has passed
// since the last valid reading, whatever the readings in between are.
{
  const h = new Hold();
  h.add(reading(0.2, 40, 0.1, 1000));
  h.add(silent(1100));
  assert.ok(h.holding(silent(1000 + HOLD_MS - 1)), '1499 ms after: still up');
  assert.ok(!h.holding(silent(1000 + HOLD_MS)), 'exactly 1500 ms after: gone');
  assert.ok(!new Hold().holding(silent(0)), 'nothing valid yet: nothing to hold');
}

// A reading seen again (the same position: a display that redraws, a poll that found nothing new)
// is the same reading: it is not counted twice.
{
  const h = new Hold();
  for (let i = 0; i < 9; ++i) h.add(reading(0, 40, 0.1, i * 33));
  for (let i = 0; i < 5; ++i) h.add(reading(1, 40, 0.1, 9 * 33));
  assert.equal(h.held(reading(0)).cents, 0, 'one reading at 1 c, not five');
}

// An earlier position means the source restarted (a new file, a new stream): everything goes.
{
  const h = new Hold();
  for (let i = 0; i < 12; ++i) h.add(reading(0.3, 40, 0.1, 5000 + i * 33));
  h.add(silent(20));
  assert.ok(!h.holding(silent(40)), 'the old note is not held into the new stream');
  assert.ok(Number.isNaN(h.held(reading(0.3)).cents), 'nor its readings');
}

// Positions at a real rate: 1600 samples at 48 kHz are 33.33 ms, and 45 of them exactly 1500 ms.
{
  const h = new Hold();
  const at48 = (r, s) => ({ ...r, samples: s, rate: 48000 });
  h.add(at48(reading(0.2), 0));
  assert.ok(h.holding(at48(silent(0), 44 * 1600)), '44 reads later: up');
  assert.ok(!h.holding(at48(silent(0), 45 * 1600)), '45 reads later (1500 ms): gone');
}

assert.equal(new Hold().held(null), null);
console.log('hold: all tests passed');
