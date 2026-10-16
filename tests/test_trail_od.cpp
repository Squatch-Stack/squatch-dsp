// SPDX-License-Identifier: MIT
// Trail OD's behaviour apart from the browser parity (tests/test_parity_trail_od.cpp): silence in, silence
// out; the meters; no allocation is in tests/test_rt.cpp.
#include <squatch/dsp/blocks/trail_od.hpp>

#include <cmath>
#include <vector>

#include "check.hpp"
#include "signals.hpp"

using namespace squatch::dsp;

namespace {

// A plucked-string-like test signal: decaying harmonics of E2 plus a little noise, at a given peak.
std::vector<float> pluck(int n, double rate, float amp) {
  std::vector<float> x(n);
  sig::Rng rng{3};
  for (int i = 0; i < n; ++i) {
    const double t = i / rate, env = std::exp(-1.5 * t);
    double v = 0.0;
    for (int h = 1; h <= 6; ++h) v += std::sin(2.0 * 3.14159265358979323846 * 82.41 * h * t) / h;
    x[i] = static_cast<float>(amp * env * v / 2.45) + 0.001f * rng.uniform();
  }
  return x;
}

}  // namespace

TEST(trailOdIsSilentOnSilence) {
  static TrailOD od;
  Arena none;
  od.prepare(Setup{48000.0, 256}, none);
  std::vector<float> zero(4864, 0.0f), out(4864, 1.0f);  // 19 blocks of 256
  for (size_t i = 0; i < zero.size(); i += 256) od.process(&zero[i], &out[i], 256);
  float peak = 0.0f;
  for (float v : out) peak = std::fmax(peak, std::fabs(v));
  CHECK(peak < 1e-7f, "silence in, %g out", peak);
  CHECK(od.get(TrailOD::Clip) == 0.0f, "no clipping on silence");
}

TEST(trailOdMetersReportPeakAndClipping) {
  static TrailOD od;
  Arena none;
  od.prepare(Setup{48000.0, 256}, none);
  od.set(TrailOD::Drive, 1.0f);
  const std::vector<float> hot = pluck(48128, 48000.0, 1.0f);  // 188 blocks of 256
  std::vector<float> out(hot.size());
  for (size_t i = 0; i < hot.size(); i += 256) od.process(&hot[i], &out[i], 256);
  od.process(&hot[0], &out[0], 256);  // the loudest part of the pluck
  CHECK(od.get(TrailOD::Clip) == 1.0f, "full drive on a hot pluck clips");
  CHECK(od.get(TrailOD::Out) > -40.0f && od.get(TrailOD::Out) <= 0.0f, "output meter %g dBFS", od.get(TrailOD::Out));
}
