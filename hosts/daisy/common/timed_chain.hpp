// SPDX-License-Identifier: MIT
// Blocks in series, like squatch::dsp::Chain, with a cycle count taken around every block (the
// whole callback is timed by the host, see meter.hpp). Clock is any type with a static `std::uint32_t now()`: the DWT cycle
// counter on the board, a fake clock in the host tests. Wrapping subtraction makes a counter
// overflow between two reads harmless.
//
// process() runs on the audio thread, with flush-to-zero on (DenormalGuard), and records into
// the statistics it is handed: the Meter's current bank (meter.hpp), stage i into stats[i].
#pragma once
#include <cstdint>
#include <squatch/dsp/block.hpp>
#include <squatch/dsp/denormal.hpp>
#include <squatch/dsp/span.hpp>

#include "cycle_stats.hpp"

namespace squatch::board {

template <class Clock, int MaxStages = 8>
class TimedChain {
 public:
  static constexpr int kMaxStages = MaxStages;

  /// Setup only. False when full.
  bool add(dsp::Block& b, const char* name) {
    if (count_ == kMaxStages) return false;
    blocks_[count_] = &b;
    names_[count_++] = name;
    return true;
  }

  void prepare(const dsp::Setup& s, dsp::Arena& mem) {
    for (int i = 0; i < count_; ++i) blocks_[i]->prepare(s, mem);
  }

  /// in == out allowed. n <= the prepared maxBlock. stats holds size() entries.
  void process(const float* in, float* out, int n, CycleStats* stats) {
    dsp::DenormalGuard ftz;
    std::uint32_t mark = Clock::now();
    if (count_ == 0) dsp::copy(in, out, n);
    for (int i = 0; i < count_; ++i) {
      blocks_[i]->process(i == 0 ? in : out, out, n);
      const std::uint32_t t = Clock::now();
      stats[i].record(t - mark);
      mark = t;
    }
  }

  int size() const { return count_; }
  const char* name(int i) const { return names_[i]; }
  dsp::Block& block(int i) { return *blocks_[i]; }

  int latency() const {
    int sum = 0;
    for (int i = 0; i < count_; ++i) sum += blocks_[i]->latency();
    return sum;
  }

 private:
  dsp::Block* blocks_[kMaxStages] = {};
  const char* names_[kMaxStages] = {};
  int count_ = 0;
};

}  // namespace squatch::board
