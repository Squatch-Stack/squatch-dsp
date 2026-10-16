// SPDX-License-Identifier: MIT
// Squatch NAM: a Neural Amp Modeler (.nam) model behind the Block interface, run by
// NeuralAmpModelerCore (pass its checkout as NAM_CORE), for the plugin adapter (hosts/clap).
//
// A host-side block, like the Pi rig's NamAmp (hosts/jack/nam_amp.hpp): NAM Core needs Eigen,
// exceptions and heap allocation while it loads, so it is never part of the library. This
// header names no NAM type; nam_block.cpp holds everything that touches NAM Core.
//
// The model is the plugin's file ("file model <path>" in its saved state), loaded by the adapter
// off the audio thread, a second bank prepared and crossfaded in, as the cabinet's response is.
// prepare() calls NAM's Reset() with the host's largest call, which sizes and prewarms every
// buffer, so process() never allocates; a call larger than that (never, from the adapter) is
// silenced rather than handed to NAM, which would overrun its buffers (NeuralAmpModelerCore
// issue #303). With no model the block passes its input through. The model's own loudness
// figure, when it has one, is brought to -18 dBFS, as NAM's plugin does, before Output.
//
// The signal path is the browser rig's NAM head (squatch-rig web/worklets/dsp/namhead.js), so a
// patch moves between the two: input gain, model, output gain (with Normalize), the Bass low shelf
// (160 Hz, Q 0.7071, 0 to -12 dB) and a 10 Hz DC block, last. Normalize and the DC block are live
// switches (the head's Norm and DC block); Bass glides over 12 ms as the head's does. The head
// de-zippers its output gain with a one-pole, this block with a 20 ms ramp: the same once settled.
#pragma once
#include <memory>
#include <squatch/dsp/block.hpp>
#include <squatch/dsp/blocks/biquad.hpp>
#include <squatch/dsp/contracts/nam_amp.hpp>
#include <squatch/dsp/param.hpp>
#include <string>

namespace nam {
class DSP;
}

namespace squatch::dsp {

class NamAmp final : public Block, public contracts::NamAmpContract {
 public:
  NamAmp();
  /// Setup time, before prepare(): the loaded model (not owned; null for none) and its
  /// loudness make-up in dB.
  void use(nam::DSP* model, float makeupDb);

  void prepare(const Setup& s, Arena& mem) override;
  void process(const float* in, float* out, int n) override;
  void reset() override {}  // NAM's Reset() allocates and prewarms: setup time only
  void set(ParamId id, float value) override;
  float get(ParamId id) const override;
  /// One model instance: a stereo host runs it linked, the mono result on every channel.
  bool supports(Layout) const override { return true; }
  Span<const ParamDesc> parameters() const override { return params(); }
  const BlockInfo& info() const override { return kInfo; }

 private:
  float gainTarget(int k) const;
  int shapeSegment(int left);  // advances the Bass glide, redesigns the shelf; samples to run
  void designBass();
  void shape(float* x, int n);  // Bass, then the DC block, in place
  void dcBlock(float* x, int n);

  nam::DSP* model_ = nullptr;
  float makeup_ = 0.0f;
  float* scratch_ = nullptr;
  int maxBlock_ = 0;
  bool ready_ = false;
  float value_[kParamCount] = {};
  Smoother gain_[2];  // Input, Output
  Smoother bass_;
  Biquad shelf_;
  bool bassActive_ = false;
  double rate_ = 48000.0, dcR_ = 1.0, dcX_ = 0.0, dcY_ = 0.0;
  float peak_ = 0.0f;
};

}  // namespace squatch::dsp

namespace squatch::clap {

template <class B>
struct PluginTraits;

/// The model file: read and built by NAM Core on the loader thread; each bank holds its own.
template <>
struct PluginTraits<dsp::NamAmp> {
  static constexpr const char* kFileKey = "model";
  struct File {
    std::shared_ptr<nam::DSP> model;
    float makeupDb = 0.0f;
  };
  /// False (and why) for a file NAM cannot read, or one trained at another sample rate.
  static bool load(const std::string& path, double rate, File& f, std::string& error);
  static void configure(dsp::NamAmp& b, const File& f) { b.use(f.model.get(), f.makeupDb); }
};

}  // namespace squatch::clap
