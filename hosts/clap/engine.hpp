// SPDX-License-Identifier: MIT
// The audio side of ClapPlugin<B>: a Bank is one prepared set of block instances with its own
// memory; the Engine runs the active bank over a buffer, with the bypass and the crossfade to a
// newly loaded bank. Everything after prepare() is real-time safe: no allocation, locks or I/O.
//
// Channels. A block that supports(Stereo) runs once, linked (gate, limiter, tuner); any other
// block runs as two instances, one per side (dual mono), when the configuration is stereo. Each
// call's channel counts come from the host's buffers, never from the configuration alone, so a
// host that hands over fewer channels than it configured (a mono track, Cubase's flush) is safe:
// missing inputs read silence, a missing right side is not written.
//
// Bypass (the contract's one Role::Bypass parameter, CLAP polarity: 1 is bypassed). The block
// keeps processing, and the output crossfades in 10 ms to the input delayed by the block's
// latency, so a bypassed plugin is sample-aligned with an engaged one and the switch is silent.
#pragma once
#include <algorithm>
#include <cstddef>
#include <memory>
#include <squatch/dsp/block.hpp>
#include <squatch/dsp/span.hpp>
#include <vector>

namespace squatch::clap {

constexpr int kChannels = 2;
constexpr float kBypassMs = 10.0f;
constexpr float kSwapMs = 20.0f;

/// One stretch of a host buffer: channel pointers (already offset) and how many of each.
struct Io {
  const float* const* in;
  int inCh;
  float* const* out;
  int outCh;
};

/// What activate() fixes: rate, the bound on frames per call, and the channel configuration.
struct Activation {
  double sampleRate = 48000.0;
  int maxFrames = 0;
  int inChannels = 2, outChannels = 2;
};

template <class B, class File>
struct Bank {
  B blocks[kChannels];
  int instances = 1;
  bool linked = false;  // one instance, prepared Stereo
  File file;            // what configure() read (the cabinet's response)
  std::unique_ptr<unsigned char[]> memory;

  /// Main or loader thread. False when the memory cannot be had.
  template <class Configure>
  bool prepare(const Activation& a, Configure&& configure) {
    const bool stereo = a.inChannels == 2 && a.outChannels == 2;
    linked = stereo && blocks[0].supports(dsp::Layout::Stereo);
    instances = stereo && !linked ? 2 : 1;
    const dsp::Setup setup{a.sampleRate, a.maxFrames, linked ? dsp::Layout::Stereo : dsp::Layout::Mono};
    const auto all = [&](dsp::Arena& mem) {
      for (int i = 0; i < instances; ++i) configure(blocks[i], file), blocks[i].prepare(setup, mem);
    };
    dsp::Arena measure;
    all(measure);
    const std::size_t bytes = measure.required() + 64 * dsp::Arena::kAlign;
    memory.reset(new (std::nothrow) unsigned char[bytes]);
    if (!memory) return false;
    dsp::Arena mem(memory.get(), bytes);
    all(mem);
    return !mem.failed();
  }

  void set(dsp::ParamId id, float v) {
    for (int i = 0; i < instances; ++i) blocks[i].set(id, v);
  }

  void run(const float* const* in, int inCh, float* const* out, int outCh, int n) {
    if (linked || outCh < 2) return blocks[0].processChannels(dsp::AudioIO{in, out, inCh, outCh}, n);
    blocks[0].process(in[0], out[0], n);
    if (instances == 2 && inCh == 2) return blocks[1].process(in[1], out[1], n);
    dsp::copy(out[0], out[1], n);
  }
};

/// A linear 0..1 ramp towards a target.
class Ramp {
 public:
  void prepare(double rate, float ms) { step_ = static_cast<float>(1.0 / std::max(1.0, ms * 1e-3 * rate)); }
  void jump(float v) { value_ = target_ = v; }
  void target(float v) { target_ = v; }
  bool moving() const { return value_ != target_; }
  float value() const { return value_; }
  float next() {
    value_ = value_ < target_ ? std::min(value_ + step_, target_) : std::max(value_ - step_, target_);
    return value_;
  }

