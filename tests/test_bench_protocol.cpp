// SPDX-License-Identifier: MIT
// The benchmark method (bench/protocol.hpp, adapted from pedal-science/NAMBench by Rik Hemsley,
// MIT; see THIRD_PARTY_NOTICES.md): the one-sided tightest-70% window, quantiles, core percentage, and
// the void-and-rerun rule, with synthetic pass times so no machine noise is involved.
#include <vector>

#include "../bench/protocol.hpp"
#include "check.hpp"

TEST(benchWindowKeepsTheTightest70PercentAndDropsSlowOutliers) {
  // Seven agree within 6%; three passes were disturbed and are far slower.
  const std::vector<double> t = {10.4, 200.0, 10.0, 10.2, 90.0, 10.1, 10.6, 50.0, 10.3, 10.5};
  const bench::Window w = bench::tightestWindow(t, 0.70);
  CHECK(w.valid && w.kept.size() == 7, "kept %zu", w.kept.size());
  CHECK(w.min == 10.0 && w.max == 10.6, "window %g..%g", w.min, w.max);
  CHECK(std::fabs(w.spread - 0.06) < 1e-9, "spread %g", w.spread);
  CHECK(std::fabs(w.mean - 10.3) < 1e-9, "mean %g", w.mean);
  // One-sided: interference only slows a pass, so the slow tail goes and nothing fast is trimmed.
  for (double v : w.kept) CHECK(v < 11.0, "outlier %g kept", v);
}

TEST(benchWindowRefusesWhatItCannotMeasure) {
  CHECK(!bench::tightestWindow({}, 0.7).valid, "no samples");
  CHECK(!bench::tightestWindow({1.0, 2.0}, 0.0).valid, "fraction 0");
  CHECK(!bench::tightestWindow({1.0, 2.0}, 1.5).valid, "fraction above 1");
  CHECK(!bench::tightestWindow({1.0, 2.0}, NAN).valid, "NaN fraction");
  CHECK(!bench::tightestWindow({0.0, 0.0, 0.0}, 0.7).valid, "a spread is relative to a positive minimum");
  const bench::Window one = bench::tightestWindow({3.0}, 0.7);
  CHECK(one.valid && one.kept.size() == 1 && one.spread == 0.0, "a single sample is a window of one");
}

TEST(benchQuantileAndCorePercent) {
  std::vector<double> v;
  for (int i = 0; i <= 100; ++i) v.push_back(i);
  CHECK(bench::quantileSorted(v, 0.99) == 99.0 && bench::quantileSorted(v, 0.5) == 50.0, "q99 %g", bench::quantileSorted(v, 0.99));
  CHECK(bench::quantileSorted({}, 0.99) == 0.0 && bench::quantileSorted({7.0}, 0.99) == 7.0, "edges");
  CHECK(std::fabs(bench::quantileSorted({0.0, 10.0}, 0.25) - 2.5) < 1e-12, "interpolates");
  CHECK(std::fabs(bench::corePercent(0.012, 10.0) - 0.12) < 1e-12, "core %g", bench::corePercent(0.012, 10.0));
  CHECK(bench::corePercent(1.0, 0.0) == 0.0, "no audio, no percentage");
}

namespace {
bench::Config fast() {
  bench::Config c;
  c.warmupSeconds = 0.0, c.windowSeconds = 0.002, c.minSamples = 15, c.maxAttempts = 3;
  return c;
}
}  // namespace

TEST(benchMeasureAcceptsAgreementAndVoidsDisagreement) {
  int resets = 0, calls = 0;
  const bench::Result steady = bench::measure(fast(), 10.0, [&] { return 0.0100 + 0.0001 * (++calls % 3); }, [&] { ++resets; });
  CHECK(steady.ok && steady.attempts == 1, "ok %d attempts %d", steady.ok, steady.attempts);
  CHECK(resets >= steady.samples, "reset before every timed pass (%d resets, %d passes)", resets, steady.samples);
  CHECK(std::fabs(steady.corePercent - steady.meanSeconds / 10.0 * 100.0) < 1e-12, "core %g", steady.corePercent);
  CHECK(steady.corePercent > 0.099 && steady.corePercent < 0.103, "core %g", steady.corePercent);
  // Negative control: passes that never agree (1x and 2x) are voided every time, and no number
  // is reported at all.
  int k = 0;
  const bench::Result noisy = bench::measure(fast(), 10.0, [&] { return (++k % 2) ? 0.01 : 0.02; }, [] {});
  CHECK(!noisy.ok && noisy.attempts == 3 && noisy.corePercent == 0.0, "ok %d attempts %d core %g", noisy.ok, noisy.attempts, noisy.corePercent);
  // Slow outliers alone do not void: 25% of passes are disturbed, the tight 70% still agree.
  int m = 0;
  const bench::Result disturbed = bench::measure(fast(), 10.0, [&] { return (++m % 4 == 0) ? 0.05 : 0.01; }, [] {});
  CHECK(disturbed.ok && std::fabs(disturbed.meanSeconds - 0.01) < 1e-12, "ok %d mean %g", disturbed.ok, disturbed.meanSeconds);
}
