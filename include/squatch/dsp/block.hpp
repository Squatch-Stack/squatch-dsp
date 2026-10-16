// SPDX-License-Identifier: MIT
// The one interface every block implements (ROADMAP.md).
//
// The contract, which the tests enforce for every block:
// - prepare() takes all memory from the arena and may be slow; it is never called while audio runs.
//   If the arena runs out, the block stays silent (writes zeros) until prepared with enough.
// - process() is real-time safe: no allocation, locks, I/O or exceptions. 1 <= n <= maxBlock,
//   and `in` may equal `out` (in-place).
// - set() is real-time safe too and is called on the audio thread; other threads reach it through
//   a ParamQueue. Values are clamped to the descriptor's range and smoothed inside the block.
// - reset() clears state (delay lines, envelopes) but keeps memory and parameters.
//
// Version 2 of the interface (2026-09) adds what plugin hosts need, without touching the rules
// above. Everything new is either a default the old blocks inherit or a pure query:
// - get(id): the value last given to set() (after clamping), so a host can save state; for a
//   meter (Role::Meter in the descriptor) the newest measurement. NaN for an id the block does
//   not hold (the bypass, which the host implements). Same threads as set().
// - parameters(), info(): the block's descriptor table and identity (type id, name, version and
//   state version) through the instance, for generic hosts. Both come from the block's control
//   contract (contracts/<key>.params.json, compiled by tools/build-contracts).
// - Layout and processChannels(): declared channel layouts. Every block handles Mono (1 -> 1) and
//   MonoToStereo (1 -> 2, the mono result copied to the right); supports(Stereo) is true only
//   for blocks with a linked stereo path of their own (a gate or limiter must not let the two
//   sides disagree). For the rest a host runs one instance per channel (dual mono). The layout
//   is fixed at prepare() (Setup::layout); processChannels() is told the channel counts of every
//   call, so a host that hands over fewer channels than configured is still safe.
//   stereoOutput() (2026-10, parity with the browser rig's stereo reverb) is true for a block whose
//   second output is its own signal rather than a copy of the first: a decorrelated reverb. It
//   changes nothing else: the first output is still the Mono path, a host that ignores the query
//   gets a valid, only less wide, result, and every block that does not override it stays false.
// - tail(): samples of output that may follow the last non-silent input (kInfiniteTail if
//   unbounded), counted from the input, so it includes latency(). Conservative by design.
// - Learned state: a block that learns something a host must keep (a noise profile) hands it
//   over as one opaque blob of at most kMaxBlockState bytes, which carries its own format and
//   version. saveState() copies it out (0: nothing learned); loadState() adopts one, or refuses
//   it (false, nothing changed); stateSerial() changes whenever it does, so a host can poll it
//   on the audio thread and hand a copy to the thread that saves. Same threads as set(), real-time
//   safe, and loadState() works before prepare() too. Blocks that learn nothing inherit all three.
// - The heap-free factory by type id lives in registry.hpp.
// Multi-port blocks (a side-chain key, a send) are a later Layout value; no block needs one yet.
#pragma once
#include <cstdint>

#include "arena.hpp"
#include "param.hpp"
#include "span.hpp"

namespace squatch::dsp {

/// Channel layouts, input -> output. Appended to, never reordered: hosts store them.
enum class Layout : std::uint8_t { Mono, MonoToStereo, Stereo };

constexpr int inputChannels(Layout l) { return l == Layout::Stereo ? 2 : 1; }
constexpr int outputChannels(Layout l) { return l == Layout::Mono ? 1 : 2; }

struct Setup {
  double sampleRate = 48000.0;
  int maxBlock = 128;  // the most samples one process() call will ever be given
  Layout layout = Layout::Mono;  // v2; blocks without supports(Stereo) are always prepared Mono
};

/// Who a block is. The type id is the contract's product id and never changes; version is the
/// semantic version of its sound and parameters; stateVersion goes up whenever a saved value
/// would be read differently (a host migrates or refuses older states by it).
struct BlockInfo {
  const char* typeId = "";   // reverse DNS, e.g. "cc.squatch.gate"
  const char* name = "";     // display name, e.g. "Squatch Gate"
  const char* vendor = "";
  const char* version = "0.0.0";
  std::uint32_t stateVersion = 0;
};

/// The channels of one processChannels() call. Pointers are never null for a channel counted.
struct AudioIO {
  const float* const* in;
  float* const* out;
  int inChannels;
  int outChannels;
};

constexpr int kInfiniteTail = -1;
constexpr int kMaxBlockState = 1024;  // bytes of learned state a block may hand over

struct Block {
  virtual void prepare(const Setup& s, Arena& mem) = 0;
  virtual void process(const float* in, float* out, int n) = 0;
  virtual void reset() = 0;
  virtual void set(ParamId id, float value) = 0;
  virtual float get(ParamId id) const = 0;  // v2
  virtual int latency() const { return 0; }  // samples, for aligning parallel branches

  // v2, with defaults for blocks that have nothing to declare.
  virtual int tail() const { return latency(); }
  virtual bool supports(Layout l) const { return l != Layout::Stereo; }
  /// True when output 1 of a 2-output call is the block's own signal, not a copy of output 0
  /// (see the note on stereoOutput() above). Output 0 always equals the Mono path.
  virtual bool stereoOutput() const { return false; }
  virtual Span<const ParamDesc> parameters() const;
  virtual const BlockInfo& info() const;
  virtual std::uint32_t stateSerial() const { return 0; }
  virtual int saveState(void* /*out*/, int /*capacity*/) const { return 0; }
  virtual bool loadState(const void* /*data*/, int /*bytes*/) { return false; }

  /// Any declared layout: Mono and MonoToStereo run process() on the first channel and copy the
  /// result to every further output. Blocks that support Stereo override this. `in` may equal
  /// `out` channel for channel.
  virtual void processChannels(const AudioIO& io, int n);

 protected:
  // Blocks live in static or host-owned storage and are never deleted through this interface,
  // so no virtual destructor (which would pull operator delete into bare-metal builds).
  ~Block() = default;
};

/// A list of block types, as the heap-free factory (registry.hpp) takes them.
template <class... T>
struct BlockList {};

inline Span<const ParamDesc> Block::parameters() const { return {}; }

inline const BlockInfo& Block::info() const {
  static constexpr BlockInfo kUnnamed{};
  return kUnnamed;
}

inline void Block::processChannels(const AudioIO& io, int n) {
  if (io.inChannels < 1 || io.outChannels < 1) return;
  process(io.in[0], io.out[0], n);
  for (int c = 1; c < io.outChannels; ++c) copy(io.out[0], io.out[c], n);
}

}  // namespace squatch::dsp