 private:
  float value_ = 0.0f, target_ = 0.0f, step_ = 1.0f;
};

/// Adapter scratch and delay memory, taken once at activate().
class Engine {
 public:
  bool prepare(const Activation& a, int latency) {
    frames_ = a.maxFrames;
    latency_ = latency;
    storage_.assign(static_cast<std::size_t>(kChannels * (2 * frames_ + latency_) + frames_), 0.0f);
    float* at = storage_.data();
    for (int c = 0; c < kChannels; ++c) {
      dry_[c] = at, at += frames_;
      fade_[c] = at, at += frames_;
      ring_[c] = at, at += latency_;
    }
    zeros_ = at;
    ringPos_ = 0;
    bypass_.prepare(a.sampleRate, kBypassMs);
    swap_.prepare(a.sampleRate, kSwapMs);
    swap_.jump(1.0f);
    return true;
  }

  void reset() {
    std::fill(storage_.begin(), storage_.end(), 0.0f);
    ringPos_ = 0;
  }

  void bypass(bool on, bool instantly) {
    bypass_.target(on ? 1.0f : 0.0f);
    if (instantly) bypass_.jump(on ? 1.0f : 0.0f);
  }
  bool bypassed() const { return bypass_.value() >= 1.0f; }
  int maxFrames() const { return frames_; }

  /// Copies the inputs aside first, so in-place buffers are safe and the dry path is intact.
  /// in may hold fewer channels than out; nullptr channels read silence.
  template <class BankT>
  void run(BankT& active, BankT* fadingOut, const Io& io, int n) {
    const float* src[kChannels];
    capture(io.in, io.inCh, n, src);
    const Io dry{src, std::max(1, std::min(io.inCh, kChannels)), io.out, io.outCh};
    active.run(dry.in, dry.inCh, dry.out, dry.outCh, n);
    if (fadingOut) crossfade(*fadingOut, dry, n);
    mixDry(src, io.out, io.outCh, n);
  }

  /// Starts the crossfade from the old bank to the new one (audio thread).
  void startSwap() { swap_.jump(0.0f), swap_.target(1.0f); }
  bool swapping() const { return swap_.moving(); }

 private:
  // src[1] is the left side again when the host gave one input channel.
  void capture(const float* const* in, int inCh, int n, const float** src) {
    const float* left = inCh >= 1 && in && in[0] ? in[0] : zeros_;
    const float* right = inCh >= 2 && in && in[1] ? in[1] : nullptr;
    dsp::copy(left, dry_[0], n);
    if (right) dsp::copy(right, dry_[1], n);
    src[0] = dry_[0];
    src[1] = right ? dry_[1] : dry_[0];
  }

  template <class BankT>
  void crossfade(BankT& old, const Io& io, int n) {
    old.run(io.in, io.inCh, fade_, io.outCh, n);
    for (int i = 0; i < n; ++i) {
      const float t = swap_.next();
      for (int c = 0; c < io.outCh; ++c) io.out[c][i] = fade_[c][i] + t * (io.out[c][i] - fade_[c][i]);
    }
  }

  // The dry path is always delayed, so switching the bypass finds it already aligned.
  void mixDry(const float* const* src, float* const* out, int outCh, int n) {
    for (int i = 0; i < n; ++i) {
      const float m = bypass_.next();
      for (int c = 0; c < outCh; ++c) {
        float d = src[c][i];
        if (latency_ > 0) std::swap(d, ring_[c][ringPos_]);
        if (m > 0.0f) out[c][i] = m >= 1.0f ? d : out[c][i] + m * (d - out[c][i]);
      }
      if (latency_ > 0) ringPos_ = ringPos_ + 1 == latency_ ? 0 : ringPos_ + 1;
    }
  }

  std::vector<float> storage_;
  float* dry_[kChannels] = {};
  float* fade_[kChannels] = {};
  float* ring_[kChannels] = {};
  float* zeros_ = nullptr;
  int frames_ = 0, latency_ = 0, ringPos_ = 0;
  Ramp bypass_, swap_;
};

}  // namespace squatch::clap
