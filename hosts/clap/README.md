<!-- SPDX-License-Identifier: MIT -->
# Desktop plugins: CLAP, VST3, AUv2

`ClapPlugin<Block>` (`clap_plugin.hpp`) turns any squatch-dsp block into a CLAP plugin;
clap-wrapper turns that into VST3 and AUv2. The plugin's identity, version, parameters, meters and
AU codes all come from the block's control contract (`contracts/`), through
`tools/build-contracts`. No custom GUI yet: hosts show their generic parameter view, meters
included (read-only parameters).

| Plugin | Block | CLAP id | AU | Latency at 48 kHz |
|---|---|---|---|---|
| Squatch Limiter | `Limiter` (linked stereo) | `cc.squatch.limiter` | `aufx SqLm SqSt` | 88 samples |
| Squatch Gate | `Gate` (linked stereo) | `cc.squatch.gate` | `aufx SqGt SqSt` | 0 |
| Squatch EQ | `Eq` (dual mono in stereo) | `cc.squatch.eq` | `aufx SqEq SqSt` | 0 |
| Squatch Tuner | `TunerBlock` (hears the mid in stereo) | `cc.squatch.tuner` | `aufx SqTn SqSt` | 0 |
| Squatch Cab | `Cabinet` (dual mono; response path in the state) | `cc.squatch.cabinet` | `aufx SqCb SqSt` | 0 |

Blocks that live only in the development tree add their plugins through
`generated/plugins-private.cmake` and their own test-host cases through `tests/extra_*.cpp`; they
build, install and run the same matrix as the plugins above, and neither file is exported.

## Build, install, check

```sh
tools/fetch-plugin-deps     # the pinned commits in deps.lock -> build/deps, validators -> build/tools
make plugins                # build/plugins/bundles: a .clap, .vst3 and .component per plugin
make plugins-install        # copies them into $HOME/Library/Audio/Plug-Ins/{CLAP,VST3,Components}
make plugins-check          # the whole matrix below; summary in build/reports/plugins-check.md
```

Nothing downloads during a build: CMake reads only `build/deps` (clap 1.2.10, clap-wrapper `next`
at e4fdc820, the VST3 SDK 3.8.0 build 66, AudioUnitSDK 1.1.0; all MIT except the Apache-2.0
AudioUnitSDK). Bump a pin in `deps.lock`, then run `make plugins-check` in full.

## What `make plugins-check` runs

- **The headless test host** (`tests/`, `squatch-clap-host`): every requirement case below on every
  plugin, linked in, and again over the built `.clap` bundles as a host loads them.
- **Sanitizer legs** of the test host: ASan+UBSan (the allocation watch then sees `malloc` too),
  TSan (the main thread reads, saves and loads state while the audio thread processes), and
  RealtimeSanitizer (Homebrew LLVM; every `process()` call runs in a nonblocking scope, also over
  the shipped bundles). Each leg first proves it can fail: `--prove-watch` runs a `process()` that
  allocates and locks, which the watch must count and RTSan must abort on.
- **clap-validator** 0.4.1, the **Steinberg VST3 validator** in extended mode, **pluginval** 1.0.4
  at strictness 10 (VST3 and AU; `PLUGINVAL_STRICTNESS` to change) and **auval -strict**.
  pluginval 1.0.4 has no `--rtcheck`; the RTSan leg covers that ground.

## The adapter's requirements, and the evidence

From real host bugs (research notes, frameworks review 2.1). Case names are in `tests/`.

