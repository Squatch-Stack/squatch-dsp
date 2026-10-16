// SPDX-License-Identifier: MIT
// Trail OD against the browser rig's Overdrive (web/worklets/dsp/overdrive.js: a 4x oversampled soft clipper,
// 129-tap Kaiser FIR pair, 32 samples of delay): stored renders (tests/golden-parity/od-*.f32, see gen.mjs) to
// ESR < 1e-6, the aliasing of a clipped sine within 1 dB of the browser's, the browser's controls and 32-sample
// latency, and negative controls (a wrong Tone, a level error, and a hard-clipped sine that must read as aliased).
#include <squatch/dsp/blocks/trail_od.hpp>

#include <cmath>

#include "check.hpp"
#include "parity_golden.hpp"
#include "signals.hpp"

using namespace squatch::dsp;

namespace {

constexpr std::size_t kN = 24064;

struct OdCase {
  const char* file;
  float drive, tone, level;
};
// The browser Overdrive's parameters, as gen.mjs sets them (default: the rig's defaults).
const OdCase kCases[] = {
    {"od-default.f32", 0.5f, 0.5f, -6.0f}, {"od-A.f32", 0.2f, 0.3f, -6.0f}, {"od-B.f32", 0.9f, 0.7f, -10.0f},
    {"od-C.f32", 1.0f, 1.0f, 12.0f},       {"od-D.f32", 0.0f, 0.0f, -30.0f},
    {"od-noise.f32", 0.7f, 1.0f, 0.0f},
};

struct Golden {
  std::vector<float> in, out;
};
Golden golden(const char* file) {
  const std::vector<float> all = parity::load(file);
  if (all.size() != 2 * kN) return {};
  return {parity::part(all, 0, kN), parity::part(all, 1, kN)};
}

std::vector<float> render(const std::vector<float>& in, float drive, float tone, float level) {
  static TrailOD od;
  Arena none;
  od.set(TrailOD::Drive, drive), od.set(TrailOD::Tone, tone), od.set(TrailOD::Level, level);
  od.prepare(Setup{48000.0, parity::kQuantum}, none);
  return parity::run(od, in);
}

// Level (dB, relative to the fundamental) of everything in the last 8192 samples that is not a harmonic below
// Nyquist of a sine on bin `bin` of 8192: the aliased harmonics, and noise. The window holds whole cycles.
double aliasingDb(const std::vector<float>& y, int bin) {
  const int n = 8192;
  const float* x = &y[y.size() - n];
  std::vector<double> power(n / 2, 0.0);
  for (int b = 1; b < n / 2; ++b) {
    double re = 0.0, im = 0.0;
    for (int i = 0; i < n; ++i) {
      const double a = 2.0 * sig::kPi * ((static_cast<long long>(b) * i) % n) / n;
      re += x[i] * std::cos(a), im -= x[i] * std::sin(a);
    }
    power[b] = re * re + im * im;
  }
  double rest = 0.0;
  for (int b = 1; b < n / 2; ++b)
    if (b % bin != 0) rest += power[b];
  return 10.0 * std::log10(rest / power[bin]);
}

}  // namespace

TEST(trailOdParityWithTheBrowserOverdrive) {
  for (const OdCase& c : kCases) {
    const Golden g = golden(c.file);
    CHECK(!g.in.empty(), "%s: stored render missing", c.file);
    if (g.in.empty()) continue;
    const double e = parity::esr(render(g.in, c.drive, c.tone, c.level), g.out);
    std::printf("     trail od %s: ESR %.3g\n", c.file, e);
    CHECK(e < parity::kBar, "%s: ESR %.3g (bar %.0e)", c.file, e, parity::kBar);
  }
}

// A sine at -12 dBFS pushed hard through the clipper aliases only as much as the browser's 4x filters let it.
TEST(trailOdAliasingIsTheBrowsers) {
  const Golden g = golden("od-sine.f32");
  CHECK(!g.in.empty(), "od-sine.f32: stored render missing");
  if (g.in.empty()) return;
  const std::vector<float> y = render(g.in, 0.8f, 1.0f, 0.0f);
  const double cpp = aliasingDb(y, 550), js = aliasingDb(g.out, 550);
  std::printf("     aliasing of the 3.2 kHz sine at Drive 0.8: this block %.1f dB, browser %.1f dB\n", cpp, js);
  CHECK(std::fabs(cpp - js) <= 1.0, "aliasing %.1f dB against the browser's %.1f dB", cpp, js);
  CHECK(js < -40.0, "the browser's own aliasing is low (%.1f dB), so the comparison means something", js);
}

TEST(trailOdLatencyIsTheFilterPairsDelay) {
  CHECK(TrailOD().latency() == 32, "round trip of the 129-tap pair at 4x is 32 samples");
  // An impulse through the clean path (Drive 0, Tone and Level open): the filter pair puts its peak 32 samples late.
  // (A sine would not do: the boosted branch's 720 Hz high-pass leads the unity path in phase at any frequency.)
  std::vector<float> x(512, 0.0f);
  x[100] = 0.5f;
  const std::vector<float> y = render(x, 0.0f, 1.0f, 12.0f);
  int at = 0;
  for (int i = 0; i < 512; ++i)
    if (std::fabs(y[i]) > std::fabs(y[at])) at = i;
  CHECK(at - 100 >= 32 && at - 100 <= 33, "the impulse response peaks %d samples late, want 32 (33 with the Tone low-pass)", at - 100);
}

TEST(trailOdControlsAreTheBrowsers) {
  const ParamDesc& d = TrailOD::kParams[TrailOD::Drive];
  const ParamDesc& t = TrailOD::kParams[TrailOD::Tone];
  const ParamDesc& l = TrailOD::kParams[TrailOD::Level];
  CHECK(d.min == 0 && d.max == 1 && d.def == 0.5f && t.min == 0 && t.max == 1 && t.def == 0.5f, "Drive and Tone 0..1, 0.5");
  CHECK(l.min == -30 && l.max == 12 && l.def == -6, "Level -30..12 dB, default -6");
}

TEST(trailOdParityNegativeControl) {
  const OdCase& c = kCases[2];
  const Golden g = golden(c.file);
  CHECK(!g.in.empty(), "%s: stored render missing", c.file);
  if (g.in.empty()) return;
  const double wrongTone = parity::esr(render(g.in, c.drive, 0.8f, c.level), g.out);
  CHECK(wrongTone > 100 * parity::kBar, "Tone 0.8 instead of 0.7 must fail the bar: ESR %.3g", wrongTone);
  const double wrongDrive = parity::esr(render(g.in, 0.8f, c.tone, c.level), g.out);
  CHECK(wrongDrive > 100 * parity::kBar, "Drive 0.8 instead of 0.9 must fail the bar: ESR %.3g", wrongDrive);
  std::vector<float> bad = g.out;
  for (float& v : bad) v *= 1.01f;
  CHECK(parity::esr(g.out, bad) > parity::kBar, "a 1 %% level error must fail the bar");
  // The aliasing measure can see aliasing: the same sine hard-clipped with no oversampling reads far worse.
  const Golden s = golden("od-sine.f32");
  if (s.in.empty()) return;
  std::vector<float> clipped = s.in;
  for (float& v : clipped) v = std::fmax(-0.15f, std::fmin(0.15f, v * 4.0f));
  const double naive = aliasingDb(clipped, 550), js = aliasingDb(s.out, 550);
  CHECK(naive > js + 10.0, "a hard-clipped sine aliases %.1f dB against the browser's %.1f dB", naive, js);
}
