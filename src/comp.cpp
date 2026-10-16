// SPDX-License-Identifier: MIT
#include <cmath>
#include <squatch/dsp/blocks/comp.hpp>

namespace squatch::dsp {
namespace {

float coef(float ms, double fs) { return static_cast<float>(std::exp(-1.0 / (0.001 * ms * fs))); }

}  // namespace

Compressor::Compressor() {
  for (int k = 0; k < kParamCount; ++k) p_[k].jump(kParams[k].def);
  design();
}

void Compressor::prepare(const Setup& s, Arena&) {
  sampleRate_ = s.sampleRate;
  for (int k = 0; k < kParamCount; ++k) p_[k].prepare(s.sampleRate, kParams[k].smoothMs);
  design();
  reset();
}

void Compressor::reset() {
  gr_ = 0.0f;
  grPeak_ = 0.0f;
}

void Compressor::design() {
  attack_ = coef(p_[Attack].value(), sampleRate_);
  release_ = coef(p_[Release].value(), sampleRate_);
  makeup_ = std::pow(10.0f, p_[Makeup].value() / 20.0f);
}

float Compressor::curve(float x, float threshold, float ratio, float knee) {
  const float d = x - threshold;
  if (2.0f * d < -knee) return x;
  if (knee > 0.0f && 2.0f * std::fabs(d) <= knee) return x + (1.0f / ratio - 1.0f) * (d + knee / 2.0f) * (d + knee / 2.0f) / (2.0f * knee);
  return threshold + d / ratio;
}

void Compressor::process(const float* in, float* out, int n) {
  const float threshold = p_[Threshold].value(), ratio = p_[Ratio].value(), knee = p_[Knee].value();
  float peak = 0.0f;
  for (int done = 0; done < n;) {
    const bool moving = smoothing();
    const int len = moving ? (n - done < kControlRate ? n - done : kControlRate) : n - done;
    if (moving) {
      for (Smoother& s : p_) s.advance(len);
      design();
    }
    const float mix = p_[Mix].value();
    for (int i = done; i < done + len; ++i) {
      const float a = std::fabs(in[i]);
      const float x = a > 1e-9f ? 20.0f * std::log10(a) : -180.0f;
      const float want = x - curve(x, threshold, ratio, knee);
      gr_ = want > gr_ ? attack_ * gr_ + (1.0f - attack_) * want : release_ * gr_ + (1.0f - release_) * want;
      peak = std::fmax(peak, gr_);
      const float g = std::pow(10.0f, -gr_ / 20.0f) * makeup_;
      out[i] = in[i] * (mix * g + (1.0f - mix));
    }
    done += len;
  }
  grPeak_ = peak;
}

void Compressor::set(ParamId id, float value) {
  if (id >= kParamCount) return;
  p_[id].setTarget(kParams[id].clamp(value));
  if (!smoothing()) design();
}

float Compressor::get(ParamId id) const {
  if (id < kParamCount) return p_[id].target();
  if (id == Reduction) return kParams[Reduction].clamp(grPeak_);
  return NAN;
}

bool Compressor::smoothing() const {
  for (const Smoother& s : p_)
    if (s.smoothing()) return true;
  return false;
}

}  // namespace squatch::dsp
