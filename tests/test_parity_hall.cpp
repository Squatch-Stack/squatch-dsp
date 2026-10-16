// SPDX-License-Identifier: MIT
// Hall against the browser rig's Reverb (web/worklets/dsp/reverb.js): the stored left and right
// renders (tests/golden-parity/hall-*.f32, see gen.mjs) to ESR < 1e-6 in all three layouts, the
// mono output being the left channel, the Size range, Width 0 collapsing the sides, and a negative
// control (a wrong Width must fail the bar, so the bar can fail).
#include <squatch/dsp/blocks/hall.hpp>

#include "check.hpp"
#include "parity_golden.hpp"

using namespace squatch::dsp;

namespace {

constexpr std::size_t kN = 24064;
alignas(64) unsigned char gMem[1 << 20];

struct HallCase {
  const char* file;
  float decay, size, damp, predelay, mix, width;
  bool stereoIn;
};
// The browser's Reverb parameters, as gen.mjs sets them (A: all defaults).
const HallCase kCases[] = {
    {"hall-A.f32", 2.4f, 1.0f, 6500.0f, 18.0f, 0.25f, 1.0f, false},
    {"hall-B.f32", 1.2f, 1.6f, 3000.0f, 5.0f, 0.5f, 0.4f, false},
    {"hall-C.f32", 4.0f, 0.3f, 12000.0f, 40.0f, 1.0f, 0.0f, true},
};

void configure(Hall& h, const HallCase& c, float width) {
  h.set(Hall::Decay, c.decay), h.set(Hall::Size, c.size), h.set(Hall::Damp, c.damp);
  h.set(Hall::Predelay, c.predelay), h.set(Hall::Mix, c.mix), h.set(Hall::Width, width);
}

struct Stereo {
  std::vector<float> l, r;
};

// The block as the host drives it in `layout`, 128 samples at a time after 0.5 s of silence.
Stereo runLayout(Hall& h, Layout layout, const std::vector<float>& inL, const std::vector<float>& inR) {
  Arena mem(gMem, sizeof gMem);
  h.prepare(Setup{48000.0, parity::kQuantum, layout}, mem);
  Stereo out{std::vector<float>(inL.size()), std::vector<float>(inL.size())};
  std::vector<float> zero(parity::kQuantum, 0.0f), sa(parity::kQuantum), sb(parity::kQuantum);
  const int ins = inputChannels(layout), outs = outputChannels(layout);
  const float* zi[2] = {zero.data(), zero.data()};
  float* so[2] = {sa.data(), sb.data()};
  for (int i = 0; i < 24000 / parity::kQuantum; ++i) h.processChannels({zi, so, ins, outs}, parity::kQuantum);
  for (std::size_t p = 0; p < inL.size(); p += parity::kQuantum) {
    const float* in[2] = {&inL[p], &inR[p]};
    float* o[2] = {&out.l[p], &out.r[p]};
    h.processChannels({in, o, ins, outs}, parity::kQuantum);
  }
  return out;
}

struct Golden {
  std::vector<float> inL, inR, outL, outR;
};
Golden golden(const char* file) {
  const std::vector<float> all = parity::load(file);
  if (all.size() != 4 * kN) return {};
  return {parity::part(all, 0, kN), parity::part(all, 1, kN), parity::part(all, 2, kN), parity::part(all, 3, kN)};
}

}  // namespace

TEST(hallParityWithTheBrowserReverb) {
  for (const HallCase& c : kCases) {
    static Hall h;
    const Golden g = golden(c.file);
    CHECK(!g.inL.empty(), "%s: stored render missing", c.file);
    if (g.inL.empty()) continue;
    configure(h, c, c.width);
    const Layout layout = c.stereoIn ? Layout::Stereo : Layout::MonoToStereo;
    const Stereo y = runLayout(h, layout, g.inL, g.inR);
    const double el = parity::esr(y.l, g.outL), er = parity::esr(y.r, g.outR);
    std::printf("     hall %s (%s): ESR left %.3g, right %.3g\n", c.file, c.stereoIn ? "stereo in" : "mono in", el, er);
    CHECK(el < parity::kBar && er < parity::kBar, "%s: ESR left %.3g, right %.3g (bar %.0e)", c.file, el, er, parity::kBar);
  }
}

