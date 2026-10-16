<!-- SPDX-License-Identifier: MIT -->
# Squatch Tuner

A guitar and bass tuner core that reads a string to a tenth of a cent, built as a software
strobe. Portable C++17, header-only, no dependencies. It has fixed-size buffers and never
allocates or locks in the audio path, so the same code can run on a Daisy Seed (Cortex-M7), an
ESP32-S3, a Pi 5 LV2 plugin, a desktop plugin and a WebAssembly AudioWorklet.

Measured numbers, with how they were measured and where it fails, are in [RESULTS.md](RESULTS.md).
It is developed here, in squatch-dsp's `blocks/tuner/`, and also published on its own as
[squatch-tuner](https://github.com/squatch-stack/squatch-tuner), with a
[browser demo](https://squatch-stack.github.io/squatch-tuner/). The
`TunerBlock` adapter (`include/squatch/dsp/blocks/tuner.hpp` at the top of the repo) puts it
behind the common `Block` interface, and its hosts are in the top-level `hosts/`.
The docs site has a chapter explaining the design and comparing it with other tuners and methods.

## How it works

```text
input (any rate) -> decimate to ~12 kHz -> history -> MPM every 5.3 ms: which note?
                                       \-> strobe bank, every sample: how far off?
                                             -> stiffness-aware fit -> reading
```

1. **Coarse: which note.** The McLeod Pitch Method (normalised square difference) finds the
   period. It runs every hop until a note is found, then every fourth hop to check the note.
2. **Fine: how far off.** This is a software strobe. Each partial k of the note is mixed down
   with a reference at k times the note's frequency, from one 32-bit phase accumulator. It is
   then low-passed by two boxcars a whole number of periods long, which null every other
   partial exactly. The drift rate of each partial's phase is its frequency error, found by a
   weighted least-squares line fit over a sliding window. A parabola fit reads the drift at the
   newest point, so a gliding note is followed without lag. That correction is shrunk by its
   own significance.
3. **Stiffness.** Real strings are stiff, so partial k sits at k·f0·√(1+Bk²). The fit estimates
   B per note and reports the fundamental itself, not the sharp average of the harmonics.
   Measured on real DI, MPM alone reads 2 to 6 cents sharp for this reason. B is the string's,
   so it is learned while the note is young and loud (50 ms to 2 s, within 30 dB of its peak)
   and then held.
4. **Robustness.** It handles these problem cases:
   - Partials that disagree with the rest (hum, a sympathetic string) are weighted down
     smoothly, so the number never flips between two partials. If they still disagree by more
     than 3 cents, the number is withheld rather than shown.
   - Octave slips in both directions are caught in the first half second of a note: upwards by
     the coarse stage, downwards when the odd partials of the reference are empty. After that a
     note is re-named only by a new pluck.
   - Readings are withheld once a note has decayed into the room's hum and noise floor.
   - Hum cannot start a note once a real note has been heard, and nothing starts without a
     pluck at a note's tail unless it is within 20 dB of that note and above 70 Hz.
   - Strings ringing together (a hard pluck rings the other open strings) can still start a
     note: a pitch the coarse stage keeps returning at lower clarity, checked against it.
   - A steady test tone present from power-on still reads.
5. **Polyphonic strum check** (`poly.hpp`). The tuning is known, so each string gets its own
   strobe channels at its target pitch.

The display gets two readings. `hz` follows the string, glide included, and drives the strobe
animation. `steadyHz` is averaged over the window, read a quarter of the way from its centre to
the newest point, and drives the number and needle.
`strobeTurns[k]` is each partial's phase, for drawing the strobe.

## Use

```cpp
#include <squatch/tuner/tuner.hpp>

static squatch::tuner::Tuner tuner;   // ~128 kB, keep it off the stack

void setup(float sampleRate) {
  squatch::tuner::TunerConfig c;
  c.sampleRate = sampleRate;           // any rate; decimated internally towards 12 kHz
  c.minHz = 38.0f;                     // 27 for a 5-string bass (B0)
  tuner.configure(c);                  // not real-time safe: builds tables
}

void audioCallback(const float* in, int n) {
  tuner.process(in, n);                // real-time safe, any block size
  const auto& r = tuner.reading();
  if (r.valid) show(r.midi, r.cents, r.steadyHz, r.strobeTurns);
}
```

## Layout

| Path | What |
|---|---|
| `include/squatch/tuner/` | the core: `tuner.hpp` (mono), `poly.hpp` (strum check), `strobe.hpp`, `phase_slope.hpp`, `partial_fit.hpp`, `period.hpp` (MPM, YIN), `level.hpp`, `decimator.hpp`, `nco.hpp`, `history.hpp`, `common.hpp` |
| `tests/test_core.cpp` | unit tests, built with AddressSanitizer and UBSan; one test counts heap allocations during `process()` (must be zero) |
| `bench/` | the measurement harness: synthetic plucked strings with known truth, real DI evaluation, polyphonic strums, CPU cost; baselines (YIN, MPM, MPM + phase vocoder) behind the same front end |
| `bench/quality/` | the quality suite: the display path the apps use (TunerBlock, 30 reads a second, the release hold) over synthetic notes with exact truth and real DI with an offline reference, scored for accuracy and steadiness on every platform (`make tuner-quality`, see its README) |
| `results/` | raw output of `make results`, which RESULTS.md summarises |
| `docs/screens/` | screenshots of the live page |

The hosts are at the top of squatch-dsp:

| Path (from the repo root) | What |
|---|---|
| `hosts/common/` | host layer any front end can reuse: `display.hpp` (note name, steady and fast cents against a run-time A4); `latest.hpp` forwards to the core's lock-free handoff, `include/squatch/dsp/latest.hpp`; `tuner_host.hpp` and `tuner_publisher.hpp` (the live tuner's real-time step and JSON line, shared by the JACK and CoreAudio hosts); `tone_gen.hpp` (the exact sine) |
| `hosts/jack/` | `squatch-tuner-live`, the tuner behind a JACK input (Linux, the Pi), and `squatch-tone`, an exact test tone |
| `hosts/coreaudio/` | the same two programs for macOS, on CoreAudio directly: no JACK, only system frameworks |
| `hosts/web/` | `serve.py` (standard-library bridge: page + Server-Sent Events) and the strobe page |
| `hosts/wasm/` | the browser demo: the core as WebAssembly in an AudioWorklet, the same page with its own input bar, and its tests (`make wasm-test`) |
| `hosts/tests/` | the host layer's tests (ThreadSanitizer) |

## Build, test, measure

In this directory:

```sh
make test      # unit tests (ASan + UBSan); the top-level `make test` runs them too
make all       # tests and the bench programs
make results   # every number in RESULTS.md; needs the Guitar-TECHS DI files (see RESULTS.md)
make lint      # lizard: CCN <= 10, <= 60 lines, <= 5 parameters per function
```

The hosts build from the repo root: `make live` (CoreAudio on macOS, JACK elsewhere), and `make test` runs the host tests,
including the CoreAudio host's audio path against a test tone, with no audio device.

There are no dependencies to fetch: a C++17 compiler is enough. The bench uses only the
standard library.

## Live on a Raspberry Pi

`hosts/jack` runs the core behind a JACK input and `hosts/web/serve.py` streams its readings to
the strobe page in any browser on the network. RESULTS.md, "Live on the Pi 5", has the numbers
from a Pi 5 with a USB audio interface.

```text
capture_1 -> jackd (48 kHz, 64 x 3) -> squatch-tuner-live
     JACK realtime thread: Tuner::process() -> Latest<Frame>     (no allocation, locks or I/O)
     main thread, 30 times a second: newest Frame -> one JSON line on stdout
  -> serve.py: page + /events (Server-Sent Events) + /clock + POST /a4 -> browser
```

```sh
make live                                             # needs libjack-jackd2-dev
python3 hosts/web/serve.py -- build/squatch-tuner-live --input 1
build/squatch-tone 110                                # an exact test tone on playback_1
```

## Live on a Mac

On macOS `make live` builds the same two programs on CoreAudio. They link system frameworks only
(`otool -L` shows `/System` and `/usr/lib`), so nothing needs installing, and the options and the JSON
lines are the JACK host's.

```sh
make live
build/squatch-tuner-live --list-devices               # every device: channels, rate, which are the defaults
build/squatch-tuner-live                              # the default input device, channel 1
build/squatch-tuner-live --device umc --input 2       # a device by (part of) its name, channel 2
build/squatch-tone 110 --device umc --port 1          # an exact tone on that device's output 1
python3 hosts/web/serve.py -- build/squatch-tuner-live
```

- The first run asks, once, whether your terminal may use the microphone. If that was refused the input is
  digital silence, and the tuner says so on stderr after two seconds.
- It uses a device as it is: it never changes the Mac's default devices, a buffer size or a sample rate.
  `--out thru` needs one device with both inputs and outputs (an interface); a Mac's microphone and its
  speakers are two devices.
- `capMs` and `tin` come from the device's reported latency, not a loopback measurement.
- `make live LIVE_BACKEND=jack` builds the JACK version on a Mac that has JACK.

- The number and needle show `steadyHz`; the strobe drifts at the fast reading. Rows are
  partials 1, 2 and 4, so a fine error shows first on the bottom row.
- A4 is shared by every viewer (430 to 450 Hz). The polyphonic strum check is not exposed.
- The footer measures input-to-screen latency across the Pi's and the viewer's clocks, and
  shows the Pi's CPU.

## Porting notes

- **Daisy Seed (STM32H750, Cortex-M7 480 MHz).** Use the core as is. It needs single-precision
  floats in the per-sample path, plus a few hundred double operations per second in the fits,
  which the M7's FPU handles in hardware.
- **ESP32-S3.** Its FPU is single precision only. The fit's double sums run in software, so a
  `float` build of `PhaseSlopeWindow` is the first port task. The MPM inner loop should go
  through ESP-DSP's dot product. RESULTS.md has the estimates.
- **Plugins and Pi.** Wrap `Tuner` in the plugin framework.
- **Web.** Done: `hosts/wasm` compiles the same headers with Emscripten and calls `process()`
  from the AudioWorklet's 128-frame callback (`make wasm`, `make wasm-test`).

## The standalone repository

[squatch-tuner](https://github.com/squatch-stack/squatch-tuner) is the easy-to-embed copy: the
same files in a flat layout (`include/`, `bench/`, `tests/`, `hosts/`) plus the built demo in
`docs/`. It is written from this repository by `tools/export-public tuner`, never edited by
hand: its `EXPORTED` file lists a SHA-256 per file and the squatch-dsp commit they came from,
and its CI fails if a file there differs from that list or from squatch-dsp's main branch.

## Licence

MIT, like the rest of squatch-dsp (`LICENSE` at the repository root).