| # | Requirement | How | Evidence |
|---|---|---|---|
| 1 | Configurable mono and stereo buses, re-read every buffer | `configurable-audio-ports` and `audio-ports-config`: 1->1, 1->2, 2->2. Each call's channel counts come from its buffers | `portConfigurations`; `blockSizesAndLayouts` sends mono buffers to stereo configurations, the reverse, and no input at all; clap-validator layout tests; auval lists exactly `[2,2] [1,1] [1,2]` and passes its 1-channel and 1-to-2 renders |
| 2 | Exactly one bypass; processing continues | the contract's one `role: bypass` is `CLAP_PARAM_IS_BYPASS`; the block keeps running, the output crossfades (10 ms) to the input delayed by the latency | `bypassKeepsProcessingAligned`: bypassed output equals the input `latency` samples late, bit for bit; released, it equals a never-bypassed plugin bit for bit; auval Bypass Effect |
| 3 | Latency set before activation, changed only via a restart request | fixed in `activate()`, reported to `clap_host_latency.changed` there if it moved; if a block's latency ever differed while active, `request_restart` | `latencyAndSampleRate`: an impulse sounds exactly the reported latency later, 200 varied calls leave it and the restart count alone, 44.1 -> 96 kHz reports the change during activation |
| 4 | Chunked state, including before activation | versioned text by contract key (`state_text.hpp`), loops over partial reads and writes, a load asks the host to rescan values | `stateBeforeActivation` (one-byte reads, three-byte writes, no activate, refuses another type, a newer version and an empty stream); clap-validator state-reproducibility (basic, binary, buffered) and invalid-state tests; pluginval state and restoration |
| 5 | Parameter flush while inactive | flush only updates the values `activate()` applies | `flushWhileInactive`; clap-validator param-set-events (flush equals process) |
| 6 | Tolerate deactivate without stop_processing | `deactivate()` clears the processing flag itself | `latencyAndSampleRate` deactivates without stopping and reactivates |
| 7 | Max block size is a bound | events cut the buffer sample-accurately, and no stretch exceeds the activated maximum | `blockSizesAndLayouts`: 1, 512, random sizes, and a 1000-frame call on a 512 maximum; clap-validator random and varying block sizes; auval Bad Max Frames |
| 8 | Read the host-information extension | `clap-wrapper.host-information/0` at `init()`: wrapper flavour and the real host's name, logged | `hostInformation` (wrapped as VST3, and plain CLAP) |
| 9 | Model/IR swap off-thread with a short crossfade | `banks.hpp`: a loader thread prepares a second bank; the audio thread takes it with one atomic exchange and crossfades 20 ms; a failed file keeps the current one and is reported through `request_callback` | `fileSwapsOffThread`: no step over 0.002 during the swap, the missing file logged, the old response kept; the audio thread's watch stays at zero while the loader allocates |
| 10 | A block's learned state and trigger parameters | `learned.hpp`: after a buffer in which a block's learned state changed, the audio thread copies it through a lock-free triple buffer; a save writes it beside the parameters (`learned N <hex>`), a load hands it back the same way. A state is refused if the block refuses its learned state; one without learned state keeps the current one. A `Role::Trigger` parameter reaches the block only as an event, never from a state load or a re-apply | cases for the blocks that learn (`tests/extra_*.cpp`): learned through the plugin, restored before activation, while active, at a new rate, in another layout, and across threads under TSan |
| - | Real-time safe `process()` | no allocation, lock or I/O; lazy symbol binding disabled (`-bind_at_load`), found by the lock watch | every `process()` call in every case is watched: 0 allocations, frees and locks; 0 RTSan reports, also over the shipped bundles; 0 TSan reports |

## Known, outside this repository

- clap-validator's `param-set-events` and `param-set-wrong-namespace` compare every parameter,
  read-only meters included, after processing audio. Meters publish at their contract rate (30 Hz)
  from one period after activation, so the validator's single 512-sample buffer never moves them,
  but a longer run would. Worth a clap-validator issue: skip `CLAP_PARAM_IS_READONLY` there.
- clap-wrapper's AUv2 `SupportsTail()` returns false although `GetTailTime()` reads `clap.tail`
  (auval: "Tail Time: Recommended Property is not supported"). A one-line upstream fix.
- Re-running CMake's configure step regenerates an AU bundle's default `Info.plist`, and
  clap-wrapper only copies its generated one when the AU relinks. `finish-bundles.cmake` restores
  it after every build and signs the bundles ad hoc.

## Real hosts

`tests/reaper-smoke.lua` runs inside REAPER: a new project tab with every installed Squatch plugin
on its own track, fed an A2 sine 13 cents sharp for 4 s with the master at -inf, then prints each
plugin's parameters as REAPER formats them (the tuner's Note should read A).
