// SPDX-License-Identifier: MIT
// Synthetic plucked-string tones with known truth, for measuring tuners.
//
// Model (all in double precision):
//  - partial k at k * f0 * sqrt(1 + B k^2) (stiff string, Fletcher & Rossing),
//  - amplitude from pluck and pickup position: sin(k pi p) sin(k pi q) / k (a velocity pickup
//    over a plucked string), with an optional extra gain on the fundamental,
//  - per-partial exponential decay, higher partials faster,
//  - pitch glide from tension modulation: +g cents * (envelope of partial 1)^2, so the note
//    starts sharp and settles as it decays (Tolonen, Valimaki & Karjalainen 2000),
//  - a pick-noise burst, white noise at a set SNR, optional 60 Hz mains hum with its 3rd.
// Truth is f0: the fundamental once the glide has died away.
//
// Optional, all off by default so the older benches synthesise exactly what they did:
//  - vibrato (sinusoidal, in cents) and a slow bend (raised-cosine ramp, then held),
//  - mains hum at 50 or 60 Hz with a chosen number of harmonics (odd ones stronger, as a
//    single-coil pickup hears it) instead of the fixed 60 + 180 Hz pair,
//  - a second pickup mixed in (the comb of two positions, as a Strat's in-between settings),
//  - a peak level in dBFS for the whole output, applied last.
// truthAt() includes the glide, vibrato and bend, so it is the true pitch at every moment.
#pragma once
#include <algorithm>
#include <cmath>
#include <random>
#include <vector>

