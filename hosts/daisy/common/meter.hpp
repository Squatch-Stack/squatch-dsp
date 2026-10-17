// SPDX-License-Identifier: MIT
// The measurement every Seed3 firmware reports once a second: cycles per callback and per stage,
// the time between callbacks, late and overrunning callbacks, and peak levels.
//
// Two banks of statistics. The audio callback records into the active bank; at the end of each
// window (a whole number of blocks, one second of audio) it flips to the other bank and marks
// the finished one ready. The reporting loop summarises the ready bank, clears it and releases
// it. The callback never waits and never summarises (a 256-bin walk would show up in the worst
// case it is measuring); if the loop has not released the last bank, the window just runs on and
// `stretched` says so. One audio thread, one reporting thread.
#pragma once
#include <atomic>
#include <cmath>
#include <cstdint>

#include "cycle_stats.hpp"

namespace squatch::board {

template <int Slots>
struct Window {
  std::uint32_t seq = 0;        // windows reported so far, from 1
  std::uint32_t blocks = 0;     // callbacks in this window
  std::uint32_t late = 0;       // callbacks that started over 1.5 block periods after the last
  std::uint32_t overruns = 0;   // callbacks whose own work took longer than a block period
  std::uint32_t stretched = 0;  // windows merged into this one because the reader was slow
  float inPeak = 0.0f, outPeak = 0.0f;
  CycleSummary slot[Slots];
};

template <int Slots>
class Meter {
 public:
  static_assert(Slots >= 2, "slot 0 is the callback, slot 1 the period");
  static constexpr int kCallback = 0, kPeriod = 1, kFirstStage = 2;

  /// Setup only, before audio starts.
  void configure(std::uint32_t budgetCycles, std::uint32_t blocksPerWindow) {
    budget_ = budgetCycles;
    blocksPerWindow_ = blocksPerWindow ? blocksPerWindow : 1;
    for (Bank& b : banks_) b.clear(budgetCycles);
  }

  /// Audio thread: the active bank's statistics, slot kFirstStage + i for stage i.
  CycleStats* stats() { return banks_[active_].slot; }

  /// Audio thread, once per callback. `start` is the callback's first clock reading, `work` its
  /// cycles. Peaks are the block's absolute peaks in and out.
  void endBlock(std::uint32_t start, std::uint32_t work, float inPeak, float outPeak) {
    Bank& b = banks_[active_];
    if (havePrevious_) notePeriod(b, start - previousStart_);
    previousStart_ = start;
    havePrevious_ = true;
    b.slot[kCallback].record(work);
    if (work > budget_) ++b.overruns;
    b.inPeak = std::fmax(b.inPeak, inPeak);
    b.outPeak = std::fmax(b.outPeak, outPeak);
    ++b.blocks;
    if (++sinceFlip_ >= blocksPerWindow_) flip();
  }

  /// Reporting thread: true when a finished window was ready; it is then in `w`.
  bool take(Window<Slots>& w) {
    const std::uint32_t ready = ready_.load(std::memory_order_acquire);
    if (ready == 0) return false;
    Bank& b = banks_[ready - 1];
    w.seq = ++seq_;
    w.blocks = b.blocks;
    w.late = b.late;
    w.overruns = b.overruns;
    w.stretched = b.stretched;
    w.inPeak = b.inPeak;
    w.outPeak = b.outPeak;
    for (int i = 0; i < Slots; ++i) w.slot[i] = b.slot[i].summarize();
    b.clear(budget_);
    ready_.store(0, std::memory_order_release);
    return true;
  }

 private:
  struct Bank {
    CycleStats slot[Slots];
    std::uint32_t blocks = 0, late = 0, overruns = 0, stretched = 0;
    float inPeak = 0.0f, outPeak = 0.0f;

    void clear(std::uint32_t budget) {
      for (CycleStats& s : slot) s.configure(budget);
      blocks = late = overruns = stretched = 0;
      inPeak = outPeak = 0.0f;
    }
  };

  void notePeriod(Bank& b, std::uint32_t period) {
    b.slot[kPeriod].record(period);
    if (period > budget_ + budget_ / 2) ++b.late;
  }

  void flip() {
    sinceFlip_ = 0;
    if (ready_.load(std::memory_order_acquire) != 0) {
      ++banks_[active_].stretched;  // the reader still holds the other bank: keep this one going
      return;
    }
    ready_.store(static_cast<std::uint32_t>(active_ + 1), std::memory_order_release);
    active_ ^= 1;
  }

  Bank banks_[2];
  std::atomic<std::uint32_t> ready_{0};  // 0, or 1 + the index of the bank waiting to be read
  int active_ = 0;                       // audio thread only
  std::uint32_t budget_ = 0, blocksPerWindow_ = 1;
  std::uint32_t previousStart_ = 0, sinceFlip_ = 0;  // audio thread only
  bool havePrevious_ = false;
  std::uint32_t seq_ = 0;  // reporting thread only
};

}  // namespace squatch::board
