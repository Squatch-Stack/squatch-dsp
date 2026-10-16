// SPDX-License-Identifier: MIT
// Double-precision references the blocks are checked against. They are written for clarity, not
// speed: brute-force windows instead of deques and running sums, and the RBJ cookbook formulas
// transcribed literally, not refactored as in src/biquad.cpp.
#pragma once
#include <algorithm>
#include <cmath>
#include <complex>
#include <vector>

#include "signals.hpp"

namespace ref {

// ---- RBJ Audio EQ Cookbook, verbatim ----
struct Biquad {
  double b0, b1, b2, a0, a1, a2;
};

inline Biquad rbj(int shape, double fs, double f0, double gainDb, double q) {  // 0 low, 1 peak, 2 high
  const double A = std::pow(10.0, gainDb / 40.0), w0 = 2.0 * sig::kPi * f0 / fs;
  const double cw = std::cos(w0), alpha = std::sin(w0) / (2.0 * q), sA = 2.0 * std::sqrt(A) * alpha;
  if (shape == 1) return {1 + alpha * A, -2 * cw, 1 - alpha * A, 1 + alpha / A, -2 * cw, 1 - alpha / A};
  if (shape == 0)
    return {A * ((A + 1) - (A - 1) * cw + sA), 2 * A * ((A - 1) - (A + 1) * cw), A * ((A + 1) - (A - 1) * cw - sA),
            (A + 1) + (A - 1) * cw + sA,       -2 * ((A - 1) + (A + 1) * cw),    (A + 1) + (A - 1) * cw - sA};
  return {A * ((A + 1) + (A - 1) * cw + sA), -2 * A * ((A - 1) + (A + 1) * cw), A * ((A + 1) + (A - 1) * cw - sA),
          (A + 1) - (A - 1) * cw + sA,       2 * ((A - 1) - (A + 1) * cw),      (A + 1) - (A - 1) * cw - sA};
}

inline double magnitudeDb(const Biquad& c, double hz, double fs) {
  const std::complex<double> z1 = std::polar(1.0, -2.0 * sig::kPi * hz / fs), z2 = z1 * z1;
  return sig::db(std::abs((c.b0 + c.b1 * z1 + c.b2 * z2) / (c.a0 + c.a1 * z1 + c.a2 * z2)));
}

inline std::vector<double> filter(const Biquad& c, const std::vector<double>& x) {
  std::vector<double> y(x.size());
  double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
  for (std::size_t i = 0; i < x.size(); ++i) {
    y[i] = (c.b0 * x[i] + c.b1 * x1 + c.b2 * x2 - c.a1 * y1 - c.a2 * y2) / c.a0;
    x2 = x1, x1 = x[i], y2 = y1, y1 = y[i];
  }
  return y;
}

// ---- true peak: 16x oversampling with a long Blackman-windowed sinc ----
inline double truePeak(const std::vector<float>& y) {
  constexpr int kOver = 16, kHalf = 48;
  static double h[kOver][2 * kHalf];
  static bool made = false;
  for (int f = 0; f < kOver && !made; ++f)
    for (int k = 0; k < 2 * kHalf; ++k) {
      const double u = double(f) / kOver - (k - (kHalf - 1)), a = sig::kPi * (u / kHalf + 1.0);
      h[f][k] = (u == 0.0 ? 1.0 : std::sin(sig::kPi * u) / (sig::kPi * u)) * (0.42 - 0.5 * std::cos(a) + 0.08 * std::cos(2 * a));
    }
  made = true;
  double peak = 0.0;
  const int n = static_cast<int>(y.size());
  for (int i = kHalf; i + kHalf < n; ++i)
    for (int f = 0; f < kOver; ++f) {
      double acc = 0.0;
      for (int k = 0; k < 2 * kHalf; ++k) acc += h[f][k] * y[i - (kHalf - 1) + k];
      peak = std::max(peak, std::fabs(acc));
    }
  return peak;
}

inline double kaiser(double z) {  // beta 8
  auto i0 = [](double x) {
    double sum = 1.0, term = 1.0;
    for (int k = 1; k < 40; ++k) term *= x * x / (4.0 * k * k), sum += term;
    return sum;
  };
  return i0(8.0 * std::sqrt(std::max(0.0, 1.0 - z * z))) / i0(8.0);
}

// ---- the limiter's definition, brute force, no final clamp ----
struct Limiter {
  double ceiling;
  double releaseMs;
  double rate;

