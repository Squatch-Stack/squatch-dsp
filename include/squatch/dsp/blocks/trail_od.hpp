// SPDX-License-Identifier: MIT
// Squatch Trail OD: the browser rig's Overdrive (squatch-rig web/worklets/dsp/overdrive.js) ported to C++, so
// a preset sounds the same in both places (parity fixes 2, 2026-10-11; tests/test_parity_trail_od.cpp). It
// replaces an earlier circuit-model port, which was a different model under the
// same name (no oversampling: it aliased 18 to 44 dB louder than the browser).
//
// Signal path, per sample (Voice 720 Hz and Asym 0.35 are the browser's defaults; they are not controls):
//   4x polyphase upsampler (129-tap Kaiser FIR, beta 8.6, 21 kHz edge at 192 kHz)
//   -> u + 0.5 * (tanh(2 v + bias) - tanh(bias))   at 4x, with v = low-pass(high-pass(u) * (1 + 110 Drive^2)):
//                                    the op-amp stage, a clean unity path that is never clipped plus a boosted,
//                                    band-limited, asymmetric soft-clipped branch (high-pass at the Voice corner,
//                                    low-pass at the feedback corner 1 / (2 pi Rf 51 pF), Rf = 51k + 500k Drive^2)
//   -> the same FIR as the 4x decimator
//   -> DC block (8 Hz) -> Tone low-pass (500 Hz .. 11 kHz, 500 * 2^(4.5 Tone)) -> Level
//
// The filter pair delays the signal by 32 samples at the base rate (latency()), as the browser's does and as its
// graph compensates. All state is double and the arithmetic is the browser's, so the two agree to the float
// rounding of the output. The 4x filter's edge is fixed in Hz of the 192 kHz rate the browser runs it at.
//
// Parameters come from contracts/trail-od.params.json (the browser's Drive, Tone, Level). Meters: the output
// peak of the last call in dBFS, and whether the boosted branch drove the clipper past its knee in that call.
#pragma once
#include <squatch/dsp/block.hpp>
#include <squatch/dsp/contracts/trail_od.hpp>
#include <squatch/dsp/param.hpp>

namespace squatch::dsp {

class TrailOD final : public Block, public contracts::TrailODContract {
 public:
  static constexpr int kControlRate = 16;  // samples between coefficient updates while smoothing
  static constexpr int kOversample = 4;
  static constexpr int kTaps = 129;  // the 4x filter
  static constexpr int kPerPhase = (kTaps + kOversample - 1) / kOversample;
  static constexpr int kLatency = (kTaps - 1) / kOversample;  // round trip, at the base rate

  TrailOD();
  void prepare(const Setup& s, Arena& mem) override;
  void process(const float* in, float* out, int n) override;
  void reset() override;
  void set(ParamId id, float value) override;
  float get(ParamId id) const override;
  int latency() const override { return kLatency; }
  Span<const ParamDesc> parameters() const override { return params(); }
  const BlockInfo& info() const override { return kInfo; }

 private:
  struct OnePole {  // topology-preserving one-pole: low-pass and high-pass
    double g = 0.0, s = 0.0;
    void cutoff(double fc, double rate);
    double lp(double x);
    double hp(double x) { return x - lp(x); }
  };

  bool smoothing() const;
  void advance(int n);
  void design();
  double nonlinear(double u);
  double sample(float x);
  void up(double x, double* out4);
  double down(const double* in4);

  Smoother p_[kParamCount];
  double sampleRate_ = 48000.0;
  OnePole hp_, lp_, tone_;
  double gain_ = 1.0, bias_ = 0.0, biasTanh_ = 0.0, level_ = 1.0;
  double dcR_ = 1.0, dcX1_ = 0.0, dcY1_ = 0.0;
  double h_[kTaps] = {};                       // the Kaiser low-pass
  double phase_[kOversample][kPerPhase] = {};  // L * h, split by phase, for the interpolator
  double xHist_[2 * kPerPhase] = {};
  double uHist_[2 * kTaps] = {};
  int xi_ = 0, ui_ = 0;
  float peak_ = 0.0f;
  bool clipped_ = false;
};

}  // namespace squatch::dsp
