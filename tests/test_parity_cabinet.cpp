// SPDX-License-Identifier: MIT
// Cabinet against the browser rig's Cab (CabNode: convolver, 2nd-order low cut and high cut at Q 0.7 dB,
// level), given the rig's own synthetic IR: stored renders (tests/golden-parity/cab-*.f32, gen.mjs, the
// rig's offline WebAudio emulation) to ESR < 1e-6, and negative controls (a wrong cut, and no cuts at
// all, must fall outside the bar).
#include <squatch/dsp/blocks/cabinet.hpp>

#include "check.hpp"
#include "parity_golden.hpp"

using namespace squatch::dsp;

namespace {

constexpr std::size_t kN = 24064, kIr = 4096;
alignas(64) unsigned char gMem[1 << 22];

struct CabCase {
  const char* file;
  float lowcut, highcut, level;
};
// The rig's Cab parameters, as gen.mjs sets them (default: the rig's defaults).
const CabCase kCases[] = {
    {"cab-default.f32", 70.0f, 9000.0f, 0.0f},
    {"cab-B.f32", 150.0f, 5000.0f, -6.0f},
    {"cab-open.f32", 20.0f, 16000.0f, 0.0f},
};

struct Golden {
  std::vector<float> ir, in, out;
};
Golden golden(const char* file) {
  const std::vector<float> all = parity::load(file);
  if (all.size() != kIr + 2 * kN) return {};
  return {std::vector<float>(all.begin(), all.begin() + kIr), parity::part({all.begin() + kIr, all.end()}, 0, kN),
          parity::part({all.begin() + kIr, all.end()}, 1, kN)};
}

// The block with the rig's IR and the case's controls (cuts as asked), run over the stored input.
std::vector<float> render(const Golden& g, const CabCase& c, float lowcut, bool cuts) {
  static Cabinet cab;
  Arena mem(gMem, sizeof gMem);
  cab.configure(g.ir.data(), static_cast<int>(g.ir.size()), Cabinet::kDefaultPartition, cuts);
  cab.set(Cabinet::Lowcut, lowcut), cab.set(Cabinet::Highcut, c.highcut), cab.set(Cabinet::Level, c.level);
  cab.prepare(Setup{48000.0, parity::kQuantum}, mem);
  return parity::run(cab, g.in);
}

}  // namespace

TEST(cabinetParityWithTheBrowserCab) {
  for (const CabCase& c : kCases) {
    const Golden g = golden(c.file);
    CHECK(!g.ir.empty(), "%s: stored render missing", c.file);
    if (g.ir.empty()) continue;
    const double e = parity::esr(render(g, c, c.lowcut, true), g.out);
    std::printf("     cabinet %s: ESR %.3g\n", c.file, e);
    CHECK(e < parity::kBar, "%s: ESR %.3g (bar %.0e)", c.file, e, parity::kBar);
  }
}

TEST(cabinetCutsAreTheBrowsersRangesAndDefaults) {
  const ParamDesc& lo = Cabinet::kParams[Cabinet::Lowcut];
  const ParamDesc& hi = Cabinet::kParams[Cabinet::Highcut];
  CHECK(lo.min == 20 && lo.max == 300 && lo.def == 70 && lo.taper == Taper::Log, "Low cut 20..300 Hz, 70, log");
  CHECK(hi.min == 2000 && hi.max == 16000 && hi.def == 9000 && hi.taper == Taper::Log, "High cut 2..16 kHz, 9 kHz, log");
  static Cabinet cab;
  cab.set(Cabinet::Lowcut, 5.0f), cab.set(Cabinet::Highcut, 1e6f);
  CHECK(cab.get(Cabinet::Lowcut) == 20.0f && cab.get(Cabinet::Highcut) == 16000.0f, "both clamp to the rig's range");
}

// A cut that moves does not click: stepping Low cut from 20 to 300 Hz and High cut from 16 kHz to 2 kHz
// while a sine plays changes the output sample to sample by no more than the signal itself could.
TEST(cabinetCutsMoveWithoutClicks) {
  static Cabinet cab;
  const float h[1] = {1.0f};
  Arena mem(gMem, sizeof gMem);
  cab.configure(h, 1, Cabinet::kDefaultPartition, true);
  cab.set(Cabinet::Lowcut, 20.0f), cab.set(Cabinet::Highcut, 16000.0f);
  cab.prepare(Setup{48000.0, parity::kQuantum}, mem);
  std::vector<float> x(9600);
  for (std::size_t i = 0; i < x.size(); ++i) x[i] = 0.5f * std::sin(0.05f * static_cast<float>(i));
  std::vector<float> y(x.size());
  cab.process(x.data(), y.data(), 4800);
  cab.set(Cabinet::Lowcut, 300.0f), cab.set(Cabinet::Highcut, 2000.0f);
  cab.process(&x[4800], &y[4800], 4800);
  float step = 0.0f;
  for (std::size_t i = 4801; i < y.size(); ++i) step = std::fmax(step, std::fabs(y[i] - y[i - 1]));
  CHECK(step < 0.1f && cab.get(Cabinet::Lowcut) == 300.0f, "largest sample step while moving: %.4f", step);
}

TEST(cabinetParityNegativeControl) {
  const CabCase& c = kCases[0];
  const Golden g = golden(c.file);
  if (g.ir.empty()) return;
  const double wrongCut = parity::esr(render(g, c, 100.0f, true), g.out);
  CHECK(wrongCut > 100 * parity::kBar, "a Low cut at 100 Hz instead of 70 must fail the bar: ESR %.3g", wrongCut);
  const double noCuts = parity::esr(render(g, c, c.lowcut, false), g.out);
  CHECK(noCuts > 100 * parity::kBar, "no cuts at all must fail the bar: ESR %.3g", noCuts);
  std::vector<float> bad = g.out;
  for (float& v : bad) v *= 1.01f;
  CHECK(parity::esr(g.out, bad) > parity::kBar, "a 1 %% level error must fail the bar");
}