  std::vector<double> run(const std::vector<float>& in) const {
    const int look = static_cast<int>(std::ceil(1.5e-3 * rate)), latency = look + 16, n = int(in.size());
    std::vector<double> x(in.begin(), in.end()), y(x.size());
    for (double& v : x) v = v == v ? std::min(std::max(v, -1e4), 1e4) : 0.0;
    const std::vector<double> rel = released(x, look);
    for (int t = 0; t < n; ++t) {
      double sum = 0.0;
      for (int j = t - look; j <= t; ++j) sum += j < 0 ? 1.0 : rel[j];
      y[t] = (t >= latency ? x[t - latency] : 0.0) * sum / (look + 1);
    }
    return y;
  }

  // The gain each segment needs, held over look + 2 segments, then released exponentially.
  std::vector<double> released(const std::vector<double>& x, int look) const {
    const int n = int(x.size());
    std::vector<double> need(x.size()), rel(x.size());
    for (int t = 0; t < n; ++t) need[t] = std::min(1.0, ceiling / segmentPeak(x, t - 16));
    const double k = 1.0 - std::exp(-1.0 / (releaseMs * 1e-3 * rate));
    double r = 1.0;
    for (int t = 0; t < n; ++t) {
      double held = 1.0;
      for (int j = std::max(0, t - (look + 1)); j <= t; ++j) held = std::min(held, need[j]);
      r = rel[t] = held < r ? held : r + (held - r) * k;
    }
    return rel;
  }

  // Estimated true peak over [c, c+1]: the sample peak, plus twice the excess of the 8x points
  // between (Kaiser-windowed, beta 8, 32-tap sinc), plus 0.05 dB.
  static double segmentPeak(const std::vector<double>& x, int c) {
    auto at = [&](int i) { return i < 0 || i >= int(x.size()) ? 0.0 : x[i]; };
    const double samples = std::max(std::fabs(at(c)), std::fabs(at(c + 1)));
    double between = 0.0;
    for (int p = 1; p <= 7; ++p) {
      double acc = 0.0, sum = 0.0;
      for (int j = -15; j <= 16; ++j) {
        const double u = p / 8.0 - j;
        const double h = std::sin(sig::kPi * u) / (sig::kPi * u) * kaiser(u / 16.0);
        acc += h * at(c + j), sum += h;
      }
      between = std::max(between, std::fabs(acc / sum));
    }
    return (samples + 2.0 * std::max(between - samples, 0.0)) * std::pow(10.0, 0.05 / 20.0);
  }
};

// ---- the gate's definition, in double: the browser rig's NoiseGate (hysteresis a positive magnitude; attack and
// release one-pole time constants; a peak envelope that rises at once and falls over 20 ms) ----
struct Gate {
  double thresholdDb, hysteresisDb, rangeDb, attackMs, holdMs, releaseMs, rate;

  std::vector<double> run(const std::vector<float>& x) const {
    auto keep = [&](double ms) { return ms <= 0 ? 0.0 : std::exp(-1.0 / (0.001 * ms * rate)); };
    const double open = std::pow(10.0, thresholdDb / 20), close = std::pow(10.0, (thresholdDb - hysteresisDb) / 20);
    const double floor = std::pow(10.0, rangeDb / 20), holdN = std::round(holdMs * rate / 1000);
    const double fall = keep(20), up = keep(attackMs), down = keep(releaseMs);
    std::vector<double> y(x.size());
    bool isOpen = false;
    double hold = 0, g = floor, env = 0;
    for (std::size_t i = 0; i < x.size(); ++i) {
      const double level = std::fabs(x[i]);
      env = level > env ? level : fall * env + (1 - fall) * level;
      if (env >= open) isOpen = true, hold = holdN;
      else if (env < close) {
        if (hold > 0) --hold;
        else isOpen = false;
      }
      const double target = isOpen ? 1.0 : floor;
      const double c = target > g ? up : down;
      g = c * g + (1 - c) * target;
      y[i] = x[i] * g;
    }
    return y;
  }
};

}  // namespace ref
