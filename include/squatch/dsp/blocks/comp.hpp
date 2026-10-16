// SPDX-License-Identifier: MIT
// Squatch Comp: a feed-forward compressor in the log domain, after Giannoulis, Massberg and Reiss,
// "Digital Dynamic Range Compressor Design - A Tutorial and Analysis" (JAES 2012), ported from
// squatch-rig's kernel (web/worklets/dsp/compressor.js).
//
// Per sample: peak level in dB -> soft-knee static curve -> the gain reduction through a smooth
// branching peak detector (attack when it rises, release when it falls) -> make-up gain, blended
// with the dry signal. Parameters: contracts/comp.params.json. Meter: the largest gain reduction
// in the last call, in dB.
#pragma once
#include <squatch/dsp/block.hpp>
#include <squatch/dsp/contracts/comp.hpp>
#include <squatch/dsp/param.hpp>

namespace squatch::dsp {

class Compressor final : public Block, public contracts::CompressorContract {
 public:
  static constexpr int kControlRate = 16;

  Compressor();
  void prepare(const Setup& s, Arena& mem) override;
  void process(const float* in, float* out, int n) override;
  void reset() override;
  void set(ParamId id, float value) override;
  float get(ParamId id) const override;
  Span<const ParamDesc> parameters() const override { return params(); }
  const BlockInfo& info() const override { return kInfo; }

  /// The static curve: output level (dB) for input level x (dB).
  static float curve(float x, float threshold, float ratio, float knee);

 private:
  bool smoothing() const;
  void design();

  Smoother p_[kParamCount];
  double sampleRate_ = 48000.0;
  float attack_ = 0.0f, release_ = 0.0f;  // detector coefficients
  float makeup_ = 1.0f;
  float gr_ = 0.0f;                       // smoothed gain reduction, dB (>= 0)
  float grPeak_ = 0.0f;
};

}  // namespace squatch::dsp