TEST(hallMonoOutputIsTheLeftChannel) {
  for (const HallCase& c : kCases) {
    if (c.stereoIn) continue;
    static Hall mono, pair;
    const Golden g = golden(c.file);
    if (g.inL.empty()) continue;
    configure(mono, c, c.width), configure(pair, c, c.width);
    Arena mem(gMem, sizeof gMem);
    mono.prepare(Setup{48000.0, parity::kQuantum, Layout::Mono}, mem);
    const std::vector<float> y = parity::run(mono, g.inL);
    const double e = parity::esr(y, g.outL);
    CHECK(e < parity::kBar, "%s: mono process() against the browser's left channel: ESR %.3g", c.file, e);
  }
}

TEST(hallWidthZeroPutsTheSameWetSignalOnBothSides) {
  static Hall h;
  const Golden g = golden("hall-A.f32");
  if (g.inL.empty()) return;
  HallCase c = kCases[0];
  c.mix = 1.0f;
  configure(h, c, 0.0f);
  const Stereo y = runLayout(h, Layout::MonoToStereo, g.inL, g.inR);
  double worst = 0.0, peak = 0.0;
  for (std::size_t i = 0; i < kN; ++i) worst = std::fmax(worst, std::fabs(y.l[i] - y.r[i])), peak = std::fmax(peak, std::fabs(y.l[i]));
  CHECK(peak > 0.01 && worst < 1e-6, "width 0: left and right differ by %.3g (peak %.3g)", worst, peak);
  configure(h, c, 1.0f);
  const Stereo w = runLayout(h, Layout::MonoToStereo, g.inL, g.inR);
  worst = 0.0;
  for (std::size_t i = 0; i < kN; ++i) worst = std::fmax(worst, std::fabs(w.l[i] - w.r[i]));
  CHECK(worst > 0.01, "width 1: the two sides are decorrelated (largest difference %.3g)", worst);
}

TEST(hallSizeRangeIsTheBrowsers) {
  CHECK(Hall::kParams[Hall::Size].min == 0.3f && std::fabs(Hall::kParams[Hall::Size].max - 1.6f) < 2e-7f, "Size 0.3 .. 1.6");
  CHECK(Hall::kParams[Hall::Width].min == 0.0f && Hall::kParams[Hall::Width].max == 1.0f && Hall::kParams[Hall::Width].def == 1.0f,
        "Width 0 .. 1, default 1");
  static Hall h;
  h.set(Hall::Size, 9.0f);
  CHECK(std::fabs(h.get(Hall::Size) - 1.6f) < 2e-7f, "Size clamps at the browser's 1.6");
}

// Negative control: the same comparison with the wrong Width, and with one reference sample
// disturbed, must fall well outside the bar. If these passed, the parity test above would prove nothing.
TEST(hallParityNegativeControl) {
  static Hall h;
  const HallCase& c = kCases[1];
  const Golden g = golden(c.file);
  if (g.inL.empty()) return;
  configure(h, c, 0.9f);
  const Stereo y = runLayout(h, Layout::MonoToStereo, g.inL, g.inR);
  const double wrongWidth = parity::esr(y.r, g.outR);
  CHECK(wrongWidth > 100 * parity::kBar, "a Width off by 0.5 must fail the bar: ESR %.3g", wrongWidth);
  std::vector<float> bad = g.outL;
  for (float& v : bad) v *= 1.01f;  // a 1 % level error is an ESR of 1e-4
  CHECK(parity::esr(g.outL, bad) > parity::kBar, "a disturbed reference must fail the bar: ESR %.3g", parity::esr(g.outL, bad));
}
