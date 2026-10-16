// SPDX-License-Identifier: MIT
// The biquad EQ against the RBJ cookbook in double precision: magnitude to 0.01 dB at many
// frequencies, both from the coefficients the block runs and from sines through the block.
#include <squatch/dsp/blocks/eq.hpp>

#include <complex>

#include "check.hpp"
#include "golden.hpp"
#include "reference.hpp"
#include "signals.hpp"

using namespace squatch::dsp;

#if __has_include("golden/eq.inc")
static const double kGolden[golden::kSegments] =
#include "golden/eq.inc"
    ;
static const double* const kGoldenRender = kGolden;
#else
static const double* const kGoldenRender = nullptr;
#endif

namespace {

struct Setting {
  float lowF, lowG, midF, midG, midQ, highF, highG;
};

const Setting kSettings[] = {
    {100, 6, 1000, -4, 0.7f, 5000, 3},   {30, -12, 250, 12, 4.0f, 12000, -9}, {400, 18, 3000, -18, 0.3f, 2000, 18},
    {20, 3, 100, 9, 10.0f, 20000, -18},  {1000, -18, 8000, 6, 1.4f, 1000, 12}, {60, 0, 1000, 0, 0.7f, 5000, 0},
};

void apply(Eq& eq, const Setting& s) {
  const float v[] = {s.lowF, s.lowG, s.midF, s.midG, s.midQ, s.highF, s.highG};
  for (ParamId id = 0; id < Eq::kParamCount; ++id) eq.set(id, v[id]);
}

// The block, prepared and moved past its smoothing.
void settle(Eq& eq, const Setting& s, double rate) {
  Arena none;
  eq.prepare(Setup{rate, 256}, none);
  apply(eq, s);
  std::vector<float> scratch(256 * 40, 0.0f);  // 100 ms or more at every rate tested
  for (size_t i = 0; i < scratch.size(); i += 256) eq.process(&scratch[i], &scratch[i], 256);
  eq.reset();
}

double refDb(const Setting& s, double hz, double rate) {
  return ref::magnitudeDb(ref::rbj(0, rate, std::min<double>(s.lowF, 0.49 * rate), s.lowG, Eq::kShelfQ), hz, rate) +
         ref::magnitudeDb(ref::rbj(1, rate, s.midF, s.midG, s.midQ), hz, rate) +
         ref::magnitudeDb(ref::rbj(2, rate, std::min<double>(s.highF, 0.49 * rate), s.highG, Eq::kShelfQ), hz, rate);
}

// The magnitude of what the block actually runs: its float coefficients, evaluated in double.
double blockDb(const Eq& eq, double hz, double rate) {
  double total = 0.0;
  for (int b = 0; b < 3; ++b) {
    const BiquadCoeffs& c = eq.band(b).coeffs();
    const std::complex<double> s(0.0, std::tan(sig::kPi * hz / rate) / c.g), k = c.k, m0 = c.m0;
    const std::complex<double> h = (m0 * s * s + (m0 * k + double(c.m1)) * s + (m0 + double(c.m2))) / (s * s + k * s + 1.0);
    total += sig::db(std::abs(h));
  }
  return total;
}

}  // namespace

TEST(eqCoefficientsMatchCookbook) {
  static Eq eq;
  for (double rate : {44100.0, 48000.0, 96000.0})
    for (const Setting& s : kSettings) {
      settle(eq, s, rate);
      double worst = 0.0, at = 0.0;
      for (int k = 0; k <= 300; ++k) {
        const double hz = 10.0 * std::pow(0.45 * rate / 10.0, k / 300.0);
        const double err = std::fabs(blockDb(eq, hz, rate) - refDb(s, hz, rate));
        if (err > worst) worst = err, at = hz;
      }
      CHECK(worst <= 0.01, "%.0f Hz rate, low %g Hz: off by %.4f dB at %.1f Hz", rate, s.lowF, worst, at);
    }
}

TEST(eqProcessedSinesMatchCookbook) {
  static Eq eq;
  const double rate = 48000.0;
  for (const Setting& s : kSettings) {
    settle(eq, s, rate);
    double worst = 0.0, at = 0.0;
    for (int k = 0; k <= 40; ++k) {
      const double hz = 20.0 * std::pow(1000.0, k / 40.0);  // 20 Hz to 20 kHz
      std::vector<float> x = sig::sine(hz, 48000, rate, 0.25);
      eq.reset();
      eq.process(x.data(), x.data(), 48000);  // longer than maxBlock is fine for the block itself
      const double gotDb = sig::db(sig::amplitudeAt(x, hz, rate, 24000, 48000) / 0.25);
      const double err = std::fabs(gotDb - refDb(s, hz, rate));
      if (err > worst) worst = err, at = hz;
    }
    CHECK(worst <= 0.01, "low %g Hz setting: off by %.4f dB at %.1f Hz", s.lowF, worst, at);
  }
}

TEST(eqFlatIsTransparent) {
  static Eq eq;
  settle(eq, kSettings[5], 48000.0);
  const std::vector<float> x = sig::noise(4800, 0.9f, 3);
  std::vector<float> y(x.size());
  eq.process(x.data(), y.data(), 4800);
  bool same = true;
  for (size_t i = 0; i < x.size(); ++i) same = same && y[i] == x[i];
  CHECK(same, "0 dB everywhere: the output is the input, bit for bit");
}

TEST(eqSmoothsParameterMoves) {
  static Eq eq;
  settle(eq, kSettings[5], 48000.0);
  eq.set(Eq::MidGain, 12.0f);
  std::vector<float> x(480, 0.0f);
  eq.process(x.data(), x.data(), 480);  // 10 ms: halfway through 20 ms of smoothing
  const BiquadCoeffs half = eq.band(1).coeffs(), want = designBiquad(Shape::Peak, 48000.0, 1000.0, 12.0, 0.7);
  CHECK(half.m1 > 0.0f && half.m1 < want.m1, "halfway: m1 %f between 0 and %f", half.m1, want.m1);
  eq.process(x.data(), x.data(), 480);
  CHECK(eq.band(1).coeffs().m1 == want.m1, "arrived after the smoothing time");
}

TEST(eqDecaysToExactZero) {
  static Eq eq;
  settle(eq, kSettings[1], 48000.0);
  std::vector<float> x(48000 * 5, 0.0f);
  x[0] = 1.0f;
  for (size_t i = 0; i < x.size(); i += 128) eq.process(&x[i], &x[i], 128);
  CHECK(x.back() == 0.0f, "silence after an impulse ends in exact zeros, not denormals: %g", x.back());
}

TEST(eqGolden) {
  const double rate = 48000.0;
  const Setting s = kSettings[1];
  const std::vector<float> x = sig::noise(65536, 0.5f, 41);
  static Eq eq;
  settle(eq, s, rate);
  std::vector<float> y(x.size());
  for (size_t i = 0; i < x.size(); i += 128) eq.process(&x[i], &y[i], 128);
  std::vector<double> r(x.begin(), x.end());
  r = ref::filter(ref::rbj(0, rate, s.lowF, s.lowG, Eq::kShelfQ), r);
  r = ref::filter(ref::rbj(1, rate, s.midF, s.midG, s.midQ), r);
  r = ref::filter(ref::rbj(2, rate, s.highF, s.highG, Eq::kShelfQ), r);
  const double e = sig::esr(y, r);
  CHECK(e < 1e-9, "ESR against the double-precision cookbook filter %.3g", e);
  std::printf("     eq ESR vs reference: %.3g\n", e);
  golden::check("eq", y, kGoldenRender, 1e-5);
}
