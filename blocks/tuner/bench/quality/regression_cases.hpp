// SPDX-License-Identifier: MIT
// Regression cases: one synthetic recording per failure the quality suite found on real guitars,
// each made to fail the way the real one did (checked against the core before the fix) and each
// with the property the fix must keep. test_quality checks the properties; tuner_quality scores
// them as the "regression" set.
//
//   hum-tail          an A2 dies into 56.3 Hz hum (a Hi-Z input's hum wanders 55-67 Hz, outside
//                     the mains guard) with no floor learned: no A1..C2 may ever be shown
//   quiet-tail        a stiff E3 whose 2nd partial strays 3 c from the stiff-string model decays to
//                     the gate as its upper partials die: the number never leaps (B chased the
//                     stray partial as the others faded, and the number leapt 4.6 c)
//   fading-fundamental  a D3 whose fundamental and odd partials die first: never re-named an
//                     octave up without a pluck
//   slow-glide        an E2 whose pitch falls 20 c over its first second or so: the number
//                     follows it (it trailed by a quarter second: 2.0 c median error over
//                     0.5-1.7 s, now 1.5 c) and never steps
//   strings-together  a hard open E with the other five open strings ringing 3-6 dB below: the E
//                     is shown within 3 s, and nothing else ever is
//
// The generator is additive and exact (partials of a stiff string, per-partial decay, an
// exponential glide), so truth is known at every moment. It is separate from ../synth.hpp so
// those cases stay bit-identical.
#pragma once
#include <cmath>
#include <random>
#include <string>
#include <vector>

#include "cases.hpp"
#include "synth_cases.hpp"

