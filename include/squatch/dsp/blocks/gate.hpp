// SPDX-License-Identifier: MIT
// Noise gate, the browser rig's NoiseGate (squatch-rig web/worklets/dsp/gate.js) ported to C++, so a
// preset sounds the same in both places (parity fixes 2, 2026-10-11; tests/test_parity_gate.cpp).
//
// Detector: the peak over the channels, followed with an instant rise and a 20 ms one-pole fall.
// The gate opens when that envelope reaches the threshold and stays open while it is at or above
// threshold - hysteresis (hysteresis is a positive magnitude). Once the envelope has stayed below
// that for the hold time the gate closes. The gain is a one-pole move towards unity (open) or the
// range floor (closed): Attack and Release are TIME CONSTANTS (63 % of the way), not the time to
// cross the range as an earlier version of this block had them.
//
// All state is double, as in the browser, so the two agree to the float rounding of the output.
// Ranges and defaults are the browser's; contracts/gate.params.json holds them.
//
// Stereo is linked: one detector on the louder side, one gain for both, so the image never shifts.
// Meters: the gain reduction in dB and whether the gate is open. Non-finite input counts as silence
// for the detector so one bad sample cannot lock the envelope.
#pragma once
#include <squatch/dsp/block.hpp>
#include <squatch/dsp/contracts/gate.hpp>
#include <squatch/dsp/param.hpp>

namespace squatch::dsp {

class Gate final : public Block, public contracts::GateContract {
 public:
  Gate();
  void prepare(const Setup& s, Arena& mem) override;
  void process(const float* in, float* out, int n) override;
  void processChannels(const AudioIO& io, int n) override;
  void reset() override;
  void set(ParamId id, float value) override;
  float get(ParamId id) const override;
  bool supports(Layout) const override { return true; }
  Span<const ParamDesc> parameters() const override { return params(); }
  const BlockInfo& info() const override { return kInfo; }

  bool open() const { return open_; }
  float gain() const { return static_cast<float>(gain_); }

 private:
  void detect(double level);
  double step();
  void update();

  float value_[kParamCount];
  double sampleRate_ = 48000.0;
  double openLevel_ = 0.0, closeLevel_ = 0.0, floor_ = 0.0;
  double envFall_ = 0.0, attack_ = 0.0, release_ = 0.0;  // one-pole coefficients (the part kept)
  double env_ = 0.0;
  int holdSamples_ = 0;
  int holdLeft_ = 0;
  double gain_ = 0.0;
  bool open_ = false;
};

}  // namespace squatch::dsp
