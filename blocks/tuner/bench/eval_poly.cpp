// SPDX-License-Identifier: MIT
// Polyphonic strum check.
//  1. Synthetic strums in standard tuning: every string detuned (uniform ±20 c), stiff strings,
//     3 c glide, strum spread 0-40 ms, random levels. Error per string at 0.25 / 0.5 / 1.0 s.
//  2. Real: open-string plucks cut from a single-notes DI recording, mixed into strums; each
//     string's poly reading is compared with the mono tuner's reading of the same pluck alone.
//
//   eval_poly [trials] [single-notes.wav]
#include <cstdio>
#include <cstdlib>

#include <squatch/tuner/poly.hpp>

#include "measure.hpp"
#include "synth.hpp"
#include "wav.hpp"

using namespace bench;

namespace {

constexpr float kFs = 48000.0f;
const double kTimes[] = {0.25, 0.5, 1.0};
const char* kNames[] = {"E2", "A2", "D3", "G3", "B3", "E4"};

struct Strum {
  std::vector<float> x;
  std::array<ToneSpec, 6> spec{};
  std::array<double, 6> delay{};
};

Strum makeStrum(std::mt19937& rng, const PolyConfig& pc) {
  std::uniform_real_distribution<double> u(0.0, 1.0);
  Strum st;
  const double pre = 0.1, len = 1.6;
  st.x.assign(static_cast<size_t>((pre + len) * kFs), 0.0f);
  for (int s = 0; s < 6; ++s) {
    ToneSpec& t = st.spec[static_cast<size_t>(s)];
    t.f0 = pc.tuning[static_cast<size_t>(s)] * std::exp2((40.0 * u(rng) - 20.0) / 1200.0);
    t.inharmonicity = s < 3 ? 8e-5 + 1.2e-4 * u(rng) : 1.5e-5 + 6e-5 * u(rng);
    t.pluckPos = 0.12 + 0.18 * u(rng);
    t.pickupPos = 0.08 + 0.17 * u(rng);
    t.t60 = 3.0 + 5.0 * u(rng);
    t.glideCents = 3.0;
    t.snrDb = 200.0;
    t.pickNoiseDb = 0.0;
    t.peak = 0.3 * std::pow(10.0, -6.0 * u(rng) / 20.0);
    st.delay[static_cast<size_t>(s)] = 0.008 * s * u(rng);
    t.preroll = pre + st.delay[static_cast<size_t>(s)];
    t.seconds = len - st.delay[static_cast<size_t>(s)];
    const std::vector<float> y = synthesize(t, kFs, rng);
    for (size_t i = 0; i < y.size() && i < st.x.size(); ++i) st.x[i] += y[i];
  }
  std::normal_distribution<double> g(0.0, 1e-4);  // about -80 dBFS noise floor
  for (float& v : st.x) v += static_cast<float>(g(rng));
  return st;
}

/// Poly readings of every string at the given times (seconds after the strum), NAN if invalid.
std::vector<std::array<double, 6>> readPoly(const std::vector<float>& x, double strumAt) {
  PolyTuner p;
  PolyConfig pc;
  p.configure(pc);
  std::vector<std::array<double, 6>> out;
  const size_t start = static_cast<size_t>(strumAt * kFs);
  p.process(x.data(), static_cast<int>(start));
  p.restart();
  size_t pos = start;
  for (double t : kTimes) {
    const size_t until = std::min(x.size(), start + static_cast<size_t>(t * kFs));
    p.process(x.data() + pos, static_cast<int>(until - pos));
    pos = until;
    std::array<double, 6> r{};
    for (int s = 0; s < 6; ++s) r[s] = p.reading(s).valid ? p.reading(s).cents : NAN;
    out.push_back(r);
  }
  return out;
}

void syntheticStrums(int trials) {
  PolyConfig pc;
  std::vector<std::vector<double>> err(6 * 3);
  std::vector<int> valid(6 * 3, 0);
  for (int t = 0; t < trials; ++t) {
    std::mt19937 rng(static_cast<unsigned>(4242 + t));
    const Strum st = makeStrum(rng, pc);
    const auto r = readPoly(st.x, 0.1);
    for (size_t ti = 0; ti < 3; ++ti) {
      for (int s = 0; s < 6; ++s) {
        if (std::isnan(r[ti][s])) continue;
        const double truth = 1200.0 * std::log2(truthAt(st.spec[s], kTimes[ti] - st.delay[s]) / pc.tuning[s]);
        err[ti * 6 + s].push_back(r[ti][s] - truth);
        ++valid[ti * 6 + s];
      }
    }
  }
  std::printf("# Squatch Tuner: polyphonic accuracy\n\n"
              "## Synthetic strums (%d), error against the true pitch at that moment: RMS c (p95 |err|) [valid %%]\n\n"
              "| string | 250 ms | 500 ms | 1000 ms |\n|---|---|---|---|\n", trials);
  for (int s = 0; s < 6; ++s) {
    std::printf("| %s |", kNames[s]);
    for (size_t ti = 0; ti < 3; ++ti) {
      std::vector<double> a;
      double ss = 0;
      for (double e : err[ti * 6 + s]) { a.push_back(std::fabs(e)); ss += e * e; }
      std::printf(" %.2f (%.2f) [%.0f%%] |", a.empty() ? NAN : std::sqrt(ss / a.size()), quantile(a, 0.95),
                  100.0 * valid[ti * 6 + s] / trials);
    }
    std::printf("\n");
  }
}

}  // namespace

namespace bench {
void realStrums(const std::string& path, int trials);
}

int main(int argc, char** argv) {
  const int trials = argc > 1 ? std::atoi(argv[1]) : 200;
  syntheticStrums(trials);
  if (argc > 2) realStrums(argv[2], trials);
  return 0;
}
