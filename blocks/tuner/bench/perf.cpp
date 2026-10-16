// SPDX-License-Identifier: MIT
// CPU cost per input sample on this machine, for each algorithm, over 30 s of plucked notes at
// 48 kHz (one pluck every 1.5 s across the guitar's range) and over 30 s of near-silence.
// Also prints the object sizes, which is all the memory these use (nothing is allocated).
#include <chrono>
#include <cstdio>

#include <squatch/tuner/poly.hpp>

#include "measure.hpp"
#include "synth.hpp"

using namespace bench;

namespace {

constexpr float kFs = 48000.0f;

std::vector<float> plucks(double seconds) {
  const double notes[] = {82.41, 110.0, 146.83, 196.0, 246.94, 329.63, 41.2, 659.26};
  std::vector<float> x;
  std::mt19937 rng(7);
  for (int i = 0; x.size() < seconds * kFs; ++i) {
    ToneSpec s;
    s.f0 = notes[i % 8];
    s.inharmonicity = 1e-4;
    s.glideCents = 3.0;
    s.preroll = 0.0;
    s.seconds = 1.5;
    s.pickNoiseDb = 0.0;
    const auto y = synthesize(s, kFs, rng);
    x.insert(x.end(), y.begin(), y.end());
  }
  return x;
}

template <typename F>
double nsPerSample(const std::vector<float>& x, F&& process) {
  const int block = 64;
  const auto t0 = std::chrono::steady_clock::now();
  for (size_t i = 0; i + block <= x.size(); i += block) process(&x[i], block);
  const auto t1 = std::chrono::steady_clock::now();
  return std::chrono::duration<double, std::nano>(t1 - t0).count() / static_cast<double>(x.size());
}

void row(const char* name, double nsPlay, double nsQuiet) {
  // At 48 kHz one sample lasts 20833 ns; the share of one core is ns / 20833.
  std::printf("| %s | %.1f | %.2f%% | %.1f | %.2f%% |\n", name, nsPlay, 100.0 * nsPlay / 20833.3, nsQuiet,
              100.0 * nsQuiet / 20833.3);
}

}  // namespace

int main() {
  const std::vector<float> play = plucks(30.0);
  std::vector<float> quiet(play.size());
  std::mt19937 rng(3);
  std::normal_distribution<float> g(0.0f, 1e-5f);
  for (float& v : quiet) v = g(rng);
  std::printf("# Squatch Tuner: CPU cost\n\n"
              "| algorithm | ns/sample playing | of one core @48k | ns/sample quiet | of one core @48k |\n"
              "|---|---|---|---|---|\n");
  auto runAlgo = [&](const char* name, auto make) {
    auto a = make(), b = make();
    a->configure(kFs, 38.0f);
    b->configure(kFs, 38.0f);
    row(name, nsPerSample(play, [&](const float* p, int n) { a->process(p, n); }),
        nsPerSample(quiet, [&](const float* p, int n) { b->process(p, n); }));
  };
  runAlgo("Squatch Tuner (6 partials)", [] { return std::make_unique<SquatchAlgo>("", TunerConfig{}); });
  runAlgo("Squatch Tuner (3 partials)", [] { TunerConfig c; c.partials = 3; return std::make_unique<SquatchAlgo>("", c); });
  runAlgo("MPM alone (12k)", [] { return std::make_unique<CoarseAlgo<Mpm, Limits::kMaxWindow>>("", 12000.0f, true, 0.8f); });
  runAlgo("YIN alone (12k)", [] { return std::make_unique<CoarseAlgo<Yin, Limits::kMaxWindow>>("", 12000.0f, true, 0.85f); });
  runAlgo("MPM + phase vocoder", [] { return makePhaseVocoder("", 6, true); });
  PolyTuner p, q;
  p.configure(PolyConfig{});
  q.configure(PolyConfig{});
  row("Poly strum check (6 strings)", nsPerSample(play, [&](const float* s, int n) { p.process(s, n); }),
      nsPerSample(quiet, [&](const float* s, int n) { q.process(s, n); }));
  std::printf("\nObject sizes (all of the memory, no heap): Tuner %zu bytes, PolyTuner %zu bytes,\n"
              "StrobeBank %zu bytes, "
              "PartialChannel %zu bytes.\n", sizeof(Tuner), sizeof(PolyTuner), sizeof(StrobeBank), sizeof(PartialChannel));
  return 0;
}
