// SPDX-License-Identifier: MIT
// The overdrive (a Faust model, compiled to drive_faust.hpp) behind the Block
// interface, for the Seed3 chain. Header-only.
//
// The Faust object is a member: all its state is fixed-size arrays inside the class, so the
// block lives wherever it is declared (static storage on the board) and takes nothing from the
// arena. Drive, Tone and Level are the .dsp's own 0..1 sliders; Faust smooths them internally
// (si.smoo), so set() only clamps and writes. Never copy or clone the Faust object: its clone()
// allocated, and the generator removes it.
#pragma once
#include <cmath>
#include <cstring>
#include <squatch/dsp/block.hpp>
#include <squatch/dsp/param.hpp>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include "drive_faust.hpp"
#pragma GCC diagnostic pop

namespace squatch::board {

class DriveStage final : public dsp::Block {
 public:
  enum Param : dsp::ParamId { Drive, Tone, Level, kCount };

  static dsp::Span<const dsp::ParamDesc> params() {
    static const dsp::ParamDesc kParams[] = {
        {Drive, "Drive", "", 0.0f, 1.0f, 1.0f, 0.0f},
        {Tone, "Tone", "", 0.0f, 1.0f, 0.5f, 0.0f},
        {Level, "Level", "", 0.0f, 1.0f, 0.5f, 0.0f},
    };
    return kParams;
  }

  void prepare(const dsp::Setup& s, dsp::Arena&) override {
    dsp_.init(static_cast<int>(s.sampleRate + 0.5));
    Binder b{zone_};
    dsp_.buildUserInterface(&b);
    ready_ = zone_[Drive] && zone_[Tone] && zone_[Level];
    for (const dsp::ParamDesc& d : params()) set(d.id, value_[d.id]);
    dsp_.instanceClear();
  }

  void process(const float* in, float* out, int n) override {
    if (!ready_) return dsp::clear(out, n);
    float* ins[1] = {const_cast<float*>(in)};  // Faust's signature; it only reads the input
    float* outs[1] = {out};
    dsp_.compute(n, ins, outs);
  }

  void reset() override { dsp_.instanceClear(); }

  void set(dsp::ParamId id, float value) override {
    const dsp::ParamDesc* d = dsp::findParam(params(), id);
    if (!d) return;
    value_[id] = d->clamp(value);
    if (zone_[id]) *zone_[id] = value_[id];
  }

  float get(dsp::ParamId id) const override { return id < kCount ? value_[id] : NAN; }

  /// True once prepare() found all three sliders in the Faust UI.
  bool ready() const { return ready_; }

 private:
  // Receives Faust's UI description and keeps a pointer to each slider, matched by label.
  struct Binder {
    float** zones;
    void openVerticalBox(const char*) {}
    void closeBox() {}
    void declare(float*, const char*, const char*) {}
    template <class... InitMinMaxStep>
    void addHorizontalSlider(const char* label, float* zone, InitMinMaxStep...) {
      const char* names[kCount] = {"Drive", "Tone", "Level"};
      for (int i = 0; i < kCount; ++i)
        if (std::strcmp(label, names[i]) == 0) zones[i] = zone;
    }
  };

  squatch::faust::DriveFaust dsp_;
  float* zone_[kCount] = {};
  float value_[kCount] = {1.0f, 0.5f, 0.5f};
  bool ready_ = false;
};

}  // namespace squatch::board