namespace quality {

struct RegString {
  double f0 = 82.41, B = 1e-4;
  double onset = 0.02;                 // seconds
  double amp = 0.3;                    // amplitude of partial 1 at the onset
  int partials = 10;
  double t60 = 4.0, t60Slope = 0.3;    // partial k: t60 / (1 + slope (k - 1))
  double fundamentalT60 = -1.0;        // > 0: partial 1 decays with this T60 instead
  double oddT60 = -1.0;                // > 0: odd partials above 1 decay with this T60
  double glideCents = 0.0, glideTau = 0.7;  // pitch = f0 * 2^(g e^(-t / tau) / 1200)
  double pluckPos = 0.2;
  std::vector<double> partialCents;    // partial k sits partialCents[k-1] off the stiff model
};

struct RegHum {
  double hz = 0.0, amp = 0.0;  // fundamental amplitude; harmonics odd 1/k, even 0.3/k
  int harmonics = 5;
};

inline double regPitch(const RegString& s, double t) {
  return s.f0 * std::exp2(s.glideCents * std::exp(-std::max(0.0, t) / s.glideTau) / 1200.0);
}

inline double regT60(const RegString& s, int k) {
  if (k == 1 && s.fundamentalT60 > 0.0) return s.fundamentalT60;
  if (k > 1 && (k & 1) && s.oddT60 > 0.0) return s.oddT60;
  return s.t60 / (1.0 + s.t60Slope * (k - 1));
}

/// What a regression recording holds: strings, hum, white noise (RMS), its length.
struct RegScene {
  std::vector<RegString> strings;
  RegHum hum;
  double noise = 1e-5, seconds = 4.0;
};

/// Adds partial k of string s (its stiffness, stray, decay and glide) to y.
inline void regPartial(const RegString& s, int k, double fs, double phase, std::vector<double>& y) {
  const double pi = 3.141592653589793;
  const double off = static_cast<size_t>(k) <= s.partialCents.size() ? s.partialCents[static_cast<size_t>(k - 1)] : 0.0;
  const double fk = k * std::sqrt(1.0 + s.B * k * k) * std::exp2(off / 1200.0);
  const double a = s.amp * std::fabs(std::sin(k * pi * s.pluckPos)) / (k * std::sin(pi * s.pluckPos));
  const double decay = std::log(1000.0) / regT60(s, k);
  for (size_t i = static_cast<size_t>(s.onset * fs); i < y.size(); ++i) {
    const double t = static_cast<double>(i) / fs - s.onset;
    y[i] += a * std::exp(-decay * t) * std::sin(phase);
    phase += 2.0 * pi * fk * regPitch(s, t) / fs;
  }
}

/// The scene at `fs`, deterministic per seed.
inline std::vector<float> regRender(const RegScene& sc, double fs, unsigned seed) {
  const double pi = 3.141592653589793;
  std::vector<double> y(static_cast<size_t>(sc.seconds * fs), 0.0);
  std::mt19937 rng(seed);
  std::uniform_real_distribution<double> phase(0.0, 2.0 * pi);
  for (const RegString& s : sc.strings)
    for (int k = 1; k <= s.partials && k * s.f0 * 1.02 * std::sqrt(1.0 + s.B * k * k) <= 0.45 * fs; ++k) regPartial(s, k, fs, phase(rng), y);
  std::normal_distribution<double> g(0.0, 1.0);
  std::vector<float> out(y.size());
  for (size_t i = 0; i < y.size(); ++i) {
    double v = y[i] + sc.noise * g(rng);
    for (int k = 1; k <= sc.hum.harmonics && sc.hum.amp > 0.0; ++k)
      v += sc.hum.amp * ((k & 1) ? 1.0 : 0.3) / k * std::sin(2.0 * pi * sc.hum.hz * k * static_cast<double>(i) / fs + 0.7 * k);
    out[i] = static_cast<float>(v);
  }
  return out;
}

/// A case whose one segment is the first string, with exact truth.
inline Case regCase(const std::string& name, std::vector<RegString> strings, RegHum hum, double noise, double seconds) {
  Case c;
  c.dataset = "regression";
  c.family = name;
  c.value = name;
  c.id = "reg-" + name;
  c.source = "regression_cases.hpp";
  c.fs = 48000.0;
  const RegString first = strings.front();
  Segment g;
  g.onset = first.onset;
  g.end = seconds;
  g.midi = static_cast<int>(std::lround(69.0 + 12.0 * std::log2(first.f0 / 440.0)));
  g.truth = "exact";
  g.refHz = first.f0;
  g.exact = [first](double t) { return regPitch(first, t); };
  c.segments.push_back(g);
  const unsigned seed = static_cast<unsigned>(std::hash<std::string>{}(name) & 0xffffu) + 1u;
  const RegScene scene{std::move(strings), hum, noise, seconds};
  c.load = [scene, seed](Case& self) {
    self.x = regRender(scene, self.fs, seed);
    return true;
  };
  return c;
}

inline Case humTailCase() {
  RegString a;
  a.f0 = midiHz(45) * std::exp2(3.0 / 1200.0);
  a.B = 1.5e-4;
  a.t60 = 2.0;
  return regCase("hum-tail", {a}, {56.3, 0.3 * std::pow(10.0, -28.0 / 20.0)}, 1e-5, 7.0);
}

inline Case quietTailCase() {
  RegString e;
  e.f0 = midiHz(52) * std::exp2(-4.0 / 1200.0);
  e.B = 8e-4;
  e.amp = 0.25;
  e.t60 = 3.2;
  e.t60Slope = 0.9;
  e.partialCents = {0.0, 3.0};  // a real string's partials stray a few cents from the model
  return regCase("quiet-tail", {e}, {}, 1e-4, 6.0);
}

inline Case fadingFundamentalCase() {
  RegString d;
  d.f0 = midiHz(50) * std::exp2(6.0 / 1200.0);
  d.B = 1e-4;
  d.t60 = 5.0;
  d.t60Slope = 0.0;
  d.fundamentalT60 = 1.6;
  d.oddT60 = 1.8;
  return regCase("fading-fundamental", {d}, {}, 1e-5, 5.0);
}

inline Case slowGlideCase() {
  RegString e;
  e.f0 = midiHz(40) * std::exp2(-2.0 / 1200.0);
  e.B = 1.2e-4;
  e.t60 = 6.0;
  e.glideCents = 20.0;
  e.glideTau = 0.7;
  return regCase("slow-glide", {e}, {}, 1e-5, 4.0);
}

inline Case stringsTogetherCase() {
  std::vector<RegString> s;
  const int open[6] = {40, 45, 50, 55, 59, 64};
  const double detune[6] = {-1.0, 1.5, -2.0, 2.5, -1.0, 1.0}, level[6] = {0.0, -4.0, -5.0, -3.0, -6.0, -4.0};
  const double B[6] = {1.5e-4, 1.2e-4, 1e-4, 1e-4, 6e-5, 5e-5}, t60[6] = {5.0, 4.5, 4.0, 3.5, 3.0, 3.0};
  for (int i = 0; i < 6; ++i) {
    RegString r;
    r.f0 = midiHz(open[i]) * std::exp2(detune[i] / 1200.0);
    r.B = B[i];
    r.t60 = t60[i];
    r.onset = i == 0 ? 0.3 : 0.32;
    r.amp = 0.3 * std::pow(10.0, level[i] / 20.0);
    r.pluckPos = i == 0 ? 0.12 : 0.3;
    s.push_back(r);
  }
  return regCase("strings-together", s, {}, 3e-5, 8.0);
}

inline std::vector<Case> regressionCases() {
  return {humTailCase(), quietTailCase(), fadingFundamentalCase(), slowGlideCase(), stringsTogetherCase()};
}

}  // namespace quality
