// SPDX-License-Identifier: MIT
// Built with exceptions and RTTI, against NeuralAmpModelerCore, Eigen and nlohmann json (see
// nam_block.hpp); nothing else in squatch-dsp is.
#include "nam_block.hpp"

#include <cmath>
#include <exception>
#include <filesystem>

#include "NAM/dsp.h"
#include "NAM/get_dsp.h"

namespace squatch::dsp {

NamAmp::NamAmp() {
  for (int k = 0; k < kParamCount; ++k) value_[k] = kParams[k].def;
}

void NamAmp::use(nam::DSP* model, float makeupDb) {
  model_ = model;
  makeup_ = makeupDb;
}

float NamAmp::gainTarget(int k) const {
  const float make = k == Output && value_[Normalize] >= 0.5f ? makeup_ : 0.0f;
  return dbToGain(value_[k] + make);
}

void NamAmp::designBass() {
  const float db = bass_.value();
  bassActive_ = db != 0.0f;
  if (bassActive_) shelf_.setCoeffs(designBiquad(Shape::LowShelf, rate_, 160.0, db, 0.7071));
  else shelf_.reset();
}

void NamAmp::prepare(const Setup& s, Arena& mem) {
  scratch_ = mem.alloc<float>(static_cast<std::size_t>(s.maxBlock));
  maxBlock_ = s.maxBlock;
  rate_ = s.sampleRate;
  for (int k = Input; k <= Output; ++k) {
    gain_[k].prepare(s.sampleRate, kParams[k].smoothMs);
    gain_[k].jump(gainTarget(k));
  }
  bass_.prepare(s.sampleRate, kParams[Bass].smoothMs);
  bass_.jump(value_[Bass]);
  shelf_.reset();
  designBass();
  dcR_ = 1.0 - 2.0 * 3.14159265358979323846 * 10.0 / s.sampleRate;
  dcX_ = dcY_ = 0.0;
  ready_ = false;
  peak_ = 0.0f;
  if (!model_ || !scratch_) return;
  model_->SetPrewarmOnReset(true);
  model_->Reset(s.sampleRate, s.maxBlock);
  ready_ = true;
}

// How many of the next `left` samples run before the shelf needs redesigning (it glides in 16s).
int NamAmp::shapeSegment(int left) {
  if (!bass_.smoothing()) return left;
  const int len = left < 16 ? left : 16;
  bass_.advance(len);
  designBass();
  return len;
}

void NamAmp::dcBlock(float* x, int n) {
  double x1 = dcX_, y1 = dcY_;
  for (int i = 0; i < n; ++i) {
    const double in = x[i], y = in - x1 + dcR_ * y1;
    x1 = in, y1 = y;
    x[i] = static_cast<float>(y);
  }
  dcX_ = x1, dcY_ = y1;
}

void NamAmp::shape(float* x, int n) {
  for (int done = 0; done < n;) {
    const int len = shapeSegment(n - done);
    if (bassActive_) shelf_.process(x + done, x + done, len);
    done += len;
  }
  if (value_[Dcblock] >= 0.5f) dcBlock(x, n);
}

void NamAmp::process(const float* in, float* out, int n) {
  if (!ready_) {
    if (in != out) copy(in, out, n);
    peak_ = 0.0f;
    return;
  }
  if (n > maxBlock_) return clear(out, n);
  for (int i = 0; i < n; ++i) scratch_[i] = in[i] * gain_[Input].next();
  float* ip = scratch_;
  model_->process(&ip, &out, n);
  for (int i = 0; i < n; ++i) out[i] *= gain_[Output].next();
  shape(out, n);
  float peak = 0.0f;
  for (int i = 0; i < n; ++i) peak = std::fmax(peak, std::fabs(out[i]));
  peak_ = peak;
}

void NamAmp::set(ParamId id, float value) {
  if (id >= kParamCount) return;
  value_[id] = kParams[id].clamp(value);
  if (id == Input) gain_[Input].setTarget(gainTarget(Input));
  if (id == Output || id == Normalize) gain_[Output].setTarget(gainTarget(Output));
  if (id == Bass) bass_.setTarget(value_[Bass]);
  if (id == Dcblock && value_[Dcblock] < 0.5f) dcX_ = dcY_ = 0.0;
}

float NamAmp::get(ParamId id) const {
  if (id < kParamCount) return value_[id];
  if (id == Out) return kParams[Out].clamp(peak_ > 0.0f ? 20.0f * std::log10(peak_) : kParams[Out].min);
  if (id == Loaded) return ready_ ? 1.0f : 0.0f;
  return NAN;
}

}  // namespace squatch::dsp

namespace squatch::clap {

bool PluginTraits<dsp::NamAmp>::load(const std::string& path, double rate, File& f, std::string& error) {
  f = File{};
  if (path.empty()) return true;
  try {
    std::unique_ptr<nam::DSP> model = nam::get_dsp(std::filesystem::path(path));
    if (!model) return error = path + ": not a NAM model", false;
    const double want = model->GetExpectedSampleRate();
    if (want > 0.0 && std::fabs(want - rate) > 1.0) {
      error = path + ": trained at " + std::to_string(static_cast<int>(want)) + " Hz, the host runs at " +
              std::to_string(static_cast<int>(rate)) + " Hz";
      return false;
    }
    f.makeupDb = model->HasLoudness() ? static_cast<float>(-18.0 - model->GetLoudness()) : 0.0f;
    f.model = std::move(model);
  } catch (const std::exception& e) {
    error = path + ": " + e.what();
    return false;
  }
  return true;
}

}  // namespace squatch::clap
