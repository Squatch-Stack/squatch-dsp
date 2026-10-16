<!-- SPDX-License-Identifier: MIT -->
# Tuner quality suite

Proves a tuner build is accurate and steady **as a player sees it**, with ground truth, on every
platform. The Mac run is the reference; the Pi, the phone and the ESP32-S3 probe replay the same
files and are scored by the same code, and any difference between platforms on the same file is a
bug.

```sh
make tuner-quality-test                        # the suite's own tests, and hold.hpp against hold.js
make tuner-quality TUNER_CORPUS=/path/to/di-corpus  # run, then score (build/tuner-quality/scorecard)
```

Nothing from any dataset is in this repository. `TUNER_CORPUS` names a folder laid out as
`public/egfxset/Clean/<pickup>/<string>-<fret>.wav`,
`public/guitar-techs/P{1,2}_singlenotes/P{1,2}_singlenotes/{audio/directinput,midi}/`,
`owner/di-20260924/*.wav`, `public/guitarset/{annotation,audio_hex-pickup_debleeded}/`,
`public/nsynth/{audio/,examples-guitar.json}` and
`public/idmt-smt-guitar/dataset1/<setup>/{audio,annotation}/`; missing parts are skipped. Without
it, only the synthetic set runs.

**Licences.** Every set but one is CC BY 4.0. **IDMT-SMT-Guitar is CC BY-NC-ND 4.0: internal
testing only.** Nothing derived from it may be shipped, published or committed; `--pack` refuses it
(`internalOnly()` in `idmt_cases.hpp`), and the scorecard marks its rows "internal only". The corpus
folder's `LICENSES.md` has each set's source, version, licence and full attribution; the
scorecard's "Data licences" section lists the sets it used.

## The display path

`display_path.hpp` runs a recording exactly as the apps read the tuner:

- `TunerBlock` in fixed blocks of 16 or 64 frames;
- its `Latest<TunerSnapshot>` polled 30 times a second, after the block that crosses each
  1/30 s of file time;
- `toDisplay()` (`hosts/common/display.hpp`), then the phone's `TunerDisplay` rules. A note stays
  1.5 s after release. While live, the number is the reading's cents to 0.1. After release it is
  the hold's median (`hosts/common/hold.hpp`, checked against `hold.js` by `hold_parity.mjs`),
  or `--.-`. Only fresh snapshots enter the hold.

Every platform writes the same frame rows (`io.hpp`, and `hosts/common/tuner_frames.hpp`):
`t_ms,note,octave,cents,hz,valid,shown_cents`.

## Ground truth

| Set | Cases | Truth |
|---|---|---|
| Synthetic (`synth_cases.hpp`, `../synth.hpp`) | 668 | **Exact.** One factor at a time around a realistic pluck on all six open strings: tuning −49.9…+49.9 c (0.1 c steps within ±2 c), stiffness B 0…2×10⁻³, glide 0…40 c, T60 0.6…8 s, SNR 60…10 dB, level −60…−6 dBFS peak, 50/60 Hz hum with 5 harmonics at −40…−20 dB, five pickup positions and mixes, vibrato and bends. Plus low B, drop D, bass E1, F#1 and frets to 24. |
| EGFxSet clean DI | 690 | **Note** from string and fret; **pitch** from the offline reference. |
| Guitar-TECHS single notes (P1, P2) | 2 takes, 279 notes | **Note** and time from the MIDI shipped with each take (the offset to the audio is measured and applied); **pitch** from the reference. |
| Our DI session | 32 takes | Notes known where the take says what was played (open strings, frets 0–12, harmonics, re-plucks, retunes). Chords and free play are scored for steadiness only. Idle takes count false readings. |
| GuitarSet (`guitarset_cases.hpp`) | 298 string tracks (60 recordings), 8,458 notes | **Note** from the JAMS `note_midi`; **pitch over time** from the string's JAMS `pitch_contour` (pYIN, good to a few cents), truth kind `annotated`. Each hexaphonic (debleeded) channel is one string, at 44.1 kHz. Notes of 0.9 s or more also get the offline reference, for the note error. Reported as a moving set (real playing: bends, vibrato, fast lines), with tracking error by time since the note began, moving against still, and the display's lag. |
| NSynth test split, guitar notes (`nsynth_cases.hpp`) | 652 | **Note** from the MIDI label; **pitch** from the offline reference (samples are not guaranteed in tune). Resampled 16 → 48 kHz with `bench::resample` (windowed sinc, 96 taps, Blackman, cut-off 7.6 kHz). Notes outside E1–E6 are reported apart (tier `outside`). |
| IDMT-SMT-Guitar dataset1 (`idmt_cases.hpp`), **internal only** | 312 | **Note** and onset/offset from each note's XML; **pitch** from the offline reference. Four guitar and pickup setups, frets 0–12. |
| Regression (`regression_cases.hpp`) | 5 | **Exact.** One synthetic recording per failure found on real guitars, each made to fail the way the real one did: a note dying into 56 Hz hum, a stiff string's quiet tail, a fundamental that dies first, a slow pitch glide, six open strings ringing together. `test_quality` checks the property each fix must keep. |

