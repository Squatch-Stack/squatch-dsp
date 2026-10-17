// SPDX-License-Identifier: MIT
// A2-Lite (NAM's 3-channel WaveNet, 23 layers, 1,871 weights) in float32 for the Seed3's Cortex-M7, as a Block.
// Written for this board: fixed sizes, no heap, no arena, no libm, planar blocks with a tile of 8 frames kept in
// registers. The weights are NeuralAmpModelerCore's own example model (nam_to_header.py).
// The network, in NAM Core's weight order (checked against tools/a2_reference.py's float64 oracle):
//   rechannel[3]; per layer: conv[out][in][tap] (3 x 3 x K), conv bias[3], mixin[3], 1x1[out][in], 1x1 bias[3];
//   head[3][16], head bias, head scale. Layer l: z = sum conv(history of the layer input) + bias + mixin * input;
//   a = LeakyReLU(z, 0.01); head_sum += a; layer input += 1x1(a) + bias. Output = (head conv over head_sum + bias) * scale.
// Each layer keeps its input history in a mirrored ring (a plane per channel, N = lookback + kMaxBlock columns plus a
// kMaxBlock-column mirror), so every tap's run of frames is contiguous with no copying but the new block. The sum for one
// output is always formed in the same order (channel, then tap): the output does not depend on the block size or the tile.
// ~100 KB of RAM, in .bss. NAMOPT_FUSED=1 (the Seed3 images; tools/nam_bench.py measures it against the plain kernel on the
// board) costs 46% of the plain kernel's cycles with bit-for-bit the same output:
//   - all three outputs of a tile of 4 frames are accumulated together (12 accumulators), so each loaded input is used
//     three times; the weights are repacked [input][tap][output] in a copy;
//   - the activation is max(z, 0.01 z) (the same value for every finite z, and branch free);
//   - the epilogue keeps its constants in locals and stores the residual stream straight into the next layer's ring (rings
//     are a whole number of 8 columns): no x_ array, no per-layer copy. A block that is not a whole number of tiles, or
//     does not start on one, takes the plain path.
// NAMOPT_DTCM=1 (set in the image's main.cpp) puts the whole object in the 128 KB DTCM.
// NAMOPT_SLIDEH=1: the head conv does 4 frames, two taps per pass: 2.5% fewer cycles, same sum order (a window over the
// dilation-1 layers gained 0.2%, NAM Core's a2_fast layout lost 35%: docs/benchmarks/daisy-seed3.md).
#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>
#include <squatch/dsp/block.hpp>
#include <squatch/dsp/param.hpp>

#include "a2lite_weights.hpp"

#ifndef NAMOPT_FUSED
#define NAMOPT_FUSED 0
#endif
#ifndef NAMOPT_SLIDEH
#define NAMOPT_SLIDEH 0  // the head conv takes two taps per pass over a sliding window of columns (bit-identical)
#endif
#ifndef NAMOPT_BREAK
#define NAMOPT_BREAK 0  // test only: the output scale is 1 % off, the negative control of tools/nam_bench.py's accuracy gate
#endif

namespace squatch::nam {

namespace detail {
constexpr int kC = 3;
constexpr int kLayers = 23;
constexpr int kHeadK = 16;
constexpr int kMaxBlock = 64;

constexpr int kernel(int l) { return l == 14 || l == 15 ? 15 : 6; }
constexpr int dilation(int l) {
  constexpr int d[kLayers] = {1, 3, 7, 17, 41, 101, 239, 1, 3, 7, 17, 41, 101, 239, 1, 13, 1, 3, 7, 17, 41, 101, 239};
  return d[l];
}
// Columns of one ring: the lookback plus a block (a whole number of 8 columns for the fused kernel, so a tile never
// straddles the wrap), and of one plane: that and the block's mirror, rounded up to 4.
constexpr int ringLen(int lookback) { return NAMOPT_FUSED ? (lookback + kMaxBlock + 7) & ~7 : lookback + kMaxBlock; }
constexpr int ringCols(int lookback) { return (ringLen(lookback) + kMaxBlock + 3) & ~3; }
constexpr unsigned totalFloats() {
  unsigned t = static_cast<unsigned>(kC) * static_cast<unsigned>(ringCols(kHeadK - 1));
  for (int l = 0; l < kLayers; ++l) t += static_cast<unsigned>(kC) * static_cast<unsigned>(ringCols((kernel(l) - 1) * dilation(l)));
  return t;
}
}  // namespace detail

class A2Lite final : public dsp::Block {
 public:
  static constexpr int kC = detail::kC;
  static constexpr int kLayers = detail::kLayers;
  static constexpr int kHeadK = detail::kHeadK;
  static constexpr int kMaxBlock = detail::kMaxBlock;
  static constexpr int kTile = 8;
  static constexpr float kSlope = 0.01f;

