// SPDX-License-Identifier: MIT
// For the parity tests of the dynamics blocks (gate, limiter): the stored four-array renders
// (inL, inR, outL, outR; see tests/golden-parity/gen.mjs) and a runner that drives a block the way
// a host does, 128 samples at a time, mono or linked stereo, after half a second of silence.
#pragma once
#include <cmath>
#include <vector>

#include <squatch/dsp/block.hpp>

#include "parity_golden.hpp"

namespace parity {

constexpr std::size_t kSamples = 24064;  // per channel, as gen.mjs writes

struct Render {
  std::vector<float> inL, inR, outL, outR;
  bool ok() const { return !inL.empty(); }
};

/// The stored render; `ok()` is false when the file is missing or the wrong size.
inline Render loadRender(const std::string& file) {
  const std::vector<float> all = load(file);
  if (all.size() != 4 * kSamples) return {};
  return {part(all, 0, kSamples), part(all, 1, kSamples), part(all, 2, kSamples), part(all, 3, kSamples)};
}

struct Stereo {
  std::vector<float> l, r;
};

/// `b` prepared for `layout` (the caller has set its parameters), 0.5 s of silence, then the input.
inline Stereo runStereo(squatch::dsp::Block& b, squatch::dsp::Layout layout, const std::vector<float>& inL,
                        const std::vector<float>& inR) {
  using namespace squatch::dsp;
  alignas(64) static unsigned char mem[1 << 20];
  Arena arena(mem, sizeof mem);
  b.prepare(Setup{48000.0, kQuantum, layout}, arena);
  const int ins = inputChannels(layout), outs = outputChannels(layout);
  Stereo out{std::vector<float>(inL.size()), std::vector<float>(inL.size())};
  std::vector<float> zero(kQuantum, 0.0f), sa(kQuantum), sb(kQuantum);
  const float* zi[2] = {zero.data(), zero.data()};
  float* so[2] = {sa.data(), sb.data()};
  for (int i = 0; i < static_cast<int>(0.5 * 48000.0) / kQuantum; ++i) b.processChannels({zi, so, ins, outs}, kQuantum);
  for (std::size_t p = 0; p < inL.size(); p += kQuantum) {
    const float* in[2] = {&inL[p], &inR[p]};
    float* o[2] = {&out.l[p], &out.r[p]};
    b.processChannels({in, o, ins, outs}, kQuantum);
  }
  return out;
}

/// Share (0..1) of 10 ms windows in which the C++ gain (RMS out / RMS in) differs from the browser's by more
/// than 3 dB; windows with no input energy are skipped.
inline double windowsOver3dB(const std::vector<float>& in, const std::vector<float>& cpp, const std::vector<float>& js) {
  const std::size_t w = 480;
  int bad = 0, total = 0;
  for (std::size_t p = 0; p + w <= in.size(); p += w) {
    double si = 0.0, sc = 0.0, sj = 0.0;
    for (std::size_t i = p; i < p + w; ++i) {
      si += static_cast<double>(in[i]) * in[i];
      sc += static_cast<double>(cpp[i]) * cpp[i];
      sj += static_cast<double>(js[i]) * js[i];
    }
    if (si <= 0.0) continue;
    ++total;
    const double gc = 10.0 * std::log10((sc + 1e-30) / si), gj = 10.0 * std::log10((sj + 1e-30) / si);
    if (std::fabs(gc - gj) > 3.0) ++bad;
  }
  return total ? static_cast<double>(bad) / total : 0.0;
}

}  // namespace parity
