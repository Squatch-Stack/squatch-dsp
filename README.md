<!-- SPDX-License-Identifier: MIT -->
# squatch-dsp

A small real-time sound engine in C++17 for guitar effects: each block is written once and runs
on a Raspberry Pi, a microcontroller, in a browser and in desktop plugins, with a thin host for
each. Nothing in the audio path allocates, locks, does I/O or throws, and the tests prove it for
every block.

It holds the core (the `Block` interface, a memory arena, parameters with smoothing and a
lock-free queue), four effect blocks, the Squatch Tuner, and each of them as a desktop plugin.
[ROADMAP.md](ROADMAP.md) says where it is going.

**The Squatch Tuner** is developed here, in [`blocks/tuner/`](blocks/tuner/), and published on
its own as [squatch-tuner](https://github.com/squatch-stack/squatch-tuner): the easy-to-embed
copy, with a **[browser demo](https://squatch-stack.github.io/squatch-tuner/)** you can try
with a guitar or a microphone.

| Path | What |
|---|---|
| `include/squatch/dsp/` | the core: `Block`, `Arena`, `Setup`, `ParamDesc` and `Smoother`, `ParamQueue` (single producer, single consumer), `Latest` (newest-value snapshot), `DenormalGuard`, `Span`, `Chain`, and the heap-free factory by type id (`registry.hpp`) |
| `contracts/` | one control contract per block (`cc.squatch.control/1`): ids, keys, ranges, tapers, meters, identity. `tools/build-contracts` generates the C++ tables (`include/squatch/dsp/contracts/`) and the plugin list from them; see [contracts/README.md](contracts/README.md) |
| `include/squatch/dsp/blocks/`, `src/` | blocks: `Limiter` (lookahead, true-peak, with drive and output trim), `Gate` (noise gate with PiPedal's controls; ranges settled with the browser rig), `Eq` (RBJ low shelf, peak and high shelf), `Cabinet` (impulse response convolution with no added latency: a direct head and a uniformly partitioned FFT tail, on our own radix-2 `Fft`), `TunerBlock` (the tuner behind the `Block` interface) |
| `blocks/tuner/` | the Squatch Tuner: header-only core, measurement bench and [results](blocks/tuner/RESULTS.md) |
| `hosts/` | thin hosts: `clap/` (the desktop plugins: CLAP, and VST3 and AUv2 through clap-wrapper; [hosts/clap/README.md](hosts/clap/README.md)), `jack/` (the live tuner; the live rig, gate to limiter as one client; a loopback latency probe), `web/` (its page and a streaming bridge), `wasm/` (the tuner in the browser), `daisy/` (Daisy Seed3 firmware: passthrough, the gate to limiter chain with cycle counts, a latency probe, and a Neural Amp Modeler A2-Lite model running in real time on one core; [hosts/daisy/README.md](hosts/daisy/README.md)), `common/`, `tests/` |
| `tests/` | a small test runner, the real-time watch, double-precision references, golden renders |
| `bench/`, `results/` | CPU cost per block ([results/bench-mac.md](results/bench-mac.md)) |
| `docs/benchmarks/` | measured results with their method and how to repeat them: [A2-Lite on a Daisy Seed3](docs/benchmarks/daisy-seed3.md) (46.5% of one Cortex-M7 for the amp, 66.4% for the whole chain) |

## Rules

- **Real-time safe.** `process()` and `set()` never allocate, lock, do I/O or throw. All memory
  comes from the `Arena` in `prepare()`. `make test` checks this for every block, with hooks on
  operator new/delete, malloc/free and pthread and os_unfair locks.
- **C++17, with no exceptions and no RTTI in the core,** so the same code builds for a Cortex-M7
  and an ESP32-S3.
- **No fetched dependencies in the core.** Anything third-party is vendored, pinned and reviewed.
  There are no package managers and no build-time downloads. The plugin wrappers (`hosts/clap`)
  build against MIT/BSD code pinned by commit in `hosts/clap/deps.lock`, fetched once by
  `tools/fetch-plugin-deps`, never by the build.
- **Every block is checked against a reference:** a plain double-precision version of its
  definition, plus a golden render that catches any change of sound.
- **Small functions:** cyclomatic complexity of at most 10, at most 60 lines and 5 parameters per
  function (`make lint`), and conventional commit subjects.

The block contract (input and output, in place, block sizes, what `set()` may do) is at the top
of [`block.hpp`](include/squatch/dsp/block.hpp), with what version 2 added for plugin hosts:
`get()` for state and meters, channel layouts (mono, mono to stereo, linked stereo), `tail()`,
identity and version, and descriptors with a stable key, taper, steps and labels, and a role
(parameter, bypass or meter).

## Build and test

`clang++` or `g++`, and `make`. `lizard` is only needed for `make lint`.

```sh
make test       # ASan + UBSan unit, reference, golden and real-time tests; TSan for the queue
make lint       # complexity limits (lizard)
make bench      # writes results/bench-<machine>.md
make ci         # lint, a standalone compile of each header, the -O3 library, the tests
make golden     # rewrite tests/golden/*.inc, only after a deliberate change of sound
make live       # the live tuner and test tone: CoreAudio on macOS (nothing to install), JACK elsewhere (libjack-jackd2-dev)
make rig        # the JACK rig and latency probe; NAM_CORE=<NeuralAmpModelerCore checkout> adds the amp
make wasm       # the tuner's browser demo into build/web-demo (needs Emscripten)
make wasm-test  # the demo in Node and in headless Chrome, against exact test tones
make contracts  # regenerate the C++ parameter tables (and LV2/WAM/OSCQuery/MIDI-CI into build/contracts)
make plugins    # the desktop plugins into build/plugins/bundles (needs cmake and ninja)
make plugins-install   # copy them into $HOME/Library/Audio/Plug-Ins
make plugins-check     # sanitizers, clap-validator, VST3 validator, pluginval, auval
```

The Makefile uses clang++ where it exists and g++ otherwise; `CXX=...` overrides it. Where
ThreadSanitizer cannot run (a Pi 5's 47-bit address space), use `make test HOST_SAN=`.

## Adding a block

1. Write its contract, `contracts/<key>.params.json` (copy one), and run `tools/build-contracts`.
2. Put the header in `include/squatch/dsp/blocks/`, the code in `src/`, and declare the class
   `final : public Block, public contracts::<Name>Contract`: that brings the `Param` enum, the
   descriptor table `kParams`, `params()` and `kInfo`. `set()` clamps through the descriptor and
   smooths anything that would click; `get()` returns what `set()` kept, and each meter's newest
   value. Override `supports(Layout::Stereo)` only for a linked stereo path of its own.
3. Take every buffer in `prepare()` from the `Arena`. If an allocation returns null, stay silent in `process()`.
4. Support `in == out` and any `n` from 1 to `maxBlock`. Report `latency()` if the block delays the signal.
5. Keep filter state clear of denormals with `flushDenormal()` on the S3, which has no FTZ mode.
6. Write the reference in `tests/reference.hpp`: a plain double-precision version of the definition.
7. Add `tests/test_<block>.cpp` with behaviour tests, error against the reference and a golden render.
   Run `make golden` once, then check the new `.inc` file.
8. Add the block to `rtBlocksNeverAllocateOrLock` in `tests/test_rt.cpp`, to `registry.cpp`, and a row
   to `bench/bench.cpp`. For a plugin, give the contract an `auSubtype` and `features`.
9. `make ci` passes (and `make plugins-check` for a plugin), then commit with a subject like
   `feat(<block>): ...`.

## How this repository is published

squatch-dsp is developed in a working tree that also holds planning notes and scripts for our own
test bench. `tools/export-public dsp` publishes everything else here, file for file, and writes
`EXPORTED`: a SHA-256 for each published file. CI fails if a published file is
edited here directly (`tools/export-public dsp --verify .`). The same tool writes squatch-tuner
from this repository (`tools/export-public tuner`), and squatch-tuner's CI checks its copy of
the tuner against this repository's main branch, so the tuner has one source.

Issues and pull requests are welcome. A merged change is applied upstream and comes back with
the next export. Commit messages here carry no Co-Authored-By trailers, and commit dates are in
UTC (`TZ=UTC git commit`).

## Support this work

If this is useful to you, you can support its development through
[GitHub Sponsors](https://github.com/sponsors/squatch-stack).

<!-- Other ways to support, to add when the accounts exist:
  - Patreon
  - Ko-fi
  - Buy Me a Coffee
  - Liberapay
  - Open Collective
  Also uncomment the matching lines in .github/FUNDING.yml. -->

## Licence

MIT, © 2026 Squatch Stack, for the whole repository, the tuner included. See [LICENSE](LICENSE).
