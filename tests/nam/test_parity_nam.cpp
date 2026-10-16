// SPDX-License-Identifier: MIT
// The NAM adapter (hosts/common/nam_block) against the browser rig's NAM head (namhead.js + nam.wasm),
// both running the MIT NAM Core example models A1 (no stated loudness) and A2 (-26.38 dB): stored
// renders (tests/golden-parity/nam-*.f32, gen.mjs) to ESR < 1e-6 with input gain, output gain,
// Bass, Normalize and the DC block each exercised, plus negative controls (each stage switched the
// wrong way must fall outside the bar). Built against NAM Core by tests/nam/run.sh, not by `make test`.
#include "../../hosts/common/nam_block.hpp"

#include "../check.hpp"
#include "../parity_golden.hpp"

using namespace squatch::dsp;

namespace {

constexpr std::size_t kN = 24064;
alignas(64) unsigned char gMem[1 << 22];

struct NamCase {
  const char* file;
  const char* model;
  float input, output, bass, normalize, dcblock;
};
// The head's parameters, as gen.mjs sets them (an omitted one is the head's default).
const NamCase kCases[] = {
    {"nam-A1_default.f32", "namcore-example-A1.nam", 0, 0, 0, 1, 1},
    {"nam-A1_gain_bass.f32", "namcore-example-A1.nam", 6, -3, -6, 1, 1},
    {"nam-A1_nodc.f32", "namcore-example-A1.nam", 0, 0, 0, 1, 0},
    {"nam-A2_default.f32", "namcore-example-A2.nam", 0, 0, 0, 1, 1},
    {"nam-A2_nonorm.f32", "namcore-example-A2.nam", 0, 0, -12, 0, 0},
};

using Traits = squatch::clap::PluginTraits<NamAmp>;

// The adapter as the plugin builds it (the real loader, then configure), with `c`'s controls.
std::vector<float> render(const NamCase& c, const std::vector<float>& in, NamCase wrong) {
  static NamAmp amp;
  Traits::File file;
  std::string error;
  const bool loaded = Traits::load(parity::path(std::string("models/") + c.model), 48000.0, file, error);
  CHECK(loaded, "%s: %s", c.model, error.c_str());
  if (!loaded) return {};
  Traits::configure(amp, file);
  amp.set(NamAmp::Input, wrong.input), amp.set(NamAmp::Output, wrong.output), amp.set(NamAmp::Bass, wrong.bass);
  amp.set(NamAmp::Normalize, wrong.normalize), amp.set(NamAmp::Dcblock, wrong.dcblock);
  Arena mem(gMem, sizeof gMem);
  amp.prepare(Setup{48000.0, parity::kQuantum}, mem);
  CHECK(amp.get(NamAmp::Loaded) == 1.0f, "%s: the model is running", c.model);
  return parity::run(amp, in);
}

struct Golden {
  std::vector<float> in, out;
};
Golden golden(const char* file) {
  const std::vector<float> all = parity::load(file);
  if (all.size() != 2 * kN) return {};
  return {parity::part(all, 0, kN), parity::part(all, 1, kN)};
}

}  // namespace

TEST(namParityWithTheBrowserHead) {
  for (const NamCase& c : kCases) {
    const Golden g = golden(c.file);
    CHECK(!g.in.empty(), "%s: stored render missing", c.file);
    if (g.in.empty()) continue;
    const double e = parity::esr(render(c, g.in, c), g.out);
    std::printf("     nam %s: ESR %.3g\n", c.file, e);
    CHECK(e < parity::kBar, "%s: ESR %.3g (bar %.0e)", c.file, e, parity::kBar);
  }
}

TEST(namControlsAreTheHeadsRangesAndDefaults) {
  const ParamDesc& bass = NamAmp::kParams[NamAmp::Bass];
  CHECK(bass.min == -12 && bass.max == 0 && bass.def == 0, "Bass -12..0 dB, default 0");
  CHECK(NamAmp::kParams[NamAmp::Normalize].def == 1 && NamAmp::kParams[NamAmp::Normalize].steps == 1, "Normalize: a switch, on");
  CHECK(NamAmp::kParams[NamAmp::Dcblock].def == 1 && NamAmp::kParams[NamAmp::Dcblock].steps == 1, "DC block: a switch, on");
  CHECK(NamAmp::kParams[NamAmp::Input].min == -24 && NamAmp::kParams[NamAmp::Input].max == 24, "Input -24..24 dB");
  CHECK(NamAmp::kParams[NamAmp::Output].min <= -24 && NamAmp::kParams[NamAmp::Output].max >= 24, "Output covers the head's Master -24..24 dB");
}

// Negative controls: with a stage set the wrong way against the stored render, the same test must fail.
TEST(namParityNegativeControl) {
  struct Wrong {
    int golden;
    const char* what;
    NamCase c;
  };
  const Wrong wrongs[] = {
      {1, "Bass left at 0", {"", "namcore-example-A1.nam", 6, -3, 0, 1, 1}},
      {0, "DC block off", {"", "namcore-example-A1.nam", 0, 0, 0, 1, 0}},
      {3, "Normalize off", {"", "namcore-example-A2.nam", 0, 0, 0, 0, 1}},
      {1, "Output 1 dB off", {"", "namcore-example-A1.nam", 6, -2, -6, 1, 1}},
  };
  for (const Wrong& w : wrongs) {
    const NamCase& real = kCases[w.golden];
    const Golden g = golden(real.file);
    if (g.in.empty()) continue;
    const double e = parity::esr(render(real, g.in, w.c), g.out);
    CHECK(e > 100 * parity::kBar, "%s must fail the bar: ESR %.3g", w.what, e);
  }
  const Golden g = golden(kCases[0].file);
  std::vector<float> bad = g.out;
  for (float& v : bad) v *= 1.01f;
  CHECK(parity::esr(g.out, bad) > parity::kBar, "a 1 %% level error must fail the bar");
}
