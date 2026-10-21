// SPDX-License-Identifier: MIT
// The benchmark measurement method of pedal-science_NAMBench, ported for `make bench`.
//
// Adapted from pedal-science/NAMBench by Rik Hemsley (MIT), pinned to 0a5ef00: the window, the
// agreement test, the retry and the core percentage from
//   https://github.com/pedal-science/NAMBench/blob/0a5ef0086852294f8450606e78c44760e1055e35/Tools/nb_protocol.h
// and the per-block percentile from
//   https://github.com/pedal-science/NAMBench/blob/0a5ef0086852294f8450606e78c44760e1055e35/Tools/nam_ir_benchmark.cpp
// Copyright (c) 2026 Rik Hemsley; the licence text is in THIRD_PARTY_NOTICES.md.
// Left out: WAV loading, counters, Bencher output, the retry-on-parity loop and everything that
// knows about NAM models. Added: the pass is a function the caller supplies, nothing else.
//
// The method:
// - A pass runs a fixed amount of audio and returns how long it took. Warm up for a fixed time
//   first, then time passes for a fixed window (at least minSamples of them).
// - Interference only ever makes a pass SLOWER, so the pass times are not trimmed from both ends.
//   The kept set is the tightest run of sorted times that holds at least 70% of them (a
//   one-sided window); its spread, (max - min) / min, must be within 3%, or the attempt is
//   voided and run again, up to maxAttempts. A run that never agrees reports no number.
// - core_percent = mean seconds per pass / seconds of audio per pass * 100: the share of one core
//   needed to keep up in real time, independent of how long the test signal happens to be.
// - A mean hides bursts: partitioned convolution does a whole FFT in one callback. So also the
//   99th percentile of the per-block time as a percentage of that block's own deadline (its
//   audio duration). Above 100% a callback missed its deadline.
#pragma once
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <limits>
#include <vector>

namespace bench {

struct Window {
  std::vector<double> kept;
  double spread = 0.0;
  double mean = 0.0;
  double min = 0.0, max = 0.0;
  bool valid = false;
};

/// The tightest interval of the sorted samples holding at least ceil(fraction * n) of them.
/// Invalid for no samples, a fraction outside (0, 1], or when every window starts at a
/// non-positive time (a spread is relative to its minimum).
inline Window tightestWindow(std::vector<double> samples, double fraction) {
  Window w;
  if (samples.empty() || !(fraction > 0.0) || fraction > 1.0) return w;
  std::sort(samples.begin(), samples.end());
  const std::size_t width = std::max<std::size_t>(1, static_cast<std::size_t>(std::ceil(fraction * static_cast<double>(samples.size()))));
  std::size_t best = 0;
  double bestSpread = std::numeric_limits<double>::infinity();
  for (std::size_t s = 0; s + width <= samples.size(); ++s) {
    const double lo = samples[s], hi = samples[s + width - 1];
    if (!(lo > 0.0)) continue;
    const double spread = (hi - lo) / lo;
    if (spread < bestSpread) bestSpread = spread, best = s;
  }
  if (!std::isfinite(bestSpread)) return w;
  w.kept.assign(samples.begin() + static_cast<std::ptrdiff_t>(best), samples.begin() + static_cast<std::ptrdiff_t>(best + width));
  double sum = 0.0;
  for (double v : w.kept) sum += v;
  w.mean = sum / static_cast<double>(w.kept.size());
  w.min = w.kept.front(), w.max = w.kept.back();
  w.spread = bestSpread;
  w.valid = true;
  return w;
}

/// Linear-interpolated quantile of an ascending vector; 0 for none.
inline double quantileSorted(const std::vector<double>& sorted, double q) {
  if (sorted.empty()) return 0.0;
  if (sorted.size() == 1) return sorted[0];
  const double pos = q * static_cast<double>(sorted.size() - 1);
  const std::size_t lo = static_cast<std::size_t>(pos);
  const std::size_t hi = std::min(lo + 1, sorted.size() - 1);
  return sorted[lo] + (sorted[hi] - sorted[lo]) * (pos - static_cast<double>(lo));
}

/// Share of one core, in percent, for a pass of `passSeconds` that covers `audioSeconds` of audio.
inline double corePercent(double passSeconds, double audioSeconds) {
  return audioSeconds > 0.0 ? passSeconds / audioSeconds * 100.0 : 0.0;
}

struct Config {
  double warmupSeconds = 0.3;
  double windowSeconds = 1.0;
  double acceptFraction = 0.70;
  double acceptTolerance = 0.03;
  int minSamples = 15;
  int maxAttempts = 5;
};

constexpr int kMinSamplesForWindow = 3;  // below this a spread is not measuring dispersion

struct Result {
  bool ok = false;
  int attempts = 0;       // attempts run, the accepted one included
  int samples = 0;        // timed passes in the accepted attempt
  bool hitDeadline = false;
  double meanSeconds = 0.0;
  double spread = 0.0;
  double corePercent = 0.0;
};

inline double nowSeconds() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

/// One attempt's timed passes into `times`: warm up, then time until the window is done and
/// minSamples are in, or a hard deadline sized from the work (so a slow machine still reaches
/// minSamples). True when the deadline, not a full window, ended it.
template <class Run, class Reset>
bool timePasses(const Config& c, Run& run, Reset& reset, std::vector<double>& times) {
  for (double t0 = nowSeconds(); nowSeconds() - t0 < c.warmupSeconds;) reset(), run();
  reset(), run();  // at least one warm-up pass
  const double start = nowSeconds();
  double deadline = start + c.windowSeconds * 3.0;
  bool sized = false, done = false;
  while (!done) {
    reset();
    times.push_back(run());
    const double now = nowSeconds();
    if (!sized) deadline = std::max(deadline, start + (now - start) * c.minSamples * 1.5), sized = true;
    const bool full = now - start >= c.windowSeconds && static_cast<int>(times.size()) >= c.minSamples;
    done = full || now >= deadline;
    if (done) return !full;
  }
  return false;
}

/// Times passes of `run()` (which returns the seconds one pass took, measured by the caller
/// around its own loop, after `reset()`), until an attempt's tightest 70% agree within 3%.
template <class Run, class Reset>
Result measure(const Config& c, double audioSeconds, Run run, Reset reset) {
  Result r;
  for (int attempt = 1; attempt <= c.maxAttempts; ++attempt) {
    r.attempts = attempt;
    std::vector<double> times;
    r.hitDeadline = timePasses(c, run, reset, times);
    if (static_cast<int>(times.size()) < kMinSamplesForWindow) continue;
    const Window w = tightestWindow(times, c.acceptFraction);
    if (!w.valid || w.spread > c.acceptTolerance) continue;  // voided: run again
    r.ok = true;
    r.samples = static_cast<int>(times.size());
    r.meanSeconds = w.mean;
    r.spread = w.spread;
    r.corePercent = corePercent(w.mean, audioSeconds);
    return r;
  }
  return r;
}

}  // namespace bench
