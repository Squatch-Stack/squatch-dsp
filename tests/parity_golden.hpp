// SPDX-License-Identifier: MIT
// Shared by the browser-parity tests (test_parity_*.cpp): the stored browser renders in
// tests/golden-parity (written by gen.mjs from squatch-rig's own kernels), the error-to-signal
// ratio the bar is set in, and a runner that feeds a block 128 samples at a time as the browser does.
#pragma once
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <squatch/dsp/block.hpp>

namespace parity {

constexpr int kQuantum = 128;
constexpr double kBar = 1e-6;  // ESR the C++ block must reach against the browser render

/// A file under tests/golden-parity, found from this header's own path.
inline std::string path(const std::string& name) {
  const std::string here = __FILE__;
  return here.substr(0, here.find_last_of('/') + 1) + "golden-parity/" + name;
}

/// The whole stored file as float32; an empty vector means missing.
inline std::vector<float> load(const std::string& name) {
  std::vector<float> v;
  if (FILE* f = std::fopen(path(name).c_str(), "rb")) {
    float buf[4096];
    for (std::size_t n; (n = std::fread(buf, sizeof(float), 4096, f)) > 0;) v.insert(v.end(), buf, buf + n);
    std::fclose(f);
  }
  return v;
}

inline std::vector<float> part(const std::vector<float>& all, std::size_t index, std::size_t length) {
  return std::vector<float>(all.begin() + static_cast<std::ptrdiff_t>(index * length),
                            all.begin() + static_cast<std::ptrdiff_t>((index + 1) * length));
}

/// sum (c - j)^2 / sum j^2 with j the browser (reference) render and c the C++ one.
inline double esr(const std::vector<float>& c, const std::vector<float>& j) {
  double err = 0.0, ref = 0.0;
  for (std::size_t i = 0; i < j.size(); ++i) {
    const double d = static_cast<double>(c[i]) - j[i];
    err += d * d, ref += static_cast<double>(j[i]) * j[i];
  }
  return ref > 0.0 ? err / ref : err;
}

/// Half a second of silence (the smoothers arrive), then `in`, in 128-sample calls, as the browser renders.
inline std::vector<float> run(squatch::dsp::Block& b, const std::vector<float>& in, double rate = 48000.0) {
  std::vector<float> zero(kQuantum, 0.0f), sink(kQuantum), out(in.size());
  for (int i = 0; i < static_cast<int>(0.5 * rate) / kQuantum; ++i) b.process(zero.data(), sink.data(), kQuantum);
  for (std::size_t p = 0; p < in.size(); p += kQuantum) b.process(&in[p], &out[p], kQuantum);
  return out;
}

}  // namespace parity
