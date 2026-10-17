// SPDX-License-Identifier: MIT
// Host tests for the Seed3's A2-Lite kernel (hosts/daisy/nam/a2lite.hpp): against an independent double-precision
// implementation of the network written straight from the weights, for any block size, with no allocation.
#include <cmath>
#include <vector>

#include "../../../tests/check.hpp"
#include "../../../tests/rt_watch.hpp"
#include "../../../tests/signals.hpp"
#include "../nam/a2lite.hpp"

using namespace squatch;
using squatch::nam::A2Lite;

namespace {

// The definition: direct sums over the full signal in double precision (zero history before the start).
using Plane = std::vector<double>;

// One layer's activations (and its head-sum contribution) from the layer input h; w points at its weights.
struct LayerRef {
  const float* w;
  int K, D;
};

std::vector<Plane> layerActivations(const LayerRef& L, const std::vector<Plane>& h, const std::vector<float>& cond,
                                    std::vector<Plane>& head) {
  const float* w = L.w;
  const int K = L.K, D = L.D, C = 3, N = static_cast<int>(cond.size());
  const float* cb = w + C * C * K;
  const float* mix = cb + C;
  std::vector<Plane> a(C, Plane(N));
  for (int o = 0; o < C; ++o)
    for (int i = 0; i < N; ++i) {
      double z = 0.0;
      for (int j = 0; j < C; ++j)
        for (int k = 0; k < K; ++k) {
          const int t = i - (K - 1 - k) * D;
          if (t >= 0) z += double(w[(o * C + j) * K + k]) * h[j][t];
        }
      z += double(cb[o]) + double(mix[o]) * cond[i];
      a[o][i] = z >= 0.0 ? z : z * 0.01;
      head[o][i] += a[o][i];
    }
  return a;
}

// The head: a 16-tap convolution over the summed activations, plus bias, times the scale; w points at head[3][16].
std::vector<double> headOutput(const float* w, const std::vector<Plane>& head) {
  const int C = 3, HK = 16, N = static_cast<int>(head[0].size());
  const double hb = w[C * HK], hs = w[C * HK + 1];
  std::vector<double> y(N);
  for (int i = 0; i < N; ++i) {
    double v = hb;
    for (int c = 0; c < C; ++c)
      for (int k = 0; k < HK; ++k) v += i - (HK - 1 - k) >= 0 ? double(w[c * HK + k]) * head[c][i - (HK - 1 - k)] : 0.0;
    y[i] = v * hs;
  }
  return y;
}

std::vector<double> reference(const std::vector<float>& xin) {
  const int C = 3, N = static_cast<int>(xin.size());
  const int ks[23] = {6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 15, 15, 6, 6, 6, 6, 6, 6, 6};
  const int ds[23] = {1, 3, 7, 17, 41, 101, 239, 1, 3, 7, 17, 41, 101, 239, 1, 13, 1, 3, 7, 17, 41, 101, 239};
  const float* w = nam::kA2LiteWeights;
  std::vector<Plane> h(C, Plane(N)), head(C, Plane(N, 0.0));
  for (int c = 0; c < C; ++c)
    for (int i = 0; i < N; ++i) h[c][i] = double(w[c]) * xin[i];
  w += C;
  for (int l = 0; l < 23; ++l) {
    const int K = ks[l];
    const std::vector<Plane> a = layerActivations({w, K, ds[l]}, h, xin, head);
    const float* l1 = w + C * C * K + 2 * C;
    const float* l1b = l1 + C * C;
    w = l1b + C;
    for (int o = 0; o < C; ++o)
      for (int i = 0; i < N; ++i) {
        double t = l1b[o];
        for (int j = 0; j < C; ++j) t += double(l1[o * C + j]) * a[j][i];
        h[o][i] += t;
      }
  }
  return headOutput(w, head);
}

std::vector<float> render(const std::vector<float>& x, bool randomBlocks) {
  static A2Lite model;  // ~100 kB of state: static, as on the board
  dsp::Arena none;
  model.prepare(dsp::Setup{48000.0, 64}, none);
  std::vector<float> y(x.size());
  sig::Rng rng{5};
  for (std::size_t i = 0; i < x.size();) {
    const int n = std::min<int>(static_cast<int>(x.size() - i), randomBlocks ? rng.range(1, 64) : 32);
    model.process(&x[i], &y[i], n);
    i += static_cast<std::size_t>(n);
  }
  return y;
}

}  // namespace

TEST(daisyA2LiteReceptiveFieldIsNamCores) {
  CHECK(A2Lite::receptiveField() == 6347, "receptive field %d", A2Lite::receptiveField());
  CHECK(nam::kA2LiteWeightCount == 1871, "weights");
}

TEST(daisyA2LiteMatchesTheDefinition) {
  const int n = 3000;  // a tap reaches at most 1195 frames back, so this exercises the ring wrap many times
  std::vector<float> x = sig::noise(n, 0.3f, 99);
  const std::vector<float> tone = sig::sine(180.0, n, 48000.0, 0.4);
  for (int i = 0; i < n; ++i) x[i] += tone[i];
  const std::vector<double> ref = reference(x);
  const std::vector<float> y = render(x, false);
  double num = 0.0, den = 0.0, worst = 0.0;
  for (int i = 0; i < n; ++i) {
    num += (y[i] - ref[i]) * (y[i] - ref[i]);
    den += ref[i] * ref[i];
    worst = std::fmax(worst, std::fabs(y[i] - ref[i]));
  }
  CHECK(den > 0.0 && num / den < 1e-10, "ESR against the double-precision definition %.3g (peak error %.3g)", num / den, worst);
  std::printf("     A2-Lite float32 vs float64 definition: ESR %.3g (%.1f dB), peak error %.3g\n", num / den,
              10.0 * std::log10(num / den), worst);
}

TEST(daisyA2LiteDoesNotDependOnTheBlockSize) {
  const std::vector<float> x = sig::noise(2500, 0.4f, 7);
  const std::vector<float> a = render(x, false), b = render(x, true);
  bool same = true;
  for (std::size_t i = 0; i < x.size(); ++i) same = same && a[i] == b[i];
  CHECK(same, "32-sample blocks and random blocks of 1 to 64 give the same bits");
}

TEST(daisyA2LiteNeverAllocatesOrLocks) {
  static A2Lite model;
  dsp::Arena none;
  model.prepare(dsp::Setup{48000.0, 64}, none);
  std::vector<float> x = sig::noise(64, 0.3f, 3), y(64);
  rt::arm();
  for (int i = 0; i < 20; ++i) model.process(x.data(), y.data(), 32 + (i % 3) * 16);
  const rt::Counts c = rt::disarm();
  CHECK(c.total() == 0, "process() allocated or locked: %ld events", c.total());
}
