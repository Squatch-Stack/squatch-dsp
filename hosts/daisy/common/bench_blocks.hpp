// SPDX-License-Identifier: MIT
// Two blocks that only exist for bring-up measurements, not effects:
//   Trim  a fixed gain in dB (the "hot" chain puts +24 dB in front of the drive, to raise
//         whatever noise reaches the input the way a high-gain pedal would: libDaisy #656);
//   Burn  busy-waits a fixed share of the block's cycle budget, to load the CPU on purpose
//         (#656 also reports ripple once the callback passes ~50% CPU).
#pragma once
#include <cmath>
#include <cstdint>
#include <squatch/dsp/block.hpp>
#include <squatch/dsp/param.hpp>
#include <squatch/dsp/span.hpp>

namespace squatch::board {

class Trim final : public dsp::Block {
 public:
  explicit Trim(float db) : gain_(dsp::dbToGain(db)) {}
  void prepare(const dsp::Setup&, dsp::Arena&) override {}
  void process(const float* in, float* out, int n) override {
    for (int i = 0; i < n; ++i) out[i] = in[i] * gain_;
  }
  void reset() override {}
  void set(dsp::ParamId, float) override {}
  float get(dsp::ParamId) const override { return NAN; }  // no parameters

 private:
  float gain_;
};

template <class Clock>
class Burn final : public dsp::Block {
 public:
  /// Setup only: how many cycles each process() call spends, on top of copying.
  void setCycles(std::uint32_t cycles) { cycles_ = cycles; }
  void prepare(const dsp::Setup&, dsp::Arena&) override {}
  void process(const float* in, float* out, int n) override {
    const std::uint32_t start = Clock::now();
    dsp::copy(in, out, n);
    while (Clock::now() - start < cycles_) {
    }
  }
  void reset() override {}
  void set(dsp::ParamId, float) override {}
  float get(dsp::ParamId) const override { return NAN; }  // no parameters

 private:
  std::uint32_t cycles_ = 0;
};

}  // namespace squatch::board
