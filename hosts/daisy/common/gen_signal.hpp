// SPDX-License-Identifier: MIT
// A built-in hot test signal for the on-board limiter check (seed3-chain "gen" variants), so the check needs no
// source wired to the input. 12 s that repeat: each 2 s segment is a different way to break a limiter.
//   0  low note   82.41 Hz (open E) sine
//   1  bursts     1 kHz sine, 50 ms on, 150 ms off
//   2  inter-sample  a sine at fs/4 at 45 degrees: every sample is 0.707 of the true peak, which is 1
//   3  noise      white, uniform
//   4  square     100 samples per period
//   5  silence    (the gate and the release recover)
// Every segment peaks at `amp`. Pure C++, host-tested; nothing here allocates.
#pragma once
#include <cmath>
#include <cstdint>

namespace squatch::board {

class HotSignal {
 public:
  static constexpr int kSegments = 6;

  HotSignal(float amp, int rate) : amp_(amp), rate_(rate), seg_len_(2 * rate) {}

  float next() {
    float v = 0.0f;
    switch (seg_) {
      case 0: v = tone(82.41f); break;
      case 1: v = (pos_ % (rate_ / 5)) < rate_ / 20 ? tone(1000.0f) : 0.0f; break;
      case 2: {  // the samples are +-0.7071 of a waveform whose true peak is 1
        static constexpr float kS[4] = {0.70710678f, 0.70710678f, -0.70710678f, -0.70710678f};
        v = kS[pos_ & 3];
        break;
      }
      case 3:
        noise_ = noise_ * 1664525u + 1013904223u;
        v = static_cast<float>(static_cast<std::int32_t>(noise_)) * (1.0f / 2147483648.0f);
        break;
      case 4: v = (pos_ % 100) < 50 ? 1.0f : -1.0f; break;
      default: v = 0.0f; break;
    }
    if (++pos_ >= seg_len_) pos_ = 0, seg_ = seg_ + 1 == kSegments ? 0 : seg_ + 1;
    return v * amp_;
  }

  int segment() const { return seg_; }

 private:
  static constexpr float kPi = 3.14159265358979f;

  // A sine of `hz` from a wrapped phase, so the argument stays small.
  float tone(float hz) {
    phase_ += 2.0f * kPi * hz / static_cast<float>(rate_);
    if (phase_ >= 2.0f * kPi) phase_ -= 2.0f * kPi;
    return std::sin(phase_);
  }
  float phase_ = 0.0f;
  float amp_;
  int rate_, seg_len_;
  int seg_ = 0, pos_ = 0;
  std::uint32_t noise_ = 12345u;
};

}  // namespace squatch::board
