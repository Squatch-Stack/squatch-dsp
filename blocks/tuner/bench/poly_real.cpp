// SPDX-License-Identifier: MIT
// Real strums for the polyphonic check: open-string plucks from a single-notes DI recording are
// mixed with a random strum spread and level, and each string's poly reading is compared with
// what the mono tuner reads for the same pluck on its own (same instant after its onset).
#include <cstdio>
#include <random>

#include <squatch/tuner/poly.hpp>

#include "measure.hpp"
#include "onsets.hpp"
#include "wav.hpp"

namespace bench {
namespace {

const double kTimes[] = {0.25, 0.5, 1.0};
const char* kNames[] = {"E2", "A2", "D3", "G3", "B3", "E4"};

struct OpenString {
  double onset = -1.0;
  Trace mono;  // the mono tuner's steady reading, re-timed to this onset
};
using OpenStrings = std::array<OpenString, 6>;

struct Tally {
  std::vector<std::vector<double>> diff = std::vector<std::vector<double>>(18);
  std::vector<int> valid = std::vector<int>(18, 0);
};

double monoAt(const Trace& tr, double t, double refHz) {
  std::vector<double> v;
  for (size_t i = 0; i < tr.t.size(); ++i)
    if (std::fabs(tr.t[i] - t) <= 0.02 && tr.e[i].valid) v.push_back(1200.0 * std::log2(tr.e[i].hz / refHz));
  return quantile(v, 0.5);
}

/// Which open string (if any) this note is: within 60 c of it, and the first note of a run (the
/// note before it was at least 250 c higher).
int openStringOf(double cents, double prevCents, const PolyConfig& pc) {
  for (int s = 0; s < 6 && prevCents - cents > 250.0; ++s) {
    if (std::fabs(cents - 1200.0 * std::log2(pc.tuning[s] / 440.0)) < 60.0) return s;
  }
  return -1;
}

OpenStrings findOpenStrings(const Audio& a, const PolyConfig& pc) {
  SquatchAlgo mono("mono", TunerConfig{}, true);
  mono.configure(a.rate, 38.0f);
  const Trace all = runTrace(mono, a.x, a.rate, 0.0);
  OpenStrings out{};
  double prev = 1e9;
  for (double t : findOnsets(a.x, a.rate)) {
    const Trace tr = slice(all, t, 1.6);
    const double c = monoAt(tr, 1.2, 440.0);
    if (std::isnan(c)) continue;
    const int s = openStringOf(c, prev, pc);
    if (s >= 0 && out[s].onset < 0) out[s] = {t, tr};
    prev = c;
  }
  return out;
}

/// Mix the six open strings into one strum; returns each string's delay.
std::array<double, 6> mixStrum(const Audio& a, const OpenStrings& open, std::mt19937& rng, std::vector<float>& x) {
  std::uniform_real_distribution<double> u(0.0, 1.0);
  std::array<double, 6> delay{};
  x.assign(static_cast<size_t>(1.8 * a.rate), 0.0f);
  for (int s = 0; s < 6; ++s) {
    if (open[s].onset < 0) continue;
    delay[s] = 0.008 * s * u(rng);
    const float g = static_cast<float>(std::pow(10.0, -6.0 * u(rng) / 20.0));
    const size_t src = static_cast<size_t>((open[s].onset - 0.1) * a.rate), dst = static_cast<size_t>(delay[s] * a.rate);
    for (size_t i = 0; dst + i < x.size() && src + i < a.x.size(); ++i) x[dst + i] += g * a.x[src + i];
  }
  return delay;
}

void scoreStrum(const std::vector<float>& x, float rate, const OpenStrings& open, const std::array<double, 6>& delay,
                Tally& tally) {
  PolyConfig pc;
  static PolyTuner p;
  p.configure(pc);
  const size_t strum = static_cast<size_t>(0.1 * rate);
  p.process(x.data(), static_cast<int>(strum));
  p.restart();
  size_t pos = strum;
  for (size_t ti = 0; ti < 3; ++ti) {
    const size_t until = strum + static_cast<size_t>(kTimes[ti] * rate);
    p.process(x.data() + pos, static_cast<int>(until - pos));
    pos = until;
    for (int s = 0; s < 6; ++s) {
      const double m = open[s].onset < 0 ? NAN : monoAt(open[s].mono, kTimes[ti] - delay[s], pc.tuning[s]);
      if (std::isnan(m) || !p.reading(s).valid) continue;
      tally.diff[ti * 6 + s].push_back(p.reading(s).cents - m);
      ++tally.valid[ti * 6 + s];
    }
  }
}

void printTable(const OpenStrings& open, const Tally& tally, int trials) {
  PolyConfig pc;
  for (int s = 0; s < 6; ++s) {
    if (open[s].onset < 0) std::printf("| %s | not found |", kNames[s]);
    else std::printf("| %s | %+.1f c |", kNames[s], monoAt(open[s].mono, 1.2, pc.tuning[s]));
    for (size_t ti = 0; ti < 3; ++ti) {
      std::vector<double> ab;
      double ss = 0;
      for (double d : tally.diff[ti * 6 + s]) {
        ab.push_back(std::fabs(d));
        ss += d * d;
      }
      std::printf(" %.2f (%.2f) [%.0f%%] |", ab.empty() ? NAN : std::sqrt(ss / ab.size()), quantile(ab, 0.95),
                  100.0 * tally.valid[ti * 6 + s] / trials);
    }
    std::printf("\n");
  }
}

}  // namespace

void realStrums(const std::string& path, int trials) {
  Audio a;
  if (!readWav(path, a)) return;
  const OpenStrings open = findOpenStrings(a, PolyConfig{});
  std::printf("\n## Real strums (%d)\n\nOpen strings from %s mixed with 0-40 ms spread and 0 to -6 dB levels;\n"
              "poly reading minus the mono reading of the same pluck alone, RMS c (p95 |diff|) [valid %%]\n\n"
              "| string | open-string pitch (mono, 1.2 s) | 250 ms | 500 ms | 1000 ms |\n|---|---|---|---|---|\n",
              trials, path.substr(path.find_last_of('/') + 1).c_str());
  Tally tally;
  std::vector<float> x;
  for (int t = 0; t < trials; ++t) {
    std::mt19937 rng(static_cast<unsigned>(99 + t));
    const auto delay = mixStrum(a, open, rng, x);
    scoreStrum(x, a.rate, open, delay, tally);
  }
  printTable(open, tally, trials);
}

}  // namespace bench
