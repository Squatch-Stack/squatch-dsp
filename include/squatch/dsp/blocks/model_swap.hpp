// SPDX-License-Identifier: MIT
// ModelSwap: two interchangeable blocks (two NAM amp models, two cabinet responses), one playing
// and one standing by, with a click-free handover from the first to the second while audio runs.
//
// Adapted from the model-swap plan in namchain by Rik Hemsley (MIT), pinned to e208bb8: prepare
// the new model off the audio thread, warm it over the model's lookback (the receptive field),
// then a short equal-power crossfade (Crossfade, adapted from the same repository):
//   https://github.com/pedal-science/namchain/blob/e208bb869069b15a7331b452e03b7dc88726ffee/docs/transitions.md
//   https://github.com/pedal-science/namchain/blob/e208bb869069b15a7331b452e03b7dc88726ffee/include/namchain/lookback.h
//   https://github.com/pedal-science/namchain/blob/e208bb869069b15a7331b452e03b7dc88726ffee/include/namchain/rt/crossfade.h
// Copyright (c) 2026 Rik Hemsley; the licence text is in THIRD_PARTY_NOTICES.md. His own
// warm start was still to be built (his transitions.md, "M3"); this is ours: the standby block is
// fed the live input and its output thrown away for warmFrames, so its state is the state the
// playing block has, with no burst of work on the audio thread; both then run through the fade.
// The level stays steady because Crossfade measures how alike the two outputs are (two amp models
// playing the same guitar are strongly correlated, where equal power alone adds 3 dB).
//
// Who touches what. The control thread owns the standby block only between standby() and
// publish() (or cancel()); the audio thread owns everything else. The handoff is one atomic with
// acquire/release ordering, so neither side ever waits and nothing is allocated or locked on the
// audio thread. The standby block is loaded and prepared by the caller, with an arena of its own
// (a block takes its memory in prepare(), see block.hpp), and any parameter it needs is replayed
// from the last values set() saw when the swap begins.
//
//   Block* b = swap.standby();        // control thread; null while a swap is under way
//   load(*b); b->prepare(setup, mem2);
//   swap.publish();                   // the audio thread takes it from the next process()
//
// The block that was playing becomes the standby once the fade ends (idle() is true again), and
// the caller may reuse its arena then, not before.
#pragma once
#include <atomic>
#include <squatch/dsp/block.hpp>
#include <squatch/dsp/crossfade.hpp>
#include <squatch/dsp/param.hpp>

namespace squatch::dsp {

class ModelSwap final : public Block {
 public:
  static constexpr float kFadeMs = 20.0f;
  static constexpr int kMaxParams = 16;

  /// `warmFrames` is how long the standby runs on the live input before the fade (the new
  /// model's receptive field, or more); it is not audible. Neither block is owned; `a` plays first.
  ModelSwap(Block& a, Block& b, int warmFrames = 0) : block_{&a, &b}, warm_(warmFrames < 0 ? 0 : warmFrames) {}

  /// Prepares both blocks from `mem` (the standby's memory is whatever it was given here until
  /// the caller prepares it again from its own arena).
  void prepare(const Setup& s, Arena& mem) override;
  void process(const float* in, float* out, int n) override;
  void reset() override;
  void set(ParamId id, float value) override;
  float get(ParamId id) const override { return block_[active_.load(std::memory_order_relaxed)]->get(id); }
  int latency() const override { return block_[active_.load(std::memory_order_relaxed)]->latency(); }

  /// Control thread. The block to load the new model into, or null while a swap is under way (or
  /// before prepare()). Between this and publish()/cancel() the audio thread never touches it.
  Block* standby();
  /// Control thread. The standby is loaded and prepared: swap to it, warming it for `warmFrames`
  /// first (the new model's receptive field; negative keeps the current setting). Returns false
  /// if standby() was not taken first.
  bool publish(int warmFrames = -1);
  /// Control thread. Gives the standby back without swapping.
  void cancel();

  bool idle() const { return state_.load(std::memory_order_acquire) == Idle; }
  bool swapping() const { return state_.load(std::memory_order_acquire) >= Requested; }
  /// Swaps finished since prepare().
  long swaps() const { return swaps_.load(std::memory_order_relaxed); }
  /// Which of the two constructor blocks is playing: 0 for `a`, 1 for `b`.
  int playing() const { return active_.load(std::memory_order_relaxed); }

 private:
  enum State : int { Idle, Loading, Requested, Warming, Fading };
  void begin();
  void finish();

  Block* block_[2];
  std::atomic<int> warm_;
  std::atomic<int> state_{Idle};
  std::atomic<int> active_{0};
  std::atomic<long> swaps_{0};
  Crossfade fade_;
  float* scratch_ = nullptr;
  float* wet_ = nullptr;
  int fadeFrames_ = 0;
  int warmLeft_ = 0;
  bool ready_ = false;
  float value_[kMaxParams] = {};
  bool known_[kMaxParams] = {};
};

}  // namespace squatch::dsp
