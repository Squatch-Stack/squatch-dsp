// SPDX-License-Identifier: MIT
// NamAmp: a NAM (.nam) amp model behind the Block interface, running NeuralAmpModelerCore.
//
// A stopgap for the live rig until our own lean engine exists (ROADMAP.md, milestone 3). It is a
// host-side adapter, not part of the library: NAM Core needs Eigen, allocates while it loads and
// uses exceptions, so it is only built when a checkout of it is named (make rig NAM_CORE=...).
// Without one, SQUATCH_WITH_NAM is undefined and the rig runs with the amp bypassed.
//
// Models swap while playing. Two voices (a model each) sit behind a ModelSwap: load() on a
// running amp builds the new model on the calling thread (the file read, the network, NAM's
// Reset() with its prewarm, all of which allocate), then hands it over; the audio thread warms
// it on the live input for the model's prewarm length and crossfades (see model_swap.hpp). Each
// voice carries its own loudness makeup, so the level is right through the fade. Before
// prepare() (or when the amp has no model yet) load() works as it always did.
//
// A NAM model answers silence with a constant (the two shipped examples: -58.5 and -63.6 dBFS), because it
// models an amp without the coupling capacitor that would block it. The cabinet after it blocks DC in
// steady state, so nothing is heard, but the constant comes and goes with the stage (a bypass fade, a model
// swap) and the step rings the cabinet and the EQ for 200 ms (-58 dBFS peak measured, with the input at
// exactly 0). So every voice ends in a 5 Hz DC blocker, as the NAM plugin's output does (its
// kDCBlockerFrequency, NeuralAmpModeler.cpp, issue 271), started in the state the model's own constant
// leaves it in, so there is no start-up step either. setDcBlock(false) is for the test that shows what
// the blocker prevents.
//
// load() is for the control thread, one caller at a time; process() never allocates or locks.
//
// The swap's plan is adapted from pedal-science/namchain by Rik Hemsley (MIT), pinned to e208bb8,
// licence in THIRD_PARTY_NOTICES.md (see model_swap.hpp):
//   https://github.com/pedal-science/namchain/blob/e208bb869069b15a7331b452e03b7dc88726ffee/docs/transitions.md
// Copyright (c) 2026 Rik Hemsley
#pragma once
#ifdef SQUATCH_WITH_NAM
#include <cmath>
#include <exception>
#include <filesystem>
#include <memory>
#include <squatch/dsp/block.hpp>
#include <squatch/dsp/blocks/model_swap.hpp>
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

  NamAmp() : swap_(voice_[0], voice_[1]) {
    for (int k = 0; k < kCount; ++k) set(static_cast<squatch::dsp::ParamId>(k), params()[k].def);
  }

  /// Empty on success, else what went wrong. `normalize` scales the output so the model's own
  /// loudness figure (when the file has one) lands at -18 dBFS, as NAM's plugin does. Before
  /// prepare() it sets the model; on a running amp it swaps to it without a click, and says so
  /// if a swap is already under way.
  std::string load(const char* path, bool normalize) {
    if (!prepared_) return voice_[0].load(path, normalize);
    squatch::dsp::Block* standby = swap_.standby();
    if (!standby) return "a model swap is already under way";
    Voice& v = *static_cast<Voice*>(standby);
    std::string err = v.load(path, normalize);
    if (err.empty()) err = v.settle(rate_, maxBlock_);
    if (err.empty() && v.expectedRate() > 0.0 && std::fabs(v.expectedRate() - rate_) > 0.5)
      err = "the model is for " + std::to_string(static_cast<int>(v.expectedRate())) + " Hz and the amp runs at " + std::to_string(static_cast<int>(rate_)) + " Hz";
    if (!err.empty()) return swap_.cancel(), err;
    newest_ = &v == &voice_[0] ? 0 : 1;
    if (!swap_.publish(v.warmFrames())) return swap_.cancel(), "the new model cannot replace the playing one (its delay differs)";
    return {};
  }

  /// Test hook (control thread, before playing): the DC blocker on every voice, on by default.
  void setDcBlock(bool on) { voice_[0].dcBlock = voice_[1].dcBlock = on; }

  double expectedRate() const { return voice_[newest_].expectedRate(); }
  float makeupDb() const { return voice_[newest_].makeup; }
  bool loaded() const { return voice_[0].loaded() || voice_[1].loaded(); }
  bool swapping() const { return swap_.swapping(); }

  void prepare(const squatch::dsp::Setup& s, squatch::dsp::Arena& mem) override {
    rate_ = s.sampleRate;
    maxBlock_ = s.maxBlock;
    for (int k = 0; k < kCount; ++k) smooth_[k].prepare(s.sampleRate, params()[k].smoothMs);
    scratch_ = mem.alloc<float>(static_cast<std::size_t>(s.maxBlock));
    swap_.prepare(s, mem);
    prepared_ = scratch_ && !mem.failed();
  }

  void process(const float* in, float* out, int n) override {
    if (!prepared_ || n > maxBlock_) return squatch::dsp::clear(out, n);
    for (int i = 0; i < n; ++i) scratch_[i] = in[i] * gain(Input);
    swap_.process(scratch_, out, n);
    for (int i = 0; i < n; ++i) out[i] *= gain(Output);
  }

  void reset() override { swap_.reset(); }

  void set(squatch::dsp::ParamId id, float value) override {
    if (id >= kCount) return;
    value_[id] = params()[id].clamp(value);
    smooth_[id].setTarget(squatch::dsp::dbToGain(value_[id]));
  }

  float get(squatch::dsp::ParamId id) const override { return id < kCount ? value_[id] : NAN; }

 private:
  // One model, as a block: NAM's network and its constant makeup gain.
  struct Voice final : squatch::dsp::Block {
    std::unique_ptr<nam::DSP> dsp;
    float* scratch = nullptr;
    float makeup = 0.0f;  // dB
    float gain = 1.0f;
    int maxBlock = 0;
    bool dcBlock = true;
    double dcR = 0.999, dcX1 = 0.0, dcY1 = 0.0;  // one-pole DC blocker y = x - x1 + R y1, and its state

    std::string load(const char* path, bool normalize) {
      std::unique_ptr<nam::DSP> next;
      try {
        next = nam::get_dsp(std::filesystem::path(path));
      } catch (const std::exception& e) {
        return e.what();
      }
      if (!next) return "the file did not give a model";
      makeup = normalize && next->HasLoudness() ? static_cast<float>(-18.0 - next->GetLoudness()) : 0.0f;
      gain = squatch::dsp::dbToGain(makeup);
      dsp = std::move(next);
      return {};
    }
    // NAM's Reset() allocates and prewarms: control thread only.
    std::string settle(double rate, int block) {
      try {
        dsp->SetPrewarmOnReset(true);
        dsp->Reset(rate, block);
      } catch (const std::exception& e) {
        dsp.reset();
        return e.what();
      }
      learnDc(rate, block);
      return {};
    }
    // The model's answer to silence, taken after its prewarm: the blocker starts settled on it.
    void learnDc(double rate, int block) {
      constexpr double kTwoPi = 6.283185307179586;
      dcR = std::exp(-kTwoPi * 5.0 / rate);
      float zeros[64] = {}, out[64] = {};
      float *ip = zeros, *op = out;
      const int n = block < 64 ? block : 64;
      dsp->process(&ip, &op, n);
      dcX1 = double(out[n - 1]) * gain, dcY1 = 0.0;
    }
    int warmFrames() const { return dsp ? dsp->GetPrewarmSamples() : 0; }
    double expectedRate() const { return dsp ? dsp->GetExpectedSampleRate() : 0.0; }
    bool loaded() const { return dsp != nullptr; }

    void prepare(const squatch::dsp::Setup& s, squatch::dsp::Arena& mem) override {
      scratch = mem.alloc<float>(static_cast<std::size_t>(s.maxBlock));
      maxBlock = s.maxBlock;
      if (dsp && scratch) settle(s.sampleRate, s.maxBlock);
    }
    void process(const float* in, float* out, int n) override {
      if (!dsp || !scratch || n > maxBlock) return squatch::dsp::clear(out, n);
      for (int i = 0; i < n; ++i) scratch[i] = in[i];
      float* ip = scratch;
      dsp->process(&ip, &out, n);
      for (int i = 0; i < n; ++i) out[i] *= gain;
      if (!dcBlock) return;
      for (int i = 0; i < n; ++i) {
        const double x = out[i], y = x - dcX1 + dcR * dcY1;
        dcX1 = x, dcY1 = std::fabs(y) < 1e-30 ? 0.0 : y;  // exact silence would decay it into denormals
        out[i] = static_cast<float>(y);
      }
    }
    void reset() override {}  // NAM's Reset() allocates and prewarms: setup time only
    void set(squatch::dsp::ParamId, float) override {}
    float get(squatch::dsp::ParamId) const override { return 0.0f; }
  };

  float gain(int k) { return smooth_[k].next(); }

  Voice voice_[2];
  squatch::dsp::ModelSwap swap_;
  float* scratch_ = nullptr;
  double rate_ = 48000.0;
  int maxBlock_ = 0;
  int newest_ = 0;
  bool prepared_ = false;
  float value_[kCount] = {};
  squatch::dsp::Smoother smooth_[kCount];
};

}  // namespace squatch::host
#endif  // SQUATCH_WITH_NAM