  A2Lite() { layout(); }

  void prepare(const dsp::Setup&, dsp::Arena&) override { reset(); }

  void reset() override {
    std::memset(ring_, 0, sizeof ring_);
    for (Layer& l : layer_) l.wpos = l.bpos = 0;
    head_.wpos = head_.bpos = 0;
  }

  void set(dsp::ParamId, float) override {}
  float get(dsp::ParamId) const override { return NAN; }  // no parameters

  /// Samples of history the network reads: 1 + sum (K-1) D + (kHeadK - 1).
  static constexpr int receptiveField() {
    int r = 1 + kHeadK - 1;
    for (int l = 0; l < kLayers; ++l) r += (detail::kernel(l) - 1) * detail::dilation(l);
    return r;
  }

  /// Bytes of state (rings), for the report.
  static constexpr unsigned stateBytes() { return sizeof(ring_); }

  void process(const float* in, float* out, int n) override {
    while (n > kMaxBlock) {
      block(in, out, kMaxBlock);
      in += kMaxBlock;
      out += kMaxBlock;
      n -= kMaxBlock;
    }
    if (n > 0) block(in, out, n);
  }

 private:
  // One mirrored ring of kC planes.
  struct Ring {
    float* buf = nullptr;
    int N = 0, cap = 0, wpos = 0, bpos = 0;
  };
  struct Layer : Ring {
    int K = 0, D = 0;
    const float *w = nullptr, *wf = nullptr, *bias = nullptr, *mixin = nullptr, *l1 = nullptr, *l1b = nullptr;
  };

  // Splits the ring storage and the weight stream among the layers.
  void layout() {
    const float* w = kA2LiteWeights;
    rechannel_ = w;
    w += kC;
    float* mem = ring_;
    for (int l = 0; l < kLayers; ++l) {
      Layer& L = layer_[l];
      L.K = detail::kernel(l);
      L.D = detail::dilation(l);
      const int lookback = (L.K - 1) * L.D;
      L.N = detail::ringLen(lookback);
      L.cap = detail::ringCols(lookback);
      L.buf = mem;
      mem += kC * L.cap;
      L.w = w;
#if NAMOPT_FUSED
      L.wf = repack(w, L.K, wfast_ + (w - kA2LiteWeights));
#endif
      w += kC * kC * L.K;
      L.bias = w;
      w += kC;
      L.mixin = w;
      w += kC;
      L.l1 = w;
      w += kC * kC;
      L.l1b = w;
      w += kC;
    }
    head_.N = detail::ringLen(kHeadK - 1);
    head_.cap = detail::ringCols(kHeadK - 1);
    head_.buf = mem;
    headW_ = w;
    w += kC * kHeadK;
    headB_ = *w++;
    headScale_ = *w++;
#if NAMOPT_BREAK
    headScale_ *= 1.01f;
#endif
  }

  // Append n frames (planar, plane stride kMaxBlock) to the ring, keeping the mirror of its first kMaxBlock columns.
  static void write(Ring& r, const float* src, int n) {
    r.bpos = r.wpos;
    const int p0 = r.wpos;
    const int n1 = p0 + n <= r.N ? n : r.N - p0;
    for (int c = 0; c < kC; ++c) {
      float* plane = r.buf + c * r.cap;
      const float* s = src + c * kMaxBlock;
      std::memcpy(plane + p0, s, sizeof(float) * static_cast<unsigned>(n1));
      if (n1 < n) std::memcpy(plane, s + n1, sizeof(float) * static_cast<unsigned>(n - n1));
      if (p0 < kMaxBlock) {
        const int m = p0 + n1 <= kMaxBlock ? n1 : kMaxBlock - p0;
        std::memcpy(plane + r.N + p0, s, sizeof(float) * static_cast<unsigned>(m));
      }
      if (n1 < n) {
        const int m = n - n1 <= kMaxBlock ? n - n1 : kMaxBlock;
        std::memcpy(plane + r.N, s + n1, sizeof(float) * static_cast<unsigned>(m));
      }
    }
    r.wpos = p0 + n >= r.N ? p0 + n - r.N : p0 + n;
  }

  // First column of the run for a tap `back` frames before this block's frame 0.
  static int start(const Ring& r, int back) {
    const int s = r.bpos - back;
    return s < 0 ? s + r.N : s;
  }

