// SPDX-License-Identifier: MIT
// The sine of squatch-tone, for any audio backend. The phase is a double accumulator, so the
// frequency is exact to the interface's own sample clock (which its input shares): the tuner's reading
// can be compared with HZ directly. It fades in and out over 10 ms. fill() is real-time safe.
#pragma once
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>

namespace squatch::host {

struct ToneGen {
  double phase = 0.0, step = 0.0;     // turns, turns per sample
  float amp = 0.1f, gain = 0.0f, fadeStep = 0.0f;
  std::atomic<float> target{0.0f};    // 0 until connected, so the start time is the real one

  void configure(double hz, double db, double rate) {
    step = hz / rate;
    amp = static_cast<float>(std::pow(10.0, db / 20.0));
    fadeStep = static_cast<float>(1.0 / (0.010 * rate));
  }

  /// Writes `n` samples to o[0], o[stride], ... and returns the gain target it saw.
  float fill(float* o, uint32_t n, uint32_t stride = 1) {
    const float tgt = target.load(std::memory_order_relaxed);
    for (uint32_t i = 0; i < n; ++i) {
      gain = gain < tgt ? std::min(gain + fadeStep, tgt) : std::max(gain - fadeStep, tgt);
      o[static_cast<size_t>(i) * stride] = amp * gain * static_cast<float>(std::sin(6.283185307179586 * phase));
      phase += step;
      phase -= std::floor(phase);
    }
    return tgt;
  }
};

}  // namespace squatch::host
