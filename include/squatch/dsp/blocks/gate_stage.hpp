// SPDX-License-Identifier: MIT
// GateStage: the noise gate as a stage that is switched on and off while audio runs, with the handover that
// does not chop mains hum. A Gate and a Bypass (kept warm) in one block, so a host gets the right fade by
// using this block and not by remembering the numbers.
//
// Why the handover is this long. A gate moves the level by tens of dB. Closing it on low-frequency material
// (mains hum) in Bypass's default 5 ms chops the hum, and a high-gain amp after it turns the chop into an
// audible swell. So switching on fades over 40 ms. Opening it again swells above both signals if the fade
// is long (measured on real material: 3.2 dB at 40 ms, 1.3 at 10) as the hiss returns, so switching off
// fades over 10 ms. The gate is kept warm (it runs on the live input while off, its output thrown away) so
// its envelope is the one the always-on gate has at the switch: no cold start.
//
// Parameters: the Gate's own ids and descriptors (contracts::GateContract). The Bypass id is the switch, as
// on every block: 1 is bypassed (the gate off), 0 is the gate on; Reduction and Open are the meters. A host
// that queues changes (ParamQueue) sends set(Bypass, ...) like any other parameter. A switch that arrives
// during a handover is held and applied when it ends (Bypass). Non-finite values are ignored.
// Mono, no latency. process() allocates nothing and takes no lock.
#pragma once
#include <squatch/dsp/block.hpp>
#include <squatch/dsp/blocks/bypass.hpp>
#include <squatch/dsp/blocks/gate.hpp>
#include <squatch/dsp/param.hpp>

namespace squatch::dsp {

class GateStage final : public Block {
 public:
  using Param = contracts::GateContract::Param;
  static constexpr float kFadeInMs = 40.0f;   // switching on: closing on hum must not chop it
  static constexpr float kFadeOutMs = 10.0f;  // switching off: longer swells above both signals
  static Span<const ParamDesc> params() { return contracts::GateContract::params(); }

  /// `engaged`: the gate starts on. The fade lengths exist for the tests that show what the defaults prevent.
  explicit GateStage(bool engaged = true, float fadeInMs = kFadeInMs, float fadeOutMs = kFadeOutMs)
      : bypass_(gate_, engaged, true, fadeInMs, fadeOutMs) {}

  void prepare(const Setup& s, Arena& mem) override { bypass_.prepare(s, mem); }
  void process(const float* in, float* out, int n) override { bypass_.process(in, out, n); }
  void reset() override { bypass_.reset(); }
  void set(ParamId id, float value) override;
  float get(ParamId id) const override;
  Span<const ParamDesc> parameters() const override { return params(); }
  const BlockInfo& info() const override { return contracts::GateContract::kInfo; }

  bool engaged() const { return bypass_.engaged(); }
  bool fading() const { return bypass_.fading(); }
  /// For a host that drives the switch through its own Bypass slot (the JACK rig): the gate and the wrapper prepared through it.
  Gate& gate() { return gate_; }
  Bypass& bypass() { return bypass_; }

 private:
  Gate gate_;  // before bypass_, which refers to it
  Bypass bypass_;
};

}  // namespace squatch::dsp