**The offline reference** (`reference.hpp`) is deliberately not the tuner's own method:

1. A Blackman-Harris window over 0.5–1.7 s after the onset.
2. Each partial is the maximum of the window's continuous spectrum: a grid, then golden-section
   search.
3. A partial is kept only with 20 dB of SNR against the median spectrum between partials, and
   only if it is clear of every 50/60 Hz harmonic.
4. A stiff-string fit (B is fitted with 3 or more partials; otherwise B = 1×10⁻⁴ is assumed).

Its own error, measured by `test_quality` on synthetic strings with known f0 (clean, 30 dB SNR,
hum at −30 dB, all six strings): **at most 0.065 c**.

For the display comparison, a contour of causal 0.6 s windows every 100 ms follows the string. It
lags a glide as the tuner's own 0.5 s memory does: within 0.46 c of the truth at the window's
centre on a 10 c glide.

`cases.jsonl` records, for each note, the fit's standard error (`ref_unc_cents`) and half the change
between the two halves of the window (`ref_drift_cents`, the string's own movement). Real strings
drift by cents over a note, so real-data errors are only as good as these.

## Measures and thresholds

Defined in `metrics.py`; the thresholds are in `score.py` (`THRESHOLDS`).

| Measure | Pass if |
|---|---|
| **wobble**: peak-to-peak of the shown number about its own straight line, over every 300 ms from 0.5 s after the onset (p95) | ≤ 0.5 c |
| raw peak-to-peak over 300 ms while the truth is steady (moved < 0.2 c) (p95) | ≤ 0.5 c |
| frame-to-frame change of the shown number (p90) | ≤ 0.2 c |
| direction flips of the shown number per second | ≤ 1 |
| wrong note (octave counted apart) among live frames after 100 ms | ≤ 1% |
| \|error\| against the truth, steady state: median / p95 | ≤ 0.1 c / ≤ 1.0 c |
| coverage of 0.1–2 s | ≥ 90% |
| time to settle within ±1 c (median) | ≤ 1 s |

Scoring rules:

- Synthetic conditions a tuner cannot be asked to read cleanly are reported as **stress** and
  kept out of the verdict: 10–20 dB SNR, −60 dBFS, hum at −20 dB, T60 0.6 s, B 2×10⁻³, a 40 c
  glide.
- Vibrato and bends are reported as **moving**.

## Other platforms

- **Pi:** `tuner_quality --platform pi --frames-only 1` on the Pi scores the Pi's arithmetic on
  every case.
- **Pi's live path:** `squatch-tuner-live --file take.wav | node live_frames.mjs` replays a take
  through the live host's own formatting and the web page's `hold.js` and `formatCents`.
- **Phone and S3 probe:** they play the phone pack and write frame rows. Two commands score and
  compare them:
  - `score.py file --case X.case.json --truth X.truth.csv --frames LOG.csv` scores one log;
  - `score.py diff A.csv B.csv` compares two logs line by line.
- `score.py scorecard --run DIR [--run DIR ...] --out DIR` puts every platform in one scorecard,
  with a frame-by-frame comparison of every pair.

## Breakdowns (`breakdowns.py`: statistics in `stats.py`, tracking in `tracking.py`, rendered by `breakdowns_md.py`)

The scorecard also gives:

- **a verdict per set**: each dataset row's steadiness and accuracy on its own, with the checks
  that failed;
- **breakdowns** per string, fret range, pickup or guitar setup, NSynth instrument, pitch band,
  velocity and quality, and GuitarSet style, player, comp or solo, and note length;
- **the pickup question**: EGFxSet's five positions and IDMT's four setups, paired by string and
  fret. Per measure: Friedman's test, Kendall's W as the effect size, and each position's median
  paired difference from a reference position with a bootstrap 95% interval;
- **tracking over time** on GuitarSet: error against the contour by time since the note began,
  moving (≥ 5 c in 100 ms) against still, the lag that best explains the display on a moving
  string, and how often notes of each length are read at all;
- the worst notes of each new set, with file and time, and the data licences.
