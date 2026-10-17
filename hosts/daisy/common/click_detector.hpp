// SPDX-License-Identifier: MIT
// Finds a click in a recording of the input, for the round-trip latency firmware.
//
// The firmware writes a one-sample impulse to the output and records the input from kPre
// samples before it. What comes back through a loopback cable is the impulse response of the
// DAC and ADC filters, delayed by the buffering. Its peak is the latency for a linear-phase
// filter (the group delay) and close to it for a low-latency one; the onset (first sample over
// a tenth of the peak) bounds it from below. The peak is refined to 1/64 sample by evaluating
// the band-limited (windowed-sinc) interpolation of the recording between the samples either
// side of it: the converters' filters make the returned click band-limited, and a parabola
// through three samples of a sinc is biased by up to ~0.2 sample. Pure C++, host-tested.
#pragma once
#include <cmath>

namespace squatch::board {

struct ClickResult {
  bool found = false;    // peak over 10x the noise before the click and over -60 dBFS
  float lag = 0.0f;      // samples from the click leaving to its peak arriving (sub-sample)
  int onset = 0;         // samples to the first |x| over 10% of the peak
  float peak = 0.0f;     // signed value at the peak (negative: the loop inverts)
  float noiseRms = 0.0f; // before the click could have arrived
};

inline float rms(const float* x, int n) {
  double sum = 0.0;
  for (int i = 0; i < n; ++i) sum += double(x[i]) * x[i];
  return n > 0 ? static_cast<float>(std::sqrt(sum / n)) : 0.0f;
}

inline int argMaxAbs(const float* x, int from, int to) {
  int best = from;
  for (int i = from; i < to; ++i)
    if (std::fabs(x[i]) > std::fabs(x[best])) best = i;
  return best;
}

/// x between samples: sum of x[k] sinc(t - k), Hann-windowed over +-16 samples.
inline float interpolate(const float* x, int n, float t) {
  constexpr int kHalf = 16;
  constexpr float kPi = 3.14159265f;
  const int c = static_cast<int>(std::floor(t));
  float sum = 0.0f;
  for (int k = c - kHalf + 1; k <= c + kHalf; ++k) {
    if (k < 0 || k >= n) continue;
    const float d = t - static_cast<float>(k);
    const float sinc = d == 0.0f ? 1.0f : std::sin(kPi * d) / (kPi * d);
    sum += x[k] * sinc * (0.5f + 0.5f * std::cos(kPi * d / kHalf));
  }
  return sum;
}

/// The position of the largest |x(t)| within one sample of index `at`, to 1/64 sample.
inline float refinePeak(const float* x, int n, int at) {
  float best = static_cast<float>(at), bestMag = std::fabs(x[at]);
  for (int step = -64; step <= 64; ++step) {
    const float t = static_cast<float>(at) + static_cast<float>(step) / 64.0f;
    const float mag = std::fabs(interpolate(x, n, t));
    if (mag > bestMag) best = t, bestMag = mag;
  }
  return best;
}

/// x holds `pre` samples recorded before the click was sent, then n - pre after it.
inline ClickResult analyzeClick(const float* x, int n, int pre) {
  ClickResult r;
  if (pre < 1 || n <= pre + 2) return r;
  r.noiseRms = rms(x, pre);
  const int at = argMaxAbs(x, pre, n);
  r.peak = x[at];
  const float mag = std::fabs(r.peak);
  r.found = mag > 1e-3f && mag > 10.0f * r.noiseRms;
  if (!r.found) return r;
  r.lag = refinePeak(x, n, at) - static_cast<float>(pre);
  r.onset = at - pre;
  for (int i = pre; i <= at; ++i) {
    if (std::fabs(x[i]) > 0.1f * mag) {
      r.onset = i - pre;
      break;
    }
  }
  return r;
}

/// Median of up to 64 values (copied, insertion-sorted: the report loop only).
inline float median(const float* v, int n) {
  float s[64];
  if (n > 64) n = 64;
  if (n <= 0) return 0.0f;
  for (int i = 0; i < n; ++i) {
    int j = i;
    for (; j > 0 && s[j - 1] > v[i]; --j) s[j] = s[j - 1];
    s[j] = v[i];
  }
  return n % 2 ? s[n / 2] : 0.5f * (s[n / 2 - 1] + s[n / 2]);
}

}  // namespace squatch::board
