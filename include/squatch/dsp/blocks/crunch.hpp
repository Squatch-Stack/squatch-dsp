// SPDX-License-Identifier: MIT
// Squatch Crunch: a high-gain amp preamp, for the cabinet to follow.
//
// Per input sample:
//   tight high-pass (1st order, 40 Hz .. 400 Hz by Tight) -> input gain
//   -> 4x oversampled: stage 1, an asymmetric tanh (the bias makes even harmonics) with a coupling
//      high-pass and a 7 kHz low-pass after it; stage 2, the same with the other bias and 5.5 kHz;
//      stage 3, a symmetric tanh
//   -> back to the base rate -> DC blocker -> Level (dB)
// Each tanh is shifted so that silence stays silence. Gain moves stage 1's drive over 0..36 dB and
// stage 2's over 6..18 dB: clean-ish crunch at 0, saturated high gain at 10. The oversampling
// filters are 8th-order Butterworth low-passes (four state-variable sections, Simper's
// trapezoidal form) at 0.4 of the base rate, so the harmonics the clippers make above the base
// Nyquist are removed before they could fold back into the audio band.
//
// Parameters: contracts/crunch.params.json. Meters: output peak (dBFS) and whether stage 2 was
// driven past its knee in the last call.
#pragma once
#include <squatch/dsp/block.hpp>
#include <squatch/dsp/contracts/crunch.hpp>
#include <squatch/dsp/param.hpp>

namespace squatch::dsp {

class Crunch final : public Block, public contracts::CrunchContract {
 public:
  static constexpr int kOversample = 4;
  static constexpr int kControlRate = 16;

  Crunch();
  void prepare(const Setup& s, Arena& mem) override;
  void process(const float* in, float* out, int n) override;
  void reset() override;
  void set(ParamId id, float value) override;
  float get(ParamId id) const override;
  Span<const ParamDesc> parameters() const override { return params(); }
  const BlockInfo& info() const override { return kInfo; }

 private:
  struct Svf {  // low-pass, Simper's trapezoidal state-variable filter
    float a1 = 1.0f, a2 = 0.0f, a3 = 0.0f, ic1 = 0.0f, ic2 = 0.0f;
    void design(double fc, double fs, double q);
    float run(float x) {
      const float v3 = x - ic2, v1 = a1 * ic1 + a2 * v3, v2 = ic2 + a2 * ic1 + a3 * v3;
      ic1 = 2.0f * v1 - ic1;
      ic2 = 2.0f * v2 - ic2;
      return v2;
    }
  };
  struct Pole {  // one-pole, trapezoidal: lp() and hp()
    float g = 0.0f, s = 0.0f;
    void design(double fc, double fs);
    float lp(float x) {
      const float v = (x - s) * g, y = v + s;
      s = y + v;
      return y;
    }
    float hp(float x) { return x - lp(x); }
  };

  bool smoothing() const;
  void design();
  float sample(float x);

  Smoother p_[kParamCount];
  double sampleRate_ = 48000.0;
  Svf up_[4], down_[4];
  Pole tight_, dc_, couple1_, couple2_, lp1_, lp2_;
  float g1_ = 1.0f, g2_ = 2.0f, level_ = 0.25f;
  float peak_ = 0.0f;
  bool clipped_ = false;
};

}  // namespace squatch::dsp
