// SPDX-License-Identifier: MIT
// What the page holds on screen after the tuner lets go of a note.
//
// The last valid reading is the worst one to keep: the core withholds a reading once the note
// has sunk toward the room, so the frame just before that sits right at the threshold (this is
// how a clean +0.2 c note was once frozen at -40.9 c). Instead the page keeps the last half
// second of confident readings on the same note and holds their median. If they are too few,
// span too little time or disagree, it holds the note name only and the number reads --.-, so a
// one-frame blip is never presented as a measurement.
//
// The rule, on the AUDIO clock. Every display (this page, hosts/common/hold.hpp for the C++ ones)
// applies it to the same readings and so shows the same thing, frame for frame:
//
//   - Time is the reading's position in its source's audio: `samples` at `rate` (the sample count
//     when the reading was published). An age is 1000 * (newer.samples - older.samples) / rate ms,
//     from the exact integer difference, never from a wall clock or an animation frame.
//   - add(r) takes every reading as it arrives, valid or not. A reading at the same position as the
//     newest valid one is that reading again and adds nothing. A position earlier than the newest
//     seen means the source restarted (a new file, a new stream): everything is forgotten, as it
//     is when the source goes offline (forget()), since what comes back is a new stream.
//   - A valid reading on another note than the kept ones starts afresh. A valid reading with
//     unc <= MAX_UNC is kept; kept readings more than WINDOW_MS older than the newest valid one
//     are dropped (exactly WINDOW_MS old is kept).
//   - holding(r): a let-go note stays on screen while r's age since the last valid reading is
//     under HOLD_MS (exactly HOLD_MS: gone).
//   - held(last): the median of the kept readings on last's note (the mean of the middle two for
//     an even count), or NaN (--.-) when they span less than MIN_SPAN_MS or disagree by more than
//     MAX_SPREAD cents.
//
// The page animates at its own frame rate; only these decisions follow the audio. They used to
// follow performance.now(), and a reading exactly 500 ms old (15 readings at 30 a second) was kept
// or dropped by the wall clock: the phone's page went both ways at the same audio age.

export const WINDOW_MS = 500;    // readings older than this are forgotten
export const MIN_SPAN_MS = 250;  // a held value must rest on at least this much of the note
export const MAX_UNC = 0.5;      // cents: only confident readings count
export const MAX_SPREAD = 2;     // cents: the kept readings must agree this well
export const HOLD_MS = 1500;     // a let-go note stays on screen this long

export class Hold {
  constructor() {
    this.forget();
  }

  /** Start afresh: nothing kept, nothing seen. */
  forget() {
    this.recent = [];         // { s, r } for confident readings on the current note, oldest first
    this.newest = -Infinity;  // position of the newest reading seen
    this.lastValid = -Infinity;
    this.rate = 1;
  }

  /** Every reading as it arrives (valid or not), at its position r.samples at r.rate. */
  add(r) {
    if (!r) return;
    if (r.samples < this.newest) this.forget();
    this.newest = r.samples;
    this.rate = r.rate;
    if (!r.valid || r.samples === this.lastValid) return;
    this.lastValid = r.samples;
    if (this.recent.length && this.recent[this.recent.length - 1].r.midi !== r.midi) this.recent = [];
    if (r.unc <= MAX_UNC) this.recent.push({ s: r.samples, r });
    while (this.recent.length && this.ageMs(r.samples, this.recent[0].s) > WINDOW_MS) this.recent.shift();
  }

  /** Whether a let-go note is still on screen when `r` is the newest reading. */
  holding(r) {
    return Boolean(r) && this.ageMs(r.samples, this.lastValid) < HOLD_MS;
  }

  /** The reading to hold: the newest one, with `cents` the median of the kept readings, or NaN. */
  held(last) {
    if (!last) return null;
    const kept = this.recent.filter((k) => k.r.midi === last.midi);
    const cents = kept.map((k) => k.r.cents).sort((a, b) => a - b);
    const span = kept.length ? this.ageMs(kept[kept.length - 1].s, kept[0].s) : 0;
    const agree = cents.length > 0 && cents[cents.length - 1] - cents[0] <= MAX_SPREAD;
    if (span < MIN_SPAN_MS || !agree) return { ...last, cents: NaN };
    const mid = cents.length >> 1;
    const median = cents.length % 2 ? cents[mid] : (cents[mid - 1] + cents[mid]) / 2;
    return { ...last, cents: median };
  }

  /** ms from `older` to `newer` (sample positions), from their exact integer difference. */
  ageMs(newer, older) {
    return (1000 * (newer - older)) / this.rate;
  }
}
