// SPDX-License-Identifier: MIT
// Cycle statistics for one measured stage over a reporting window: count, mean, 99th percentile
// and worst, as cycles and as a share of the block's budget (the cycles one block of audio lasts).
//
// record() is O(1) and allocation-free, for the audio callback. The 99th percentile comes from a
// histogram of kBins bins spanning 0 to 2x the budget, so it is exact to one bin: 1/128 of the
// budget (0.78%). Anything over 2x the budget lands in the last bin; worst is always exact.
// Pure C++: the same code is tested on the host (hosts/daisy/tests) and runs on the board.
#pragma once
#include <cstdint>
#include <cstdio>

namespace squatch::board {

struct CycleSummary {
  std::uint32_t count = 0;
  std::uint32_t budget = 0;  // cycles per block
  std::uint32_t mean = 0, p99 = 0, worst = 0;
};

class CycleStats {
 public:
  static constexpr int kBins = 256;

  /// Setup only. budgetCycles > 0.
  void configure(std::uint32_t budgetCycles) {
    budget_ = budgetCycles;
    binWidth_ = (2ull * budgetCycles + kBins - 1) / kBins;
    if (binWidth_ == 0) binWidth_ = 1;
    clear();
  }

  void record(std::uint32_t cycles) {
    std::uint64_t bin = cycles / binWidth_;
    ++hist_[bin < kBins ? bin : kBins - 1];
    sum_ += cycles;
    if (cycles > worst_) worst_ = cycles;
    ++count_;
  }

  CycleSummary summarize() const {
    CycleSummary s;
    s.count = count_;
    s.budget = budget_;
    s.worst = worst_;
    if (count_ == 0) return s;
    s.mean = static_cast<std::uint32_t>(sum_ / count_);
    s.p99 = percentile(99);
    return s;
  }

  void clear() {
    for (std::uint32_t& h : hist_) h = 0;
    sum_ = 0;
    worst_ = 0;
    count_ = 0;
  }

 private:
  // The upper edge of the bin holding the given percentile, capped at the worst value seen.
  std::uint32_t percentile(int pct) const {
    const std::uint64_t rank = (static_cast<std::uint64_t>(count_) * pct + 99) / 100;
    std::uint64_t seen = 0;
    for (int b = 0; b < kBins; ++b) {
      seen += hist_[b];
      if (seen >= rank) {
        const std::uint64_t edge = static_cast<std::uint64_t>(b + 1) * binWidth_;
        return edge < worst_ ? static_cast<std::uint32_t>(edge) : worst_;
      }
    }
    return worst_;
  }

  std::uint32_t hist_[kBins] = {};
  std::uint64_t sum_ = 0;
  std::uint64_t binWidth_ = 1;
  std::uint32_t budget_ = 0, worst_ = 0, count_ = 0;
};

/// Hundredths of a percent of the budget: 1234 means 12.34%.
inline std::uint32_t basisPoints(std::uint32_t cycles, std::uint32_t budget) {
  return budget ? static_cast<std::uint32_t>((10000ull * cycles + budget / 2) / budget) : 0;
}

/// "12.34" into buf (no float printf on the board). Returns buf.
inline char* formatPercent(char* buf, std::size_t size, std::uint32_t cycles, std::uint32_t budget) {
  const std::uint32_t bp = basisPoints(cycles, budget);
  std::snprintf(buf, size, "%lu.%02lu", static_cast<unsigned long>(bp / 100), static_cast<unsigned long>(bp % 100));
  return buf;
}

/// "name mean/p99/worst %" as one field of a report line, e.g. "drive 5.10/5.22/5.80".
inline int formatSummary(char* buf, std::size_t size, const char* name, const CycleSummary& s) {
  char mean[24], p99[24], worst[24];
  return std::snprintf(buf, size, "%s %s/%s/%s", name, formatPercent(mean, sizeof mean, s.mean, s.budget),
                       formatPercent(p99, sizeof p99, s.p99, s.budget), formatPercent(worst, sizeof worst, s.worst, s.budget));
}

}  // namespace squatch::board
