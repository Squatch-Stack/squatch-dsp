// SPDX-License-Identifier: MIT
// Crossfade: a click-free handover between two mono signals (a block switched in or out, a
// cabinet response swapped), with weights that keep the level steady.
//
// Adapted from rt::Crossfade in pedal-science/namchain by Rik Hemsley (MIT), pinned to e208bb8:
//   https://github.com/pedal-science/namchain/blob/e208bb869069b15a7331b452e03b7dc88726ffee/include/namchain/rt/crossfade.h
//   https://github.com/pedal-science/namchain/blob/e208bb869069b15a7331b452e03b7dc88726ffee/src/rt/crossfade.cpp
// Copyright (c) 2026 Rik Hemsley; the licence text is in THIRD_PARTY_NOTICES.md. Changes here: squatch namespace and naming, the weight
// tables come from the Arena in prepare() instead of std::vector, an unprepared or failed
// fade hands over at once instead of reading null tables, the output is flushed of
// denormals, and the correlation is a running estimate rather than the first block's. The
// weights and the normalisation law are unchanged.
//
// Equal power, not linear: for two unrelated signals the weights cos and sin of a quarter turn
// keep from^2 + to^2 == 1 all the way, where a linear fade dips 3 dB in the middle. But for two
// signals carrying the SAME audio (a bypass of a linear block, an EQ ramp) equal power adds 3 dB
// at the midpoint: a thump, continuous, so nothing that looks for steps sees it. So both weights
// are divided by sqrt(1 + 2*rho*from*to), with rho the correlation of the two signals: equal
// power at rho 0, constant amplitude at rho 1. By default rho is measured as the fade runs (a
// running estimate, sample by sample, so block size cannot change the result), and clamped to [0, 1] (a negative estimate would amplify the fade).
//
// prepare() is setup time. start() and mix() are real-time safe: no allocation, lock or call
// into the system.
#pragma once
#include "arena.hpp"

namespace squatch::dsp {

class Crossfade {
 public:
  /// Pass as `rho` to start() to measure the correlation as the fade runs: a running estimate
  /// over every sample mixed so far, so the result is the same for any block size.
  static constexpr double kMeasureCorrelation = -1.0;


  /// Setup time. Takes 2 * (maxFrames + 1) floats from the arena. `maxFrames` is the longest
  /// fade the tables cover; a longer start() is shortened, not refused. If the arena runs out
  /// the fade stays unprepared and every handover is immediate (ready() is false).
  void prepare(int maxFrames, Arena& mem);

  /// Begins a fade over `frames` (clamped to [1, maxFrames]). `rho` is the correlation of the
  /// two signals in [0, 1], or kMeasureCorrelation. Does nothing when not ready().
  void start(int frames, double rho = kMeasureCorrelation);

  /// Mixes `a` (fading out) and `b` (fading in) into `out`; `out` may equal `a` or `b`. Once
  /// the fade closes inside the block the rest is `b` alone, so one call also yields the
  /// settled signal. Not active, or not ready: `out` is `b`.
  void mix(const float* a, const float* b, float* out, int n);

  /// Ends the fade with the new signal at full weight.
  void finish() {
    position_ = length_;
    remaining_ = 0;
  }

  bool ready() const { return from_ != nullptr; }
  bool active() const { return remaining_ > 0; }
  int remaining() const { return remaining_; }
  int length() const { return length_; }           // fade in force, in frames; 0 before start()
  int maxFrames() const { return maxFrames_; }
  double correlation() const { return rho_; }       // what start() was given, or the running estimate

 private:
  void measure(float a, float b);
  float* from_ = nullptr;  // weight of the fading-out signal, by table position
  float* to_ = nullptr;    // weight of the fading-in signal
  int maxFrames_ = 0;
  int length_ = 0;
  int position_ = 0;
  int remaining_ = 0;
  float rho_ = 0.0f;
  bool measure_ = false;   // rho follows the signals, sample by sample, while the fade runs
  double ab_ = 0.0, aa_ = 0.0, bb_ = 0.0;  // running sums since start()
};

}  // namespace squatch::dsp
