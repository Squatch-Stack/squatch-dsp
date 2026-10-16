// SPDX-License-Identifier: MIT
// Squatch Hall: a feedback delay network reverb, ported from squatch-rig's kernel
// (web/worklets/dsp/reverb.js).
//
// Eight delay lines (29.7 .. 73.3 ms, scaled by Size) with a Householder feedback matrix; each
// line's gain gives the Decay time (RT60) and a one-pole damping low-pass sits in every loop,
// unity at DC, so Decay is the low-frequency RT60 and Damp shortens the highs. The input goes
// through a pre-delay and four series allpass diffusers first. The wet signal is two taps of the
// same tank, the lines summed with signs +-+-+-+- (left) and ++--++-- (right), blended with the dry
// signal by Mix; Width scales the side (left minus right) part of the wet signal as in the rig.
//
// Layouts: Mono (1 -> 1, the left tap, so at Width 1 the browser's left channel to float rounding),
// MonoToStereo (the mono input, left and right taps, the dry signal copied to both) and Stereo (the
// two inputs are summed to mono for the tank, as the rig does, and each side keeps its own dry
// signal). Stereo output needs no interface change: the v2 Layout and processChannels() carry it.
//
// The delay memory comes from the Arena in prepare(), sized for the largest Size at the rate.
// Parameters: contracts/hall.params.json.
#pragma once
#include <squatch/dsp/block.hpp>
#include <squatch/dsp/contracts/hall.hpp>
#include <squatch/dsp/param.hpp>

namespace squatch::dsp {

class Hall final : public Block, public contracts::HallContract {
 public:
  static constexpr int kLines = 8, kAllpasses = 4;
  static constexpr int kControlRate = 16;

  Hall();
  void prepare(const Setup& s, Arena& mem) override;
  void process(const float* in, float* out, int n) override;
  void processChannels(const AudioIO& io, int n) override;
  bool supports(Layout) const override { return true; }
  bool stereoOutput() const override { return true; }
  void reset() override;
  void set(ParamId id, float value) override;
  float get(ParamId id) const override;
  int tail() const override { return static_cast<int>(p_[Decay].value() * sampleRate_); }
  Span<const ParamDesc> parameters() const override { return params(); }
  const BlockInfo& info() const override { return kInfo; }

  bool ready() const { return ready_; }

 private:
  struct Line {
    float* b = nullptr;
    int cap = 0, w = 0;
    float read(int d) const {
      int r = w - d;
      if (r < 0) r += cap;
      return b[r];
    }
    void write(float x) {
      b[w] = x;
      if (++w == cap) w = 0;
    }
  };

  bool smoothing() const;
  void design();
  float diffuse(float dryIn);  // pre-delay and allpasses
  /// One step of the feedback delay network with `s` fed in; returns the left and right taps.
  struct Taps {
    float l, r;
  };
  Taps tank(float s);
  /// The block's one render loop. inR / outR may be null (mono in / mono out); in may equal out.
  void render(const float* inL, const float* inR, float* outL, float* outR, int n);

  Smoother p_[kParamCount];
  double sampleRate_ = 48000.0;
  bool ready_ = false;
  Line lines_[kLines], aps_[kAllpasses], pre_;
  int len_[kLines] = {}, apLen_[kAllpasses] = {}, preLen_ = 0;
  float gain_[kLines] = {}, damp_[kLines] = {}, dampG_ = 0.0f;
};

}  // namespace squatch::dsp