namespace bench {

struct ToneSpec {
  double f0 = 82.4069;
  double inharmonicity = 0.0;  // B
  int partials = 12;
  double pluckPos = 0.2, pickupPos = 0.2;
  double fundamentalGainDb = 0.0;
  double t60 = 4.0;            // seconds, partial 1
  double t60Slope = 0.25;      // partial k decays (1 + slope (k-1)) times faster
  double glideCents = 0.0;
  double snrDb = 60.0;         // against the initial RMS of the tone
  double humDb = -200.0;       // hum level against the initial RMS
  double pickNoiseDb = -200.0; // 5 ms burst at the onset
  double peak = 0.3;
  double preroll = 0.1;        // seconds of noise before the pluck
  double seconds = 2.0;
  double vibratoHz = 0.0, vibratoCents = 0.0, vibratoStart = 0.0;  // depth is +- cents
  double bendCents = 0.0, bendStart = 0.0, bendSeconds = 0.0;       // ramp to +bendCents, held
  double humHz = 60.0;         // with humHarmonics > 0: the mains frequency
  int humHarmonics = 0;        // 0: the fixed 60 + 180 Hz pair; n: n harmonics of humHz
  double pickupPos2 = -1.0;    // >= 0: a second pickup at this position, mixed equally
  double peakDbfs = 1.0;       // <= 0: scale the output so its peak sits here
};

/// Pitch factor (a multiplier on every partial) from glide, vibrato and bend at time t >= 0.
inline double pitchFactor(const ToneSpec& s, double t) {
  const double pi = 3.141592653589793;
  const double env1 = std::exp(-std::log(1000.0) / s.t60 * t);
  double cents = s.glideCents * env1 * env1;
  if (s.vibratoCents != 0.0 && t >= s.vibratoStart) cents += s.vibratoCents * std::sin(2.0 * pi * s.vibratoHz * (t - s.vibratoStart));
  if (s.bendCents != 0.0 && t >= s.bendStart) {
    const double u = s.bendSeconds > 0.0 ? std::min(1.0, (t - s.bendStart) / s.bendSeconds) : 1.0;
    cents += s.bendCents * 0.5 * (1.0 - std::cos(pi * u));
  }
  return std::exp2(cents / 1200.0);
}

inline double partialFreq(const ToneSpec& s, int k) { return k * s.f0 * std::sqrt(1.0 + s.inharmonicity * k * k); }

inline double partialAmp(const ToneSpec& s, int k) {
  const double pi = 3.141592653589793;
  double pick = std::sin(k * pi * s.pickupPos);
  if (s.pickupPos2 >= 0.0) pick = 0.5 * (pick + std::sin(k * pi * s.pickupPos2));
  double a = std::sin(k * pi * s.pluckPos) * pick / k;
  if (k == 1) a *= std::pow(10.0, s.fundamentalGainDb / 20.0);
  return std::fabs(a);
}

/// Mains hum at time t, unit scale: the legacy 60 + 180 Hz pair, or n harmonics of humHz with
/// odd harmonics at 1/k and even ones at 0.3/k.
inline double humAt(const ToneSpec& s, double t) {
  const double pi = 3.141592653589793;
  if (s.humHarmonics <= 0) return std::sin(2 * pi * 60 * t) + 0.5 * std::sin(2 * pi * 180 * t);
  double v = 0.0;
  for (int k = 1; k <= s.humHarmonics; ++k) v += ((k & 1) ? 1.0 : 0.3) / k * std::sin(2 * pi * s.humHz * k * t + 0.7 * k);
  return v;
}

namespace detail {

/// RMS of the tone over its first 100 ms, the reference for SNR and hum levels.
inline double onsetRms(const std::vector<double>& y, size_t pre, double rate) {
  const size_t r1 = std::min(y.size(), pre + static_cast<size_t>(0.1 * rate));
  double rms = 0.0;
  for (size_t i = pre; i < r1; ++i) rms += y[i] * y[i];
  return std::sqrt(rms / static_cast<double>(r1 - pre));
}

inline void scalePeak(std::vector<float>& out, double peakDbfs) {
  if (peakDbfs > 0.0) return;
  float peak = 0.0f;
  for (float v : out) peak = std::max(peak, std::fabs(v));
  const float g = peak > 0.0f ? static_cast<float>(std::pow(10.0, peakDbfs / 20.0)) / peak : 0.0f;
  for (float& v : out) v *= g;
}

/// Adds white noise, hum and the pick burst to the tone and converts to float.
inline std::vector<float> finish(const ToneSpec& s, const std::vector<double>& y, size_t pre, double rate, std::mt19937& rng) {
  const double rms = onsetRms(y, pre, rate);
  std::normal_distribution<double> g(0.0, 1.0);
  const double noise = rms * std::pow(10.0, -s.snrDb / 20.0), hum = rms * std::pow(10.0, s.humDb / 20.0);
  const double pick = rms * std::pow(10.0, s.pickNoiseDb / 20.0);
  std::vector<float> out(y.size());
  for (size_t i = 0; i < y.size(); ++i) {
    double v = y[i] + noise * g(rng) + hum * humAt(s, static_cast<double>(i) / rate);
    if (i >= pre && i < pre + static_cast<size_t>(0.005 * rate)) v += pick * g(rng);
    out[i] = static_cast<float>(v);
  }
  scalePeak(out, s.peakDbfs);
  return out;
}

}  // namespace detail

inline std::vector<float> synthesize(const ToneSpec& s, double rate, std::mt19937& rng) {
  const double pi = 3.141592653589793;
  const size_t pre = static_cast<size_t>(s.preroll * rate), n = pre + static_cast<size_t>(s.seconds * rate);
  std::vector<double> y(n, 0.0);
  std::uniform_real_distribution<double> uni(0.0, 2.0 * pi);
  double norm = 0.0;
  for (int k = 1; k <= s.partials; ++k) norm += partialAmp(s, k);
  const double decay1 = std::log(1000.0) / s.t60;  // amplitude rate of partial 1 (1/s)
  for (int k = 1; k <= s.partials && partialFreq(s, k) < 0.45 * rate; ++k) {
    const double amp = s.peak * partialAmp(s, k) / norm, fk = partialFreq(s, k);
    const double dk = decay1 * (1.0 + s.t60Slope * (k - 1));
    double ph = uni(rng);
    for (size_t i = pre; i < n; ++i) {
      const double t = static_cast<double>(i - pre) / rate;
      y[i] += amp * std::exp(-dk * t) * std::sin(ph);
      ph += 2.0 * pi * fk * pitchFactor(s, t) / rate;
    }
  }
  return detail::finish(s, y, pre, rate, rng);
}

/// The true fundamental at time t after the pluck: glide, vibrato and bend included.
inline double truthAt(const ToneSpec& s, double t) { return s.f0 * pitchFactor(s, t < 0.0 ? 0.0 : t); }

/// Band-limited resampling by `ratio` (> 1 raises pitch): windowed-sinc interpolation.
inline std::vector<float> resample(const std::vector<float>& x, double ratio) {
  const double pi = 3.141592653589793;
  const int half = 48;
  const double cut = ratio > 1.0 ? 0.95 / ratio : 0.95;
  std::vector<float> y(static_cast<size_t>(static_cast<double>(x.size()) / ratio));
  for (size_t i = 0; i < y.size(); ++i) {
    const double pos = static_cast<double>(i) * ratio;
    const long c = static_cast<long>(std::floor(pos));
    double acc = 0.0;
    for (long j = c - half + 1; j <= c + half; ++j) {
      if (j < 0 || j >= static_cast<long>(x.size())) continue;
      const double t = pos - static_cast<double>(j);
      const double sinc = std::fabs(t) < 1e-12 ? cut : std::sin(pi * cut * t) / (pi * t);
      const double w = 0.42 + 0.5 * std::cos(pi * t / half) + 0.08 * std::cos(2 * pi * t / half);
      acc += x[static_cast<size_t>(j)] * sinc * w;
    }
    y[i] = static_cast<float>(acc);
  }
  return y;
}

}  // namespace bench
