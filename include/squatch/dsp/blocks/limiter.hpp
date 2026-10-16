// SPDX-License-Identifier: MIT
// Lookahead true-peak limiter: ear safety at the end of every chain. The Ceiling is a true-peak
// ceiling: neither the output samples nor the waveform between them go over it.
//
// Detector, per stretch between two samples: the sample peak s and the peak i of seven points
// between them (8x polyphase Kaiser-sinc interpolation). A short interpolator reads peaks built
// from content near fs/2 low, so the estimate counts the inter-sample excess twice,
// s + 2 (i - s), and adds a fixed 0.05 dB. What each part closes, measured at 16x with a 96-tap
// meter on the test set at 44.1, 48 and 96 kHz (dB over the ceiling without it):
//   8x detector alone:     tones +0.04, squares +0.2, onsets +0.5, white noise to fs/2 +1.0
//   + excess counted twice: all closed except band-limited noise at 44.1 kHz (+0.013) and the
//                          ringing of a hard onset (+0.036)
//   + 0.05 dB:             all closed, worst -0.014
// The price: nothing for material below ~5 kHz, but a pure high tone is held below the ceiling
// by its own inter-sample excess (8 kHz: up to 0.5 dB; fs/4 at 45 degrees: 2.3 dB).
//
// Gain: the smallest gain needed over the lookahead window (a sliding minimum), an exponential
// release, then a moving average as long as the lookahead. With window lengths D+2 and D+1 for a
// delay of D, the averaged gain is never above what the sample being output needs, so the output
// reaches the ceiling without crossing it and without a discontinuity. A hard clamp at the
// ceiling follows as the last guarantee on the samples: it only engages on float rounding, after a
// ceiling cut (the lookahead holds audio gained for the old ceiling), or on non-audio input (NaN
// becomes silence, anything past +80 dBFS is treated as +80 dBFS).
//
// Controls (contracts/limiter.params.json, settled 2026-09-25 with the browser rig): Drive, a
// smoothed gain before the limiter (0..18 dB); Ceiling; Release; and Output, a smoothed trim
// after the final clamp that only attenuates (-24..0 dB), so the output never exceeds the ceiling.
// At their defaults (0 dB) both are exact multiplications by one: the sound is unchanged.
//
// Browser parity (parity fixes 2, 2026-10-11): the browser rig's Limiter (web/worklets/dsp/limiter.js) is the
// same gain stage (running minimum, release-only one-pole, moving average, delay) with a SAMPLE-peak
// detector, so it overshoots a ceiling on content near fs/4 (2.1 dB on a 12 kHz tone at -3 dB). This block
// keeps the true-peak detector, which is the correct behaviour; the overshoot is the browser's defect, to go
// when it moves to this engine. `useSamplePeakForTest(true)`, called before prepare(), swaps in the
// browser's detector, lookahead (2 ms) and windows so tests/test_parity_limiter.cpp can hold the gain stage
// to the browser's renders (ESR < 1e-6). It is not for hosts: nothing in the registry or the plugins sets it.
//
// Stereo is linked: each side has its own true-peak detector and delay line, and the louder
// side's need sets one gain for both. Meter: the most gain reduction (dB) in the last process().
#pragma once
#include <cstdint>
#include <squatch/dsp/block.hpp>
#include <squatch/dsp/contracts/limiter.hpp>
#include <squatch/dsp/param.hpp>

namespace squatch::dsp {

class Limiter final : public Block, public contracts::LimiterContract {
 public:
  static constexpr int kMaxChannels = 2;

  static constexpr float kLookaheadMs = 1.5f;
  static constexpr int kTaps = 32;    // per phase of the true-peak interpolator (Kaiser, beta 8)
  static constexpr int kPhases = 7;   // 1/8 to 7/8 of the way between samples
  static constexpr float kExcessWeight = 2.0f;  // inter-sample excess over the sample peak, counted twice
  static constexpr float kMarginDb = 0.05f;
  static constexpr float kMaxInput = 1e4f;
  static constexpr float kBrowserLookaheadMs = 2.0f;  // the browser kernel's window, used by the test-only mode

  Limiter();
  /// Test only (see above): the browser's sample-peak detector and 2 ms window. Takes effect at prepare().
  void useSamplePeakForTest(bool on) { samplePeak_ = on; }
  void prepare(const Setup& s, Arena& mem) override;
  void process(const float* in, float* out, int n) override;
  void reset() override;
  void set(ParamId id, float value) override;
  float get(ParamId id) const override;
  int latency() const override { return latency_; }
  void processChannels(const AudioIO& io, int n) override;
  bool supports(Layout) const override { return true; }
  Span<const ParamDesc> parameters() const override { return params(); }
  const BlockInfo& info() const override { return kInfo; }

  float gain() const { return gain_; }  // the gain applied to the last output sample
  bool ready() const { return ready_; }

 private:
  void tick(const float* x, float* y, int channels);
  float detect(const float* x, int channels);
  float truePeak(const float* window) const;
  float hold(float need);
  float smooth(float held);
  void sizeWindows(double sampleRate);
  void designInterpolator();
  void update();

  float value_[kParamCount];
  Smoother drive_, output_;
  int channels_ = 1;
  double sampleRate_ = 48000.0;
  float ceiling_ = 1.0f;
  float margin_ = 1.0f;
  float releaseCoef_ = 1.0f;
  float fir_[kTaps][kPhases + 1] = {};  // tap-major, padded to 8 lanes, so the phases vectorise

  int lookahead_ = 0, latency_ = 0;
  float* delay_[kMaxChannels] = {};  // latency_ samples each
  int delayPos_ = 0;
  float* hist_[kMaxChannels] = {};  // the last kTaps inputs, stored twice so a window never wraps
  int histPos_ = 0;
  float* holdVal_ = nullptr;  // monotonic deque for the sliding minimum
  std::uint32_t* holdAt_ = nullptr;
  int holdLen_ = 0, holdHead_ = 0, holdCount_ = 0;
  float* box_ = nullptr;  // moving average
  int boxLen_ = 0, boxPos_ = 0;
  float boxSum_ = 0.0f;

  std::uint32_t now_ = 0;
  float released_ = 1.0f;
  float gain_ = 1.0f;
  float leastGain_ = 1.0f;  // over the last process(), for the meter
  bool ready_ = false;
  bool samplePeak_ = false;  // test only: the browser's detector
};

}  // namespace squatch::dsp
