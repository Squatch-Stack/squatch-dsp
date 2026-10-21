// SPDX-License-Identifier: MIT
// CabSwap: the rig's cabinet stage with a response that can change while audio runs. Two Cabinets
// sit behind a ModelSwap, so a new impulse response is built on the calling thread, warmed on the
// live input for the length of the response, and crossfaded in (ModelSwap's plan, adapted from
// pedal-science/namchain by Rik Hemsley (MIT), pinned to e208bb8, licence in THIRD_PARTY_NOTICES.md:
//   https://github.com/pedal-science/namchain/blob/e208bb869069b15a7331b452e03b7dc88726ffee/docs/transitions.md
// Copyright (c) 2026 Rik Hemsley). The host adapter for Cabinet that NamAmp is for NAM models.
//
// load() is for the control thread, one caller at a time. It allocates (the standby's tables come
// from a buffer of its own, sized by a measuring arena); process() never allocates or locks.
// Before prepare() configure() sets the first response, as on a Cabinet. Each Cabinet reads its
// response again whenever it is prepared, so CabSwap keeps its own copy of every response it is
// given (made on the control thread): the caller's buffer may go as soon as the call returns.
// kMaxTaps bounds the one thing that grows with the response on the audio thread, the reset
// Bypass makes on every switch on (about 70 us at the bound, measured on the Mac at -O3).
#pragma once
#include <memory>
#include <squatch/dsp/block.hpp>
#include <squatch/dsp/blocks/cabinet.hpp>
#include <squatch/dsp/blocks/model_swap.hpp>
#include <string>
#include <vector>

namespace squatch::host {

class CabSwap final : public squatch::dsp::Block {
 public:
  static constexpr int kMaxTaps = 1 << 18;
  enum Param : squatch::dsp::ParamId {
    Level = squatch::dsp::Cabinet::Level,
    Lowcut = squatch::dsp::Cabinet::Lowcut,
    Highcut = squatch::dsp::Cabinet::Highcut,
    kCount = squatch::dsp::Cabinet::kCount
  };
  static squatch::dsp::Span<const squatch::dsp::ParamDesc> params() { return squatch::dsp::Cabinet::params(); }

  CabSwap() : swap_(a_, b_, 0) {}

  /// Setup time, before prepare(): the first response (copied, so `ir` need not outlive the call).
  /// `cuts` is Cabinet's: false leaves out the Low cut and High cut filters, for every response loaded later too.
  void configure(const float* ir, int length, bool cuts = true) {
    cuts_ = cuts;
    taps_[0].assign(ir, ir + (ir && length > 0 ? length : 0));
    a_.configure(taps_[0].empty() ? nullptr : taps_[0].data(), static_cast<int>(taps_[0].size()), squatch::dsp::Cabinet::kDefaultPartition, cuts_);
  }

  /// Empty on success, else what went wrong. Control thread. `taps` is copied; the caller may free it on return.
  std::string load(const float* taps, int length) {
    if (!prepared_) return "the cabinet is not prepared yet";
    if (!taps || length < 1 || length > kMaxTaps) return "the response has no taps, or too many";
    squatch::dsp::Block* standby = swap_.standby();
    if (!standby) return "a cabinet swap is already under way";
    auto& cab = *static_cast<squatch::dsp::Cabinet*>(standby);
    const int which = &cab == &a_ ? 0 : 1;
    taps_[which].assign(taps, taps + length);  // the standby is ours: nothing plays from its old copy
    cab.configure(taps_[which].data(), length, squatch::dsp::Cabinet::kDefaultPartition, cuts_);
    squatch::dsp::Arena measure;
    cab.prepare(setup_, measure);
    const std::size_t bytes = measure.required() + 16 * squatch::dsp::Arena::kAlign;
    std::unique_ptr<unsigned char[]> memory(new unsigned char[bytes]);
    squatch::dsp::Arena mem(memory.get(), bytes);
    cab.prepare(setup_, mem);
    if (mem.failed() || !cab.ready()) return swap_.cancel(), "the response did not fit its memory";
    // The standby's old tables are replaced by prepare() above, so its old buffer can go now.
    memory_[which] = std::move(memory);
    // Warm for the response's own length: the standby then plays what it should.
    if (!swap_.publish(length)) return swap_.cancel(), "the new response cannot replace the playing one (its delay differs)";
    return {};
  }

  bool ready() const { return prepared_; }
  bool swapping() const { return swap_.swapping(); }
  long swaps() const { return swap_.swaps(); }
  /// Taps in the response playing now (0 while it is a pass-through).
  int length() const { return (swap_.playing() == 0 ? a_ : b_).length(); }

  void prepare(const squatch::dsp::Setup& s, squatch::dsp::Arena& mem) override {
    setup_ = s;
    swap_.prepare(s, mem);
    prepared_ = !mem.failed();
  }
  void process(const float* in, float* out, int n) override { swap_.process(in, out, n); }
  void reset() override { swap_.reset(); }
  void set(squatch::dsp::ParamId id, float value) override { swap_.set(id, value); }
  float get(squatch::dsp::ParamId id) const override { return swap_.get(id); }
  int latency() const override { return swap_.latency(); }

 private:
  squatch::dsp::Cabinet a_, b_;
  squatch::dsp::ModelSwap swap_;
  squatch::dsp::Setup setup_;
  std::unique_ptr<unsigned char[]> memory_[2];
  std::vector<float> taps_[2];  // what a_ and b_ read their responses from
  bool prepared_ = false;
  bool cuts_ = true;
};

}  // namespace squatch::host
