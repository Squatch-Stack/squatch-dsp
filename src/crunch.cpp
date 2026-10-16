// SPDX-License-Identifier: MIT
#include <cmath>
#include <squatch/dsp/blocks/crunch.hpp>

namespace squatch::dsp {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr float kBias1 = 0.25f, kBias2 = -0.15f;  // stage asymmetry: even harmonics, like a triode
constexpr float kStage3 = 2.0f;
// 8th-order Butterworth as four second-order sections: Q = 1 / (2 cos((2k - 1) pi / 16)).
constexpr double kButterQ[4] = {0.50979558, 0.60134489, 0.89997622, 2.56291545};

float shaped(float x, float bias) { return std::tanh(x + bias) - std::tanh(bias); }

}  // namespace

void Crunch::Svf::design(double fc, double fs, double q) {
  const double g = std::tan(kPi * fc / fs), k = 1.0 / q;
  const double d1 = 1.0 / (1.0 + g * (g + k));
  a1 = static_cast<float>(d1);
  a2 = static_cast<float>(g * d1);
  a3 = static_cast<float>(g * g * d1);
}

void Crunch::Pole::design(double fc, double fs) {
  const double t = std::tan(kPi * std::fmin(fc, 0.49 * fs) / fs);
  g = static_cast<float>(t / (1.0 + t));
}

Crunch::Crunch() {
  for (int k = 0; k < kParamCount; ++k) p_[k].jump(kParams[k].def);
  design();
}

void Crunch::prepare(const Setup& s, Arena&) {
  sampleRate_ = s.sampleRate;
  for (int k = 0; k < kParamCount; ++k) p_[k].prepare(s.sampleRate, kParams[k].smoothMs);
  const double os = sampleRate_ * kOversample, fc = 0.4 * sampleRate_;
  for (int k = 0; k < 4; ++k) {
    up_[k].design(fc, os, kButterQ[k]);
    down_[k].design(fc, os, kButterQ[k]);
  }
  couple1_.design(20.0, os);
  couple2_.design(20.0, os);
  lp1_.design(7000.0, os);
  lp2_.design(5500.0, os);
  dc_.design(10.0, sampleRate_);
  design();
  reset();
}

void Crunch::reset() {
  for (int k = 0; k < 4; ++k) up_[k].ic1 = up_[k].ic2 = down_[k].ic1 = down_[k].ic2 = 0.0f;
  tight_.s = dc_.s = couple1_.s = couple2_.s = lp1_.s = lp2_.s = 0.0f;
  peak_ = 0.0f;
  clipped_ = false;
}

void Crunch::design() {
  const float gain = p_[Gain].value();
  g1_ = std::pow(10.0f, 36.0f * gain / 20.0f);
  g2_ = std::pow(10.0f, (6.0f + 12.0f * gain) / 20.0f);
  tight_.design(40.0 * std::pow(10.0, static_cast<double>(p_[Tight].value())), sampleRate_);
  level_ = std::pow(10.0f, p_[Level].value() / 20.0f);
}

float Crunch::sample(float x) {
  const float in = tight_.hp(x) * g1_;
  float y = 0.0f;
  for (int j = 0; j < kOversample; ++j) {
    float u = j == 0 ? in * static_cast<float>(kOversample) : 0.0f;  // zero-stuffing, gain restored
    for (Svf& f : up_) u = f.run(u);
    float s = lp1_.lp(couple1_.hp(shaped(u, kBias1)));
    const float drive2 = g2_ * s;
    clipped_ = clipped_ || std::fabs(drive2) > 1.0f;
    s = lp2_.lp(couple2_.hp(shaped(drive2, kBias2)));
    s = std::tanh(kStage3 * s);
    for (Svf& f : down_) s = f.run(s);
    if (j == 0) y = s;  // keep one of every four
  }
  return dc_.hp(y) * level_;
}

void Crunch::process(const float* in, float* out, int n) {
  float peak = 0.0f;
  clipped_ = false;
  for (int done = 0; done < n;) {
    const bool moving = smoothing();
    const int len = moving ? (n - done < kControlRate ? n - done : kControlRate) : n - done;
    if (moving) {
      for (Smoother& s : p_) s.advance(len);
      design();
    }
    for (int i = done; i < done + len; ++i) {
      out[i] = sample(in[i]);
      peak = std::fmax(peak, std::fabs(out[i]));
    }
    done += len;
  }
  peak_ = peak;
}

void Crunch::set(ParamId id, float value) {
  if (id >= kParamCount) return;
  p_[id].setTarget(kParams[id].clamp(value));
  if (!smoothing()) design();
}

float Crunch::get(ParamId id) const {
  if (id < kParamCount) return p_[id].target();
  if (id == Out) return kParams[Out].clamp(peak_ > 0.0f ? 20.0f * std::log10(peak_) : kParams[Out].min);
  if (id == Clip) return clipped_ ? 1.0f : 0.0f;
  return NAN;
}

bool Crunch::smoothing() const {
  for (const Smoother& s : p_)
    if (s.smoothing()) return true;
  return false;
}

}  // namespace squatch::dsp
