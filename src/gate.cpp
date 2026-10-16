// SPDX-License-Identifier: MIT
#include <cmath>
#include <squatch/dsp/blocks/gate.hpp>

namespace squatch::dsp {
namespace {

constexpr double kEnvelopeMs = 20.0;  // the detector's fall time constant

double toGain(double db) { return std::pow(10.0, db / 20.0); }

// A one-pole's kept fraction for a time constant in ms (0 ms: no smoothing).
double keep(double ms, double sampleRate) { return ms <= 0.0 ? 0.0 : std::exp(-1.0 / (0.001 * ms * sampleRate)); }

// The peak of the frame, non-finite counting as silence.
double peakOf(double a) { return std::isfinite(a) ? a : 0.0; }

}  // namespace

Gate::Gate() {
  for (int k = 0; k < kParamCount; ++k) value_[k] = kParams[k].def;
  update();
  reset();
}

void Gate::prepare(const Setup& s, Arena&) {
  sampleRate_ = s.sampleRate;
  update();
  reset();
}

void Gate::process(const float* in, float* out, int n) {
  for (int i = 0; i < n; ++i) {
    const float x = in[i];
    detect(peakOf(std::fabs(static_cast<double>(x))));
    out[i] = static_cast<float>(x * step());
  }
}

// Linked stereo: the louder side decides, both sides get the same gain.
void Gate::processChannels(const AudioIO& io, int n) {
  if (io.inChannels < 2 || io.outChannels < 2) return Block::processChannels(io, n);
  const float* l = io.in[0];
  const float* r = io.in[1];
  for (int i = 0; i < n; ++i) {
    const float a = l[i], b = r[i];
    detect(peakOf(std::fmax(std::fabs(static_cast<double>(a)), std::fabs(static_cast<double>(b)))));
    const double g = step();
    io.out[0][i] = static_cast<float>(a * g);
    io.out[1][i] = static_cast<float>(b * g);
  }
}

void Gate::reset() {
  open_ = false;
  holdLeft_ = 0;
  env_ = 0.0;
  gain_ = floor_;
}

void Gate::set(ParamId id, float value) {
  if (id >= kParamCount) return;
  value_[id] = kParams[id].clamp(value);
  update();
}

float Gate::get(ParamId id) const {
  if (id < kParamCount) return value_[id];
  if (id == Reduction)
    return kParams[Reduction].clamp(gain_ > 0.0 ? static_cast<float>(-20.0 * std::log10(gain_)) : kParams[Reduction].max);
  if (id == Open) return open_ ? 1.0f : 0.0f;
  return NAN;
}

// The envelope rises at once and falls over kEnvelopeMs; the thresholds, hold and open state follow.
void Gate::detect(double level) {
  env_ = level > env_ ? level : envFall_ * env_ + (1.0 - envFall_) * level;
  if (env_ >= openLevel_) {
    open_ = true;
    holdLeft_ = holdSamples_;
  } else if (env_ < closeLevel_) {
    if (holdLeft_ > 0) --holdLeft_;
    else open_ = false;
  }
}

// One-pole towards unity while open, towards the floor while closed (a range change moves the same way).
double Gate::step() {
  const double target = open_ ? 1.0 : floor_;
  const double c = target > gain_ ? attack_ : release_;
  gain_ = c * gain_ + (1.0 - c) * target;
  return gain_;
}

// Parameters apply at once: the thresholds are comparisons, and the gain already glides.
void Gate::update() {
  envFall_ = keep(kEnvelopeMs, sampleRate_);
  attack_ = keep(value_[Attack], sampleRate_);
  release_ = keep(value_[Release], sampleRate_);
  openLevel_ = toGain(value_[Threshold]);
  closeLevel_ = toGain(static_cast<double>(value_[Threshold]) - value_[Hysteresis]);
  floor_ = toGain(value_[Range]);
  holdSamples_ = static_cast<int>(std::lround(value_[Hold] * sampleRate_ / 1000.0));
}

}  // namespace squatch::dsp
