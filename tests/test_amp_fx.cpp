// SPDX-License-Identifier: MIT
// The amp end of the rig: Comp's static curve in steady state, Crunch's silence, clipping and
// aliasing, and Hall's decay time against its Decay setting.
#include <squatch/dsp/blocks/comp.hpp>
#include <squatch/dsp/blocks/crunch.hpp>
#include <squatch/dsp/blocks/hall.hpp>

#include <cmath>
#include <cstdio>
#include <vector>

#include "check.hpp"
#include "signals.hpp"

using namespace squatch::dsp;

namespace {

constexpr double kPi = 3.14159265358979323846;
alignas(64) unsigned char gHallMem[1 << 20];

std::vector<float> sine(int n, double f, double rate, float amp) {
  std::vector<float> x(n);
  for (int i = 0; i < n; ++i) x[i] = amp * static_cast<float>(std::sin(2.0 * kPi * f * i / rate));
  return x;
}

template <class B>
std::vector<float> run(B& b, const std::vector<float>& in) {
  std::vector<float> out(in.size());
  for (size_t i = 0; i < in.size(); i += 64) {
    const int len = static_cast<int>(std::min<size_t>(64, in.size() - i));
    b.process(&in[i], &out[i], len);
  }
  return out;
}

float peakDb(const std::vector<float>& x, size_t from) {
  float p = 0.0f;
  for (size_t i = from; i < x.size(); ++i) p = std::fmax(p, std::fabs(x[i]));
  return 20.0f * std::log10(p + 1e-30f);
}

// Goertzel magnitude (dB, relative to a full-scale sine) at f over [from, end).
double toneDb(const std::vector<float>& x, size_t from, double f, double rate) {
  const double w = 2.0 * kPi * f / rate, c = 2.0 * std::cos(w);
  double s1 = 0.0, s2 = 0.0;
  const size_t n = x.size() - from;
  for (size_t i = from; i < x.size(); ++i) {
    const double s0 = x[i] * (0.5 - 0.5 * std::cos(2.0 * kPi * (i - from) / n)) + c * s1 - s2;
    s2 = s1, s1 = s0;
  }
  const double mag = std::sqrt(s1 * s1 + s2 * s2 - c * s1 * s2) / (n * 0.25);
  return 20.0 * std::log10(mag + 1e-30);
}

}  // namespace

TEST(compHoldsTheStaticCurveInSteadyState) {
  static Compressor c;
  Arena none;
  c.prepare(Setup{48000.0, 64}, none);
  c.set(Compressor::Threshold, -24.0f);
  c.set(Compressor::Ratio, 4.0f);
  c.set(Compressor::Knee, 0.0f);
  c.set(Compressor::Makeup, 6.0f);
  c.set(Compressor::Attack, 1.0f);
  c.set(Compressor::Release, 500.0f);
  const std::vector<float> in = sine(48000, 1000.0, 48000.0, 0.5f);  // -6 dBFS peak
  const float got = peakDb(run(c, in), 24000);
  const float want = -6.0f - (-6.0f - Compressor::curve(-6.0f, -24.0f, 4.0f, 0.0f)) + 6.0f;  // -13.5 dB
  std::printf("  comp: -6 dBFS in, %.2f dBFS out (curve says %.2f), GR meter %.2f dB\n", got, want,
              c.get(Compressor::Reduction));
  CHECK(std::fabs(got - want) < 1.0f, "steady-state output %.2f dB, curve %.2f dB", got, want);
}

TEST(crunchIsSilentOnSilenceAndClipsWhenDriven) {
  static Crunch a;
  Arena none;
  a.prepare(Setup{48000.0, 64}, none);
  a.set(Crunch::Gain, 1.0f);
  const std::vector<float> zero(9600, 0.0f);
  CHECK(peakDb(run(a, zero), 0) < -140.0f, "silence in, %.1f dBFS out", peakDb(run(a, zero), 0));
  const std::vector<float> in = sine(48000, 110.0, 48000.0, 0.1f);  // a low A at -20 dBFS
  const std::vector<float> out = run(a, in);
  std::printf("  crunch: -20 dBFS A2 in, %.1f dBFS out, clip %g\n", peakDb(out, 24000), a.get(Crunch::Clip));
  CHECK(a.get(Crunch::Clip) == 1.0f, "full gain on -20 dBFS clips");
  CHECK(peakDb(out, 24000) < 0.0f, "output stays below full scale at the default level");
}

TEST(crunchKeepsAliasingOutOfTheBand) {
  static Crunch a;
  Arena none;
  a.prepare(Setup{48000.0, 64}, none);
  a.set(Crunch::Gain, 1.0f);
  const double rate = 48000.0, f0 = 2750.0;  // harmonics from the 9th fold back below Nyquist
  const std::vector<float> out = run(a, sine(96000, f0, rate, 0.3f));
  const double h3 = toneDb(out, 48000, 3.0 * f0, rate);
  double worst = -300.0, at = 0.0;
  double top = -300.0, topAt = 0.0;  // above the audible band: reported, not judged
  for (int k = 9; k <= 30; ++k) {
    double f = std::fmod(k * f0, rate);
    if (f > rate / 2) f = rate - f;
    if (std::fabs(std::fmod(f, f0)) < 1.0 || std::fabs(std::fmod(f, f0) - f0) < 1.0) continue;  // lands on a harmonic
    const double d = toneDb(out, 48000, f, rate);
    if (f > 16000.0) {
      if (d > top) top = d, topAt = f;
      continue;
    }
    if (d > worst) worst = d, at = f;
  }
  std::printf("  crunch aliasing: 3rd harmonic %.1f dB; worst alias below 16 kHz %.1f dB at %.0f Hz (%.1f dB below);"
              " above 16 kHz %.1f dB at %.0f Hz\n", h3, worst, at, h3 - worst, top, topAt);
  CHECK(h3 - worst > 40.0, "worst alias only %.1f dB below the 3rd harmonic", h3 - worst);
}

TEST(hallDecaysAtItsDecayTime) {
  static Hall h;
  Arena mem(gHallMem, sizeof gHallMem);
  h.prepare(Setup{48000.0, 64}, mem);
  CHECK(h.ready(), "delay memory fits");
  h.set(Hall::Decay, 1.0f);
  h.set(Hall::Mix, 1.0f);
  h.set(Hall::Damp, 20000.0f);
  h.set(Hall::Predelay, 0.0f);
  std::vector<float> in(48000 * 2, 0.0f);
  sig::Rng rng{5};
  for (int i = 0; i < 480; ++i) in[i] = rng.uniform();  // a 10 ms noise burst
  const std::vector<float> out = run(h, in);
  const auto rms = [&](double a, double b) {
    double e = 0.0;
    for (int i = static_cast<int>(a * 48000); i < static_cast<int>(b * 48000); ++i) e += out[i] * out[i];
    return 10.0 * std::log10(e / ((b - a) * 48000) + 1e-30);
  };
  const double drop = rms(0.15, 0.25) - rms(0.65, 0.75);  // over 0.5 s: 30 dB for a 1 s RT60
  std::printf("  hall: RT60 1 s, level falls %.1f dB over 0.5 s (30 expected)\n", drop);
  CHECK(std::fabs(drop - 30.0) < 5.0, "decay over 0.5 s %.1f dB", drop);
  const std::vector<float> zero(9600, 0.0f);
  h.reset();
  CHECK(peakDb(run(h, zero), 0) < -140.0f, "silence stays silent");
}
