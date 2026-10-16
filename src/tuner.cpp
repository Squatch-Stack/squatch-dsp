// SPDX-License-Identifier: MIT
#include <cmath>
#include <squatch/dsp/blocks/tuner.hpp>

namespace squatch::dsp {

TunerBlock::TunerBlock() {
  for (int k = 0; k < kParamCount; ++k) value_[k] = kParams[k].def;
  mute_.jump(value_[Mute]);
}

void TunerBlock::prepare(const Setup& s, Arena& mem) {
  config_.sampleRate = static_cast<float>(s.sampleRate);
  tuner_.configure(config_);
  mute_.prepare(s.sampleRate, kParams[Mute].smoothMs);
  maxBlock_ = s.maxBlock;
  mid_ = s.layout == Layout::Stereo ? mem.alloc<float>(static_cast<std::size_t>(s.maxBlock)) : nullptr;
  samples_ = 0;
}

void TunerBlock::process(const float* in, float* out, int n) {
  analyse(in, n);  // before out is written: in may be out
  output(in, out, n);
}

// Stereo: the tuner hears the mid; both sides pass or mute together.
void TunerBlock::processChannels(const AudioIO& io, int n) {
  if (io.inChannels < 2 || io.outChannels < 2 || !mid_ || n > maxBlock_) return Block::processChannels(io, n);
  for (int i = 0; i < n; ++i) mid_[i] = 0.5f * (io.in[0][i] + io.in[1][i]);
  analyse(mid_, n);
  Smoother right = mute_;  // the same ramp for the right side
  output(io.in[0], io.out[0], n);
  mute_ = right;
  output(io.in[1], io.out[1], n);
}

void TunerBlock::analyse(const float* in, int n) {
  tuner_.process(in, n);
  samples_ += static_cast<std::uint64_t>(n);
  TunerSnapshot& s = readings_.back();
  s.reading = tuner_.reading();
  s.samples = samples_;
  readings_.publish();
}

void TunerBlock::reset() {
  tuner_.reset();
  samples_ = 0;
}

void TunerBlock::set(ParamId id, float value) {
  if (id >= kParamCount) return;
  value_[id] = kParams[id].clamp(value);
  if (id == Mute) mute_.setTarget(value_[Mute]);
}

float TunerBlock::get(ParamId id) const {
  if (id < kParamCount) return value_[id];
  return id > Bypass && id < kCount ? meter(id) : NAN;
}

// The newest reading against this block's A4; with no reading, the meters rest at their defaults.
float TunerBlock::meter(ParamId id) const {
  const tuner::TunerReading& r = tuner_.reading();
  const float hz = r.steadyHz > 0.0f ? r.steadyHz : r.hz;
  if (id == Signal) return r.valid ? 1.0f : 0.0f;
  if (!r.valid || !(hz > 0.0f)) return kParams[id].def;
  if (id == Frequency) return kParams[id].clamp(hz);
  const double note = 69.0 + 12.0 * std::log2(double(hz) / double(value_[A4]));
  const long midi = std::lround(note);
  if (id == Cents) return kParams[id].clamp(static_cast<float>((note - double(midi)) * 100.0));
  if (id == Note) return static_cast<float>((midi % 12 + 12) % 12);
  return kParams[Octave].clamp(static_cast<float>(midi / 12 - 1));
}

// Through, muted, or ramping between the two.
void TunerBlock::output(const float* in, float* out, int n) {
  if (!mute_.smoothing()) {
    if (mute_.value() >= 1.0f) return clear(out, n);
    if (mute_.value() <= 0.0f) return copy(in, out, n);
  }
  for (int i = 0; i < n; ++i) out[i] = in[i] * (1.0f - mute_.next());
}

}  // namespace squatch::dsp
