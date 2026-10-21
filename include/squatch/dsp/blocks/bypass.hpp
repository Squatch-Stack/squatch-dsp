// SPDX-License-Identifier: MIT
// Bypass: wraps a block so switching it on or off is a short crossfade between the dry input
// and the block's output, never a step. The rig used to switch stages immediately; this is the
// same switch behind the Block interface (set(Engaged, 0 or 1)), built on Crossfade.
//
// A block that is off does not run, so it starts from reset() when it is switched on: the wet
// signal comes up from silence while the dry fades out. That is wrong for a block whose output
// depends on the input before the switch (a cabinet's tail, a NAM model's receptive field): its
// wet starts from the wrong state, and a cold cabinet swells 2 to 4 dB over the louder side for
// the length of its response. Such a block is kept warm: `keepWarm` runs it on the live input
// while it is off, the output thrown away, so the switch fades between two signals that are what
// the always-on and always-off chains give. The price is the block's CPU while it is off. A switch that arrives during a fade is
// held and applied when the fade ends (the latest wins), so a flurry of switches never makes a
// step.
//
// The fade is Crossfade, and the warm-up before it follows the plan in namchain by Rik Hemsley (MIT),
// pinned to e208bb8, see THIRD_PARTY_NOTICES.md:
//   https://github.com/pedal-science/namchain/blob/e208bb869069b15a7331b452e03b7dc88726ffee/docs/transitions.md
// Copyright (c) 2026 Rik Hemsley.
//
// A block with latency (the Limiter's lookahead) would otherwise be mixed with a dry path that is
// early by that many samples: the output would jump in time at the switch, and the wet side
// starts with an empty delay line. So the dry path is delayed by latency() whatever the state
// (delay compensation: the stage adds the same delay on or off), and a block that is switched on
// first runs for latency() frames on the live input with its output thrown away, then fades in.
// latency() reports that constant delay. Blocks with no latency (Cabinet, Eq, Gate) pay nothing.
#pragma once
#include <squatch/dsp/block.hpp>
#include <squatch/dsp/crossfade.hpp>
#include <squatch/dsp/param.hpp>

namespace squatch::dsp {

class Bypass final : public Block {
 public:
  enum Param : ParamId { Engaged, kCount };
  static constexpr float kFadeMs = 5.0f;
  static Span<const ParamDesc> params();

  /// `inner` is not owned and must be prepared by this block's prepare() (not separately).
  /// `fadeMs` is the length of the handover switching on, `fadeOutMs` (default: the same) switching off. 5 ms is right for a block that
  /// filters. A gate moves the level by tens of dB: closing it on low-frequency material (mains hum) in 5 ms chops the hum and rings
  /// the stages after it, so it fades in over 40 ms; opening it again swells above both signals if the fade is long (measured on real
  /// material: 3.2 dB at 40 ms, 1.3 at 10), so it fades out over 10 ms.
  explicit Bypass(Block& inner, bool engaged = true, bool keepWarm = false, float fadeMs = kFadeMs, float fadeOutMs = 0.0f)
      : inner_(inner), keepWarm_(keepWarm), fadeMs_(fadeMs), fadeOutMs_(fadeOutMs > 0.0f ? fadeOutMs : fadeMs), settled_(engaged), fadeTo_(engaged), target_(engaged) {}

  void prepare(const Setup& s, Arena& mem) override;
  void process(const float* in, float* out, int n) override;
  void reset() override;
  void set(ParamId id, float value) override;
  float get(ParamId id) const override { return id == Engaged && target_ ? 1.0f : 0.0f; }
  int latency() const override { return lat_; }  // the wrapped block's, on or off

  bool engaged() const { return target_; }      // where the switch is going
  bool fading() const { return fade_.active(); }
  bool keepsWarm() const { return keepWarm_; }  // the wrapped block runs on the input while bypassed

 private:
  void begin(bool engage);
  const float* alignedDry(const float* in, int n);

  Block& inner_;
  Crossfade fade_;
  float* wet_ = nullptr;
  float* dry_ = nullptr;     // the input delayed by lat_ (null when lat_ is 0)
  float* delay_ = nullptr;   // ring of the last lat_ inputs
  int lat_ = 0;
  const bool keepWarm_;
  const float fadeMs_, fadeOutMs_;
  int delayPos_ = 0;
  int warmLeft_ = 0;         // frames the block still runs unheard before its fade in
  bool settled_;  // the state when no fade is running: block on (true) or bypassed
  bool fadeTo_;   // where the running fade is heading
  bool target_;   // the latest request
  bool ready_ = false;
  int fadeFrames_ = 0, fadeOutFrames_ = 0;  // switching on, switching off
};

}  // namespace squatch::dsp
