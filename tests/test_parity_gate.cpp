// SPDX-License-Identifier: MIT
// Gate against the browser rig's NoiseGate (web/worklets/dsp/gate.js): stored renders
// (tests/golden-parity/gate-*.f32, see gen.mjs) to ESR < 1e-6 and no 10 ms window whose gain differs by more
// than 3 dB, for five settings (one stereo, linked), the browser's ranges and defaults, the meaning of
// Release as a time constant, and negative controls (the old release-as-full-range law, a negative
// hysteresis, a level error: each must fall outside the bar).
#include <squatch/dsp/blocks/gate.hpp>

#include "check.hpp"
#include "parity_stereo.hpp"

using namespace squatch::dsp;

namespace {

alignas(64) unsigned char gMem[1 << 16];

struct GateCase {
  const char* file;
  float threshold, hysteresis, range, attack, hold, release;
  bool stereo;
};
// The browser's NoiseGate parameters, as gen.mjs sets them (default: all defaults).
const GateCase kCases[] = {
    {"gate-default.f32", -58, 6, -70, 1, 40, 90, false},     {"gate-A.f32", -45, 3, -30, 5, 100, 300, false},
    {"gate-B.f32", -35, 0, -90, 0.1f, 0, 5, false},          {"gate-C.f32", -50, 12, -10, 50, 150, 1000, false},
    {"gate-stereo.f32", -40, 6, -60, 2, 60, 150, true},
};

void configure(Gate& g, const GateCase& c, float hysteresis, float release) {
  g.set(Gate::Threshold, c.threshold), g.set(Gate::Hysteresis, hysteresis), g.set(Gate::Range, c.range);
  g.set(Gate::Attack, c.attack), g.set(Gate::Hold, c.hold), g.set(Gate::Release, release);
}

// Both output channels for a stereo case; for a mono case the first one twice.
parity::Stereo render(const parity::Render& g, const GateCase& c, float hysteresis, float release) {
  static Gate gate;
  configure(gate, c, hysteresis, release);
  return parity::runStereo(gate, c.stereo ? Layout::Stereo : Layout::Mono, g.inL, g.inR);
}

}  // namespace

TEST(gateParityWithTheBrowserNoiseGate) {
  for (const GateCase& c : kCases) {
    const parity::Render g = parity::loadRender(c.file);
    CHECK(g.ok(), "%s: stored render missing", c.file);
    if (!g.ok()) continue;
    const parity::Stereo y = render(g, c, c.hysteresis, c.release);
    double e = parity::esr(y.l, g.outL);
    if (c.stereo) e = std::fmax(e, parity::esr(y.r, g.outR));
    const double apart = parity::windowsOver3dB(g.inL, y.l, g.outL);
    std::printf("     gate %s: ESR %.3g, windows over 3 dB apart %.1f %%\n", c.file, e, 100.0 * apart);
    CHECK(e < parity::kBar, "%s: ESR %.3g (bar %.0e)", c.file, e, parity::kBar);
    CHECK(apart == 0.0, "%s: %.1f %% of the windows are more than 3 dB from the browser", c.file, 100.0 * apart);
  }
}

TEST(gateControlsAreTheBrowsersRangesAndDefaults) {
  struct Want {
    const char* name;
    float min, max, def;
    Taper taper;
  } const want[] = {{"Threshold", -90, -10, -58, Taper::Linear}, {"Hysteresis", 0, 20, 6, Taper::Linear},
                    {"Range", -90, 0, -70, Taper::Linear},       {"Attack", 0.1f, 50, 1, Taper::Log},
                    {"Hold", 0, 500, 40, Taper::Linear},         {"Release", 5, 1000, 90, Taper::Log}};
  for (std::size_t i = 0; i < 6; ++i) {
    const ParamDesc& d = Gate::kParams[i];
    CHECK(!std::strcmp(d.name, want[i].name) && d.min == want[i].min && d.max == want[i].max && d.def == want[i].def &&
              d.taper == want[i].taper,
          "%s: %g..%g default %g", want[i].name, d.min, d.max, d.def);
  }
  Gate g;
  g.set(Gate::Threshold, -120.0f), g.set(Gate::Attack, 500.0f), g.set(Gate::Hysteresis, 30.0f);
  CHECK(g.get(Gate::Threshold) == -90.0f && g.get(Gate::Attack) == 50.0f && g.get(Gate::Hysteresis) == 20.0f,
        "values past the browser's ranges clamp to them");
}

