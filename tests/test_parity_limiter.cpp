// SPDX-License-Identifier: MIT
// Limiter against the browser rig's Limiter (web/worklets/dsp/limiter.js). The gain stage is the same
// design, so with the detector swapped for the browser's sample peak (a test-only mode) the stored renders
// (tests/golden-parity/lim-*.f32, see gen.mjs) are matched to ESR < 1e-6, mono and linked stereo. In its
// normal true-peak mode the block keeps the ceiling on the 12 kHz tone where the browser overshoots it.
// Negative controls: a wrong release, the true-peak detector against the sample-peak render, a level error.
#include <squatch/dsp/blocks/limiter.hpp>

#include "check.hpp"
#include "parity_stereo.hpp"
#include "reference.hpp"

using namespace squatch::dsp;

namespace {

struct LimCase {
  const char* file;
  float drive, ceiling, release;
  bool stereo;
};
// The browser Limiter's parameters, as gen.mjs sets them (default: all defaults; its `gain` is Drive).
const LimCase kCases[] = {
    {"lim-default.f32", 0, -1, 120, false}, {"lim-A.f32", 12, -3, 120, false}, {"lim-B.f32", 18, -1, 10, false},
    {"lim-C.f32", 6, -12, 1000, false},     {"lim-stereo.f32", 9, -6, 60, true},
};

parity::Stereo render(const parity::Render& g, const LimCase& c, float release, bool samplePeak) {
  static Limiter lim;
  lim.useSamplePeakForTest(samplePeak);
  lim.set(Limiter::Drive, c.drive), lim.set(Limiter::Ceiling, c.ceiling), lim.set(Limiter::Release, release);
  return parity::runStereo(lim, c.stereo ? Layout::Stereo : Layout::Mono, g.inL, g.inR);
}

}  // namespace

TEST(limiterParityWithTheBrowserLimiter) {
  for (const LimCase& c : kCases) {
    const parity::Render g = parity::loadRender(c.file);
    CHECK(g.ok(), "%s: stored render missing", c.file);
    if (!g.ok()) continue;
    const parity::Stereo y = render(g, c, c.release, true);
    double e = parity::esr(y.l, g.outL);
    if (c.stereo) e = std::fmax(e, parity::esr(y.r, g.outR));
    std::printf("     limiter %s (sample-peak mode): ESR %.3g\n", c.file, e);
    CHECK(e < parity::kBar, "%s: ESR %.3g (bar %.0e)", c.file, e, parity::kBar);
  }
}

// The 12 kHz sine at 45 degrees has samples at 0.636 of full scale and a true peak of 0.9. With a -3 dB
// ceiling the browser's sample-peak limiter never acts and lets 2.1 dB of waveform over; this block holds it.
TEST(limiterKeepsTheCeilingWhereTheBrowserOvershoots) {
  const parity::Render g = parity::loadRender("lim-tone12k.f32");
  CHECK(g.ok(), "stored 12 kHz render missing");
  if (!g.ok()) return;
  const double ceilingDb = -3.0;
  const double browser = sig::db(ref::truePeak(std::vector<float>(g.outL.begin() + 2048, g.outL.end()))) - ceilingDb;
  CHECK(browser > 1.5 && browser < 2.7, "the browser overshoots the ceiling by %+.2f dB (the measured 2.1)", browser);
  static Limiter lim;
  lim.useSamplePeakForTest(false);
  lim.set(Limiter::Ceiling, static_cast<float>(ceilingDb)), lim.set(Limiter::Drive, 0.0f), lim.set(Limiter::Release, 120.0f);
  const parity::Stereo y = parity::runStereo(lim, Layout::Mono, g.inL, g.inR);
  const std::vector<float> settled(y.l.begin() + 2048, y.l.end());
  const double samplePeak = sig::db(sig::peak(settled)) - ceilingDb;
  const double truePeak = sig::db(ref::truePeak(settled)) - ceilingDb;
  std::printf("     12 kHz at -3 dB: browser true peak %+.2f dB over, this block samples %+.3f, true peak %+.3f\n", browser,
              samplePeak, truePeak);
  CHECK(samplePeak <= 0.0, "no output sample over the ceiling: %+.3f dB", samplePeak);
  CHECK(truePeak <= 0.1, "no inter-sample peak over the ceiling + 0.1 dB: %+.3f dB", truePeak);
}

TEST(limiterDefaultsAreTheBrowsers) {
  const ParamDesc& drive = Limiter::kParams[Limiter::Drive];
  const ParamDesc& ceiling = Limiter::kParams[Limiter::Ceiling];
  const ParamDesc& release = Limiter::kParams[Limiter::Release];
  const ParamDesc& output = Limiter::kParams[Limiter::Output];
  CHECK(drive.min == 0 && drive.max == 18 && drive.def == 0, "Drive 0..18 dB, default 0");
  CHECK(ceiling.def == -1 && ceiling.min <= -12 && ceiling.max == 0, "Ceiling default -1 dB, the browser's -12..0 inside");
  CHECK(release.min == 10 && release.max == 1000 && release.def == 120 && release.taper == Taper::Log,
        "Release 10..1000 ms, log, default 120 (the browser's)");
  CHECK(output.def == 0 && output.max == 0, "Output trim defaults to 0 dB and only attenuates");
}

// The browser's detector is a test switch: a Limiter nobody switched is true-peak (1.5 ms lookahead + 16 taps
// of interpolator delay), and the switch gives the browser's 2 ms window less one sample of delay.
TEST(limiterSamplePeakModeIsOffByDefault) {
  alignas(64) static unsigned char mem[1 << 16];
  Limiter normal, browser;
  browser.useSamplePeakForTest(true);
  Arena a(mem, sizeof mem);
  normal.prepare(Setup{48000.0, 128}, a);
  Arena b(mem, sizeof mem);
  browser.prepare(Setup{48000.0, 128}, b);
  CHECK(normal.latency() == 72 + 16, "true-peak latency %d, want 88", normal.latency());
  CHECK(browser.latency() == 96 - 1, "browser-mode latency %d, want 95", browser.latency());
}

TEST(limiterParityNegativeControl) {
  const LimCase& c = kCases[1];
  const parity::Render g = parity::loadRender(c.file);
  CHECK(g.ok(), "%s: stored render missing", c.file);
  if (!g.ok()) return;
  const double wrongRelease = parity::esr(render(g, c, 60.0f, true).l, g.outL);
  CHECK(wrongRelease > 100 * parity::kBar, "a Release of 60 ms instead of 120 must fail the bar: ESR %.3g", wrongRelease);
  const double truePeakMode = parity::esr(render(g, c, c.release, false).l, g.outL);
  CHECK(truePeakMode > 100 * parity::kBar, "the true-peak detector (and its latency) against the sample-peak render must fail: ESR %.3g",
        truePeakMode);
  std::vector<float> bad = g.outL;
  for (float& v : bad) v *= 1.01f;
  CHECK(parity::esr(g.outL, bad) > parity::kBar, "a 1 %% level error must fail the bar");
}
