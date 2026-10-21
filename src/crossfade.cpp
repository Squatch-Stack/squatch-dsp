// SPDX-License-Identifier: MIT
// Adapted from rt::Crossfade in pedal-science/namchain by Rik Hemsley (MIT), pinned to e208bb8:
//   https://github.com/pedal-science/namchain/blob/e208bb869069b15a7331b452e03b7dc88726ffee/src/rt/crossfade.cpp
// Copyright (c) 2026 Rik Hemsley (MIT; see THIRD_PARTY_NOTICES.md).
#include <squatch/dsp/crossfade.hpp>

#include <algorithm>
#include <cmath>
#include <squatch/dsp/denormal.hpp>

namespace squatch::dsp {
namespace {
constexpr float kHalfPi = 1.57079632679489661923f;
}

void Crossfade::prepare(int maxFrames, Arena& mem) {
  from_ = to_ = nullptr;
  length_ = position_ = remaining_ = 0;
  rho_ = 0.0f;
  measure_ = false;
  ab_ = aa_ = bb_ = 0.0;
  maxFrames_ = maxFrames > 0 ? maxFrames : 1;
  const auto size = static_cast<std::size_t>(maxFrames_) + 1;
  float* const from = mem.alloc<float>(size);
  float* const to = mem.alloc<float>(size);
  if (!from || !to) return;
  // Quarter turn: the old signal at full weight and the new at zero at t = 0, the reverse at
  // t = 1, from^2 + to^2 == 1 between. mix() normalises for the correlation, so one table
  // serves every rho.
  for (int i = 0; i <= maxFrames_; ++i) {
    const float theta = kHalfPi * (static_cast<float>(i) / static_cast<float>(maxFrames_));
    from[i] = std::cos(theta);
    to[i] = std::sin(theta);
  }
  from_ = from;
  to_ = to;
}

void Crossfade::start(int frames, double rho) {
  if (!ready()) return;
  length_ = frames < 1 ? 1 : (frames > maxFrames_ ? maxFrames_ : frames);
  position_ = 0;
  remaining_ = length_;
  measure_ = rho < 0.0;
  rho_ = measure_ ? 0.0f : static_cast<float>(std::min(1.0, std::max(0.0, rho)));  // NaN -> 0
  ab_ = aa_ = bb_ = 0.0;
}

// Running correlation of the two signals since the fade began, updated one sample at a time so
// the result cannot depend on how the host cut the stream into blocks (the first-block estimate
// the original takes gives 1-sample and 64-sample hosts different weights). Clamped to [0, 1]:
// over near-silence the estimate is readily negative, and a negative rho would divide by less
// than one and amplify the fade. The first, noisy estimates cost nothing audible: the weights
// from * to that rho scales are near zero at the start of a fade (under 1% in the first frame of
// 240), and identical signals read rho = 1 from the first sample, so they stay exact.
void Crossfade::measure(float a, float b) {
  ab_ += static_cast<double>(a) * b;
  aa_ += static_cast<double>(a) * a;
  bb_ += static_cast<double>(b) * b;
  const double den = std::sqrt(aa_ * bb_);
  const double raw = den > 0.0 ? std::min(1.0, std::max(0.0, ab_ / den)) : 0.0;
  rho_ = static_cast<float>(raw);
}

void Crossfade::mix(const float* a, const float* b, float* out, int n) {
  int i = 0;
  while (i < n && remaining_ > 0) {
    // The last sample of the fade must land on the table's end: `length` samples are
    // `length - 1` steps. Reading position * max / length instead leaves cos(pi/2 * (L-1)/L) of
    // the old signal at the handoff (0.008 for 240 frames), which is a step. One frame is the
    // endpoint by definition.
    int index = maxFrames_;
    if (length_ > 1) index = static_cast<int>((static_cast<long long>(position_) * maxFrames_) / (length_ - 1));
    float g1 = from_[index], g2 = to_[index];
    if (measure_) measure(a[i], b[i]);
    if (rho_ > 0.0f) {
      const float norm = 1.0f / std::sqrt(1.0f + 2.0f * rho_ * g1 * g2);
      g1 *= norm;
      g2 *= norm;
    }
    out[i] = flushDenormal(a[i] * g1 + b[i] * g2);
    ++i;
    ++position_;
    --remaining_;
  }
  for (; i < n; ++i) out[i] = b[i];
}

}  // namespace squatch::dsp
