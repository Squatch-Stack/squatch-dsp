// SPDX-License-Identifier: MIT
#include <cmath>
#include <squatch/dsp/blocks/eq.hpp>

namespace squatch::dsp {

Eq::Eq() {
  for (int k = 0; k < kParamCount; ++k) p_[k].jump(kParams[k].def);
  design();
}

void Eq::prepare(const Setup& s, Arena&) {
  sampleRate_ = s.sampleRate;
  for (int k = 0; k < kParamCount; ++k) p_[k].prepare(s.sampleRate, kParams[k].smoothMs);
  design();
  reset();
}

void Eq::process(const float* in, float* out, int n) {
  for (int done = 0; done < n;) {
    const int len = smoothing() ? (n - done < kControlRate ? n - done : kControlRate) : n - done;
    if (smoothing()) advance(len);
    bands_[0].process(in + done, out + done, len);
    bands_[1].process(out + done, out + done, len);
    bands_[2].process(out + done, out + done, len);
    done += len;
  }
}

void Eq::reset() {
  for (Biquad& b : bands_) b.reset();
}

void Eq::set(ParamId id, float value) {
  if (id >= kParamCount) return;
  p_[id].setTarget(kParams[id].clamp(value));
}

float Eq::get(ParamId id) const { return id < kParamCount ? p_[id].target() : NAN; }

// A band's envelope decays with time constant Q / (pi f0); shelves have Q = 1/sqrt(2).
int Eq::tail() const {
  const double pi = 3.14159265358979323846;
  const double shelf = kShelfQ / (pi * std::fmin(p_[LowFreq].target(), p_[HighFreq].target()));
  const double peak = p_[MidQ].target() / (pi * p_[MidFreq].target());
  return static_cast<int>(std::ceil(14.0 * std::fmax(shelf, peak) * sampleRate_));
}

bool Eq::smoothing() const {
  for (const Smoother& s : p_)
    if (s.smoothing()) return true;
  return false;
}

// Moves the smoothers to the end of this sub-block and designs for where they land.
void Eq::advance(int n) {
  for (Smoother& s : p_) s.advance(n);
  design();
}

void Eq::design() {
  const double fs = sampleRate_;
  bands_[0].setCoeffs(designBiquad(Shape::LowShelf, fs, p_[LowFreq].value(), p_[LowGain].value(), kShelfQ));
  bands_[1].setCoeffs(designBiquad(Shape::Peak, fs, p_[MidFreq].value(), p_[MidGain].value(), p_[MidQ].value()));
  bands_[2].setCoeffs(designBiquad(Shape::HighShelf, fs, p_[HighFreq].value(), p_[HighGain].value(), kShelfQ));
}

}  // namespace squatch::dsp