// Release is a time constant: a closed gate whose floor drops from -20 to -60 dB covers 63 % of the way in
// the release time (100 ms), so it is at floor + (start - floor) / e then, not at the floor.
TEST(gateReleaseIsATimeConstant) {
  Gate g;
  Arena mem(gMem, sizeof gMem);
  g.prepare(Setup{48000.0, 128}, mem);
  g.set(Gate::Threshold, -40.0f), g.set(Gate::Release, 100.0f), g.set(Gate::Attack, 1.0f);
  g.set(Gate::Range, -20.0f);
  std::vector<float> quiet(48000, 0.0f), out(48000);
  g.process(quiet.data(), out.data(), 48000);  // closed, settled at -20 dB
  g.set(Gate::Range, -60.0f);
  g.process(quiet.data(), out.data(), 4800);  // 100 ms
  const double start = 0.1, floor = 0.001, want = floor + (start - floor) * std::exp(-1.0);
  CHECK(std::fabs(g.gain() - want) < 1e-3 * want, "gain %g after one time constant, want %g", g.gain(), want);
}

// Attack likewise, and a hold of 0 closes at the first sample under the close level.
TEST(gateAttackIsATimeConstantAndHoldZeroClosesAtOnce) {
  Gate g;
  Arena mem(gMem, sizeof gMem);
  g.prepare(Setup{48000.0, 128}, mem);
  g.set(Gate::Threshold, -40.0f), g.set(Gate::Range, -60.0f), g.set(Gate::Attack, 10.0f), g.set(Gate::Hold, 0.0f);
  std::vector<float> loud(480, 0.5f), out(480);
  g.process(loud.data(), out.data(), 480);  // 10 ms open
  const double floor = 0.001, want = 1.0 - (1.0 - floor) * std::exp(-1.0);
  CHECK(std::fabs(g.gain() - want) < 1e-3, "gain %g after one attack time constant, want %g", g.gain(), want);
  CHECK(g.open(), "open under the loud input");
  std::vector<float> zero(48000, 0.0f), z(48000);
  g.process(zero.data(), z.data(), 4800);  // the 20 ms detector fall, then closed at once (hold 0)
  CHECK(!g.open(), "hold 0: closed as soon as the envelope is under the close level");
}

// One NaN or infinity must not lock the detector: afterwards the gate still follows the level.
TEST(gateSurvivesNonFiniteInput) {
  Gate g;
  Arena mem(gMem, sizeof gMem);
  g.prepare(Setup{48000.0, 128}, mem);
  std::vector<float> x(9600, 0.0f), y(9600);
  x[10] = NAN, x[11] = INFINITY, x[12] = -INFINITY;
  g.process(x.data(), y.data(), 9600);
  CHECK(!g.open() && std::isfinite(g.get(Gate::Reduction)), "a bad sample reads as silence");
  std::vector<float> loud(4800, 0.5f);
  g.process(loud.data(), y.data(), 4800);
  CHECK(g.open(), "and the gate still opens on a real signal afterwards");
}

TEST(gateParityNegativeControl) {
  const GateCase& c = kCases[0];
  const parity::Render g = parity::loadRender(c.file);
  CHECK(g.ok(), "%s: stored render missing", c.file);
  if (!g.ok()) return;
  // the old law's release for the same preset: the full range in the same time, i.e. ~8x the rate
  const double wrongRelease = parity::esr(render(g, c, c.hysteresis, c.release / 8.0f).l, g.outL);
  CHECK(wrongRelease > 100 * parity::kBar, "a Release 8x too fast must fail the bar: ESR %.3g", wrongRelease);
  const double apart = parity::windowsOver3dB(g.inL, render(g, c, c.hysteresis, c.release / 8.0f).l, g.outL);
  CHECK(apart > 0.0, "and show up as windows more than 3 dB apart (%.1f %%)", 100.0 * apart);
  const GateCase& h = kCases[3];  // hysteresis 12 dB matters here
  const parity::Render gh = parity::loadRender(h.file);
  if (gh.ok()) {
    const double wrongHyst = parity::esr(render(gh, h, 1.0f, h.release).l, gh.outL);
    CHECK(wrongHyst > 100 * parity::kBar, "Hysteresis 1 dB instead of 12 must fail the bar: ESR %.3g", wrongHyst);
  }
  std::vector<float> bad = g.outL;
  for (float& v : bad) v *= 1.01f;
  CHECK(parity::esr(g.outL, bad) > parity::kBar, "a 1 %% level error must fail the bar");
}
