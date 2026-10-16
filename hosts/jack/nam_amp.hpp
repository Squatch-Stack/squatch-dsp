// SPDX-License-Identifier: MIT
// NamAmp: a NAM (.nam) amp model behind the Block interface, running NeuralAmpModelerCore.
//
// A stopgap for the live rig until our own lean engine exists (ROADMAP.md, milestone 3). It is a
// host-side adapter, not part of the library: NAM Core needs Eigen, allocates while it loads and
// uses exceptions, so it is only built when a checkout of it is named (make rig NAM_CORE=...).
// Without one, SQUATCH_WITH_NAM is undefined and the rig runs with the amp bypassed.
//
// load() and prepare() run before the JACK client is activated. load() reads the file and builds
// the network; prepare() calls NAM's Reset() with the period as its maximum buffer, which sizes
// every buffer and prewarms the model, so process() does not allocate.
#pragma once
#ifdef SQUATCH_WITH_NAM
#include <cmath>
#include <exception>
#include <filesystem>
#include <memory>
#include <squatch/dsp/block.hpp>
#include <squatch/dsp/param.hpp>
#include <string>

#include "NAM/dsp.h"
#include "NAM/get_dsp.h"

namespace squatch::host {

class NamAmp final : public squatch::dsp::Block {
 public:
  enum Param : squatch::dsp::ParamId { Input, Output, kCount };
  static squatch::dsp::Span<const squatch::dsp::ParamDesc> params() {
    static const squatch::dsp::ParamDesc kParams[] = {
        {Input, "Input", "dB", -24.0f, 24.0f, 0.0f, 20.0f},
        {Output, "Output", "dB", -40.0f, 12.0f, 0.0f, 20.0f},
    };
    return kParams;
  }

  NamAmp() {
    for (int k = 0; k < kCount; ++k) set(static_cast<squatch::dsp::ParamId>(k), params()[k].def);
  }

  /// Setup time. Empty on success, else what went wrong. `normalize` scales the output so the
  /// model's own loudness figure (when the file has one) lands at -18 dBFS, as NAM's plugin does.
  std::string load(const char* path, bool normalize) {
    try {
      dsp_ = nam::get_dsp(std::filesystem::path(path));
    } catch (const std::exception& e) {
      dsp_.reset();
      return e.what();
    }
    if (!dsp_) return "the file did not give a model";
    makeup_ = normalize && dsp_->HasLoudness() ? static_cast<float>(-18.0 - dsp_->GetLoudness()) : 0.0f;
    set(Output, value_[Output]);
    return {};
  }

  double expectedRate() const { return dsp_ ? dsp_->GetExpectedSampleRate() : 0.0; }
  float makeupDb() const { return makeup_; }
  bool loaded() const { return dsp_ != nullptr; }

  void prepare(const squatch::dsp::Setup& s, squatch::dsp::Arena& mem) override {
    scratch_ = mem.alloc<float>(static_cast<std::size_t>(s.maxBlock));
    maxBlock_ = s.maxBlock;
    for (int k = 0; k < kCount; ++k) smooth_[k].prepare(s.sampleRate, params()[k].smoothMs);
    if (!dsp_ || !scratch_) return;
    dsp_->SetPrewarmOnReset(true);
    dsp_->Reset(s.sampleRate, s.maxBlock);
  }

  void process(const float* in, float* out, int n) override {
    if (!dsp_ || !scratch_ || n > maxBlock_) return squatch::dsp::clear(out, n);
    for (int i = 0; i < n; ++i) scratch_[i] = in[i] * gain(Input);
    float* ip = scratch_;
    dsp_->process(&ip, &out, n);
    for (int i = 0; i < n; ++i) out[i] *= gain(Output);
  }

  void reset() override {}  // NAM's Reset() allocates and prewarms: setup time only

  void set(squatch::dsp::ParamId id, float value) override {
    if (id >= kCount) return;
    value_[id] = params()[id].clamp(value);
    smooth_[id].setTarget(squatch::dsp::dbToGain(value_[id] + (id == Output ? makeup_ : 0.0f)));
  }

  float get(squatch::dsp::ParamId id) const override { return id < kCount ? value_[id] : NAN; }

 private:
  float gain(int k) { return smooth_[k].next(); }

  std::unique_ptr<nam::DSP> dsp_;
  float* scratch_ = nullptr;
  int maxBlock_ = 0;
  float makeup_ = 0.0f;
  float value_[kCount] = {};
  squatch::dsp::Smoother smooth_[kCount];
};

}  // namespace squatch::host
#endif  // SQUATCH_WITH_NAM
