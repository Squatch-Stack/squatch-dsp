<!-- SPDX-License-Identifier: MIT -->
# squatch-dsp roadmap

## Goals

- **Write each effect once.** A gate, a drive or a reverb should sound the same on a Raspberry
  Pi, on a microcontroller pedal, in a browser and in a desktop plugin, because it is the same
  code. Each target gets a thin host; the blocks do not know which one they run in.
- **Real-time safe everywhere.** No allocation, locks, I/O or exceptions in the audio path, and
  a test that proves it for every block.
- **Checked, not assumed.** Every block has a reference to match (a double-precision render of
  its definition, or an established implementation such as NAM Core for amp models) and a
  golden render that catches any change of sound.
- **Small enough to trust.** C++17 without exceptions or RTTI, no fetched dependencies, small
  functions, and measured CPU cost per block on each target.

## The block interface

Every block implements one small interface (`include/squatch/dsp/block.hpp`, version 2):

```cpp
struct Block {
  virtual void prepare(const Setup& s, Arena& mem) = 0;  // rate, max block, layout; all memory taken here
  virtual void process(const float* in, float* out, int n) = 0;  // real-time safe, mono
  virtual void reset() = 0;                                // clear state, keep memory
  virtual void set(ParamId id, float value) = 0;           // clamped and smoothed inside the block
  virtual float get(ParamId id) const = 0;                 // what set() kept; a meter's newest value
  virtual int latency() const { return 0; }                // samples, for aligning parallel branches
  virtual int tail() const { return latency(); }           // samples after the input goes silent
  virtual bool supports(Layout) const;                     // Mono, MonoToStereo; Stereo if linked
  virtual void processChannels(const AudioIO& io, int n);  // any declared layout
  virtual Span<const ParamDesc> parameters() const;        // from the block's control contract
  virtual const BlockInfo& info() const;                   // type id, name, version, state version
};
```

Other threads change parameters through a single-producer queue, and blocks that only measure,
such as the tuner, publish their readings through a lock-free snapshot and as meters. A block's
parameters come from its control contract (`contracts/`), which also drives the LV2, WAM,
OSCQuery and MIDI-CI descriptions, so a preset reads the same on every target.

## Milestones

1. **Core and first blocks** (done). The `Block`, `Arena` and parameter core; a lookahead
   true-peak limiter, a noise gate and a three-band EQ; the Squatch Tuner as a block, with a
   live JACK host on a Raspberry Pi and a WebAssembly host in the browser.
2. **A microcontroller host.** The same blocks on a Cortex-M7 board (Daisy Seed), with the
   round-trip latency and the CPU per block measured on the board.
3. **Amp models.** A lean inference engine for NAM models with fixed memory, checked against
   NAM Core's output, on the Pi and on the microcontroller.
4. **Cabinets and routing.** Cabinet impulse responses (partitioned convolution), a fixed-size
   graph for series and parallel routing with latency alignment, a shared preset format and
   click-free preset switching. An LV2 plugin that hosts a whole board.
5. **More blocks.** Compressor, drives, envelope filter, delay and reverb, each with its
   reference and golden render.
6. **Browser and desktop.** The whole engine as WebAssembly in an AudioWorklet, and as desktop
   plugins. Started: the limiter, gate, EQ, tuner and cabinet are CLAP, VST3 and AUv2 plugins
   (`hosts/clap`), checked by clap-validator, the VST3 validator, pluginval, auval and sanitizer
   legs. Next: a native GUI, LV2 from the same contracts, and the iOS AUv3 spike.

## Open questions

- A small FFT of our own for convolution, or a vendored one: decided by measurement.
- Oversampling for the drives, 2x or 4x per block: measured for aliasing against CPU.
- The parameter-smoothing policy, and the control map for MIDI.