  // The convolution, activation and head sum for T frames starting at f0; leaves the activations in act[][].
  template <int T>
  void convTile(const Layer& L, const int* st, int f0, const float* cond, float (*act)[kTile], float (*head)[kMaxBlock]) {
    for (int o = 0; o < kC; ++o) {
      float acc[T];
      for (int i = 0; i < T; ++i) acc[i] = 0.0f;
      for (int j = 0; j < kC; ++j) {
        const float* plane = L.buf + j * L.cap;
        const float* wj = L.w + (o * kC + j) * L.K;
        for (int k = 0; k < L.K; ++k) {
          const float w = wj[k];
          const float* h = plane + st[k] + f0;
          for (int i = 0; i < T; ++i) acc[i] += w * h[i];
        }
      }
      const float b = L.bias[o], m = L.mixin[o];
      for (int i = 0; i < T; ++i) {
        const float z = (acc[i] + b) + m * cond[f0 + i];
        const float a = z >= 0.0f ? z : z * kSlope;
        act[o][i] = a;
        head[o][f0 + i] += a;
      }
    }
  }

  template <int T>
  void residualTile(const Layer& L, int f0, float (*act)[kTile], float (*x)[kMaxBlock]) {
    for (int o = 0; o < kC; ++o) {
      for (int i = 0; i < T; ++i) {
        float t = L.l1[o * kC] * act[0][i];
        for (int j = 1; j < kC; ++j) t += L.l1[o * kC + j] * act[j][i];
        x[o][f0 + i] += t + L.l1b[o];
      }
    }
  }

#if NAMOPT_FUSED
  static constexpr int kFast = 4;  // frames per fused tile

  // [out][in][tap] -> [in][tap][out] into dst, so the three outputs' weights for one input and tap are adjacent.
  static const float* repack(const float* w, int K, float* dst) {
    for (int o = 0; o < kC; ++o)
      for (int j = 0; j < kC; ++j)
        for (int k = 0; k < K; ++k) dst[(j * K + k) * kC + o] = w[(o * kC + j) * K + k];
    return dst;
  }

  // The convolution of one tile for all three outputs, summed channel by channel and tap by tap, as the plain kernel does.
  static void convFused(const Layer& L, const int* st, int f0, float (&a)[kC][kFast]) {
    for (auto& row : a)
      for (float& v : row) v = 0.0f;
    const float* wp = L.wf;
    for (int j = 0; j < kC; ++j) {
      const float* plane = L.buf + j * L.cap + f0;
      for (int k = 0; k < L.K; ++k, wp += kC) {
        const float* h = plane + st[k];
        for (int i = 0; i < kFast; ++i) {
          const float x = h[i];
          for (int o = 0; o < kC; ++o) a[o][i] += wp[o] * x;
        }
      }
    }
  }

  // Bias, mixin, activation and the head sum; the activations land in v.
  void activateFused(const Layer& L, int f0, const float (&a)[kC][kFast], float (&v)[kC][kFast]) {
    const float b[kC] = {L.bias[0], L.bias[1], L.bias[2]}, m[kC] = {L.mixin[0], L.mixin[1], L.mixin[2]};
    for (int i = 0; i < kFast; ++i) {
      const float c = cond_[f0 + i];
      for (int o = 0; o < kC; ++o) {
        const float z = (a[o][i] + b[o]) + m[o] * c;
        v[o][i] = std::fmax(z, z * kSlope);
        head_sum_[o][f0 + i] += v[o][i];
      }
    }
  }

  // The 1x1 residual: this layer's input (its newest tap) plus 1x1(v) + bias, stored into the next layer's ring and its mirror.
  static void residualFused(const Layer& L, Ring& nx, int xs, int f0, const float (&v)[kC][kFast]) {
    float r[kC][kC], q[kC];
    for (int o = 0; o < kC; ++o) {
      q[o] = L.l1b[o];
      for (int j = 0; j < kC; ++j) r[o][j] = L.l1[o * kC + j];
    }
    int p = nx.bpos + f0;
    if (p >= nx.N) p -= nx.N;
    for (int i = 0; i < kFast; ++i) {
      for (int o = 0; o < kC; ++o) {
        float t = r[o][0] * v[0][i];
        t += r[o][1] * v[1][i];
        t += r[o][2] * v[2][i];
        const float y = L.buf[o * L.cap + xs + i] + (t + q[o]);
        nx.buf[o * nx.cap + p + i] = y;
        if (p < kMaxBlock) nx.buf[o * nx.cap + nx.N + p + i] = y;
      }
    }
  }

  // One layer for a block that starts on a tile; the next layer's ring is filled directly (nullptr after the last layer).
  void layerFused(int l, int n, const int* st) {
    const Layer& L = layer_[l];
    Ring* nx = l + 1 < kLayers ? &layer_[l + 1] : nullptr;
    if (nx) advance(*nx, n);
    for (int f0 = 0; f0 + kFast <= n; f0 += kFast) {
      float a[kC][kFast], v[kC][kFast];
      convFused(L, st, f0, a);
      activateFused(L, f0, a, v);
      if (nx) residualFused(L, *nx, st[L.K - 1] + f0, f0, v);
    }
  }

  // The ring position bookkeeping of write(), for a ring the previous layer fills itself.
  static void advance(Ring& r, int n) {
    r.bpos = r.wpos;
    r.wpos = r.wpos + n >= r.N ? r.wpos + n - r.N : r.wpos + n;
  }

  // True when this block can take the fused path: whole tiles, and every ring starting on a tile.
  bool fusedBlock(int n) const {
    bool ok = n % kFast == 0;
    for (const Layer& l : layer_) ok = ok && l.wpos % kFast == 0;
    return ok;
  }
#else
  void layerFused(int, int, const int*) {}
  bool fusedBlock(int) const { return false; }
#endif

  void layerBlock(int l, int n) {
    const Layer& L = layer_[l];
    if (!fused_ || l == 0) write(layer_[l], &x_[0][0], n);  // on the fused path the layer before has filled this ring
    int st[15];
    for (int k = 0; k < L.K; ++k) st[k] = start(L, (L.K - 1 - k) * L.D);
    if (fused_) return layerFused(l, n, st);
    float act[kC][kTile];
    const bool residual = l + 1 < kLayers;
    int f0 = 0;
    for (; f0 + kTile <= n; f0 += kTile) {
      convTile<kTile>(L, st, f0, cond_, act, head_sum_);
      if (residual) residualTile<kTile>(L, f0, act, x_);
    }
    for (; f0 < n; ++f0) {
      convTile<1>(L, st, f0, cond_, act, head_sum_);
      if (residual) residualTile<1>(L, f0, act, x_);
    }
  }

#if NAMOPT_SLIDEH
  // The head conv for tiles of 4 frames, two taps per pass (5 columns per channel loaded once); the same sum as the plain loop.
  void headWindow(int n, float* out) const {
    const int s0 = start(head_, kHeadK - 1);
    for (int f0 = 0; f0 < n; f0 += 4) {
      float o[4] = {0.0f, 0.0f, 0.0f, 0.0f};
      for (int k = 0; k < kHeadK; k += 2) {
        float x[kC][5];
        for (int c = 0; c < kC; ++c)
          for (int q = 0; q < 5; ++q) x[c][q] = head_.buf[c * head_.cap + s0 + f0 + k + q];
        for (int g = 0; g < 2; ++g)
          for (int i = 0; i < 4; ++i) {
            float t = headW_[k + g] * x[0][g + i];
            t += headW_[kHeadK + k + g] * x[1][g + i];
            t += headW_[2 * kHeadK + k + g] * x[2][g + i];
            o[i] += t;
          }
      }
      for (int i = 0; i < 4; ++i) out[f0 + i] = o[i];
    }
  }
#endif

  // The head conv over the head sum's history into out[0..n), tap by tap.
  void headConv(int n, float* out) const {
    for (int f = 0; f < n; ++f) out[f] = 0.0f;
#if NAMOPT_SLIDEH
    if (n % 4 == 0 && start(head_, 0) == start(head_, kHeadK - 1) + kHeadK - 1) return headWindow(n, out);
#endif
    for (int k = 0; k < kHeadK; ++k) {
      const int st = start(head_, kHeadK - 1 - k);
      for (int f = 0; f < n; ++f) {
        float t = headW_[k] * head_.buf[st + f];
        for (int c = 1; c < kC; ++c) t += headW_[c * kHeadK + k] * head_.buf[c * head_.cap + st + f];
        out[f] += t;
      }
    }
  }

  void block(const float* in, float* out, int n) {
    for (int f = 0; f < n; ++f) cond_[f] = in[f];
    for (int c = 0; c < kC; ++c)
      for (int f = 0; f < n; ++f) x_[c][f] = rechannel_[c] * in[f], head_sum_[c][f] = 0.0f;
    fused_ = fusedBlock(n);
    for (int l = 0; l < kLayers; ++l) layerBlock(l, n);
    write(head_, &head_sum_[0][0], n);
    headConv(n, out);
    for (int f = 0; f < n; ++f) out[f] = (out[f] + headB_) * headScale_;
  }

  Layer layer_[kLayers];
  Ring head_;
  const float* rechannel_ = nullptr;
  const float* headW_ = nullptr;
  float headB_ = 0.0f, headScale_ = 1.0f;
  float cond_[kMaxBlock];
  float x_[kC][kMaxBlock];
  float head_sum_[kC][kMaxBlock];
  float ring_[detail::totalFloats()];
#if NAMOPT_FUSED
  float wfast_[kA2LiteWeightCount];  // the weights repacked for the fused kernel (the originals stay in flash)
#endif
  bool fused_ = false;  // this block takes the fused path
};

}  // namespace squatch::nam
