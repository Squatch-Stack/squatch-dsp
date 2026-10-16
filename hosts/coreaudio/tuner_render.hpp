// SPDX-License-Identifier: MIT
// The CoreAudio host's audio callback body, with no CoreAudio types: pick one input channel out of
// the device's buffers, run Core::processBlock on it, and write the outputs the way --out says
// (none and mute: silence; thru: the input on outputs 1 and 2). The IOProc in
// tuner_live.cpp is a thin wrapper that turns an AudioBufferList into Chunks; the offline test calls
// render() directly with test buffers, so the same code that runs on the device is the code tested.
// Real-time safe: static scratch, no allocation, no locks.
#pragma once
#include <array>

#include "../common/tuner_host.hpp"
#include "audio_chunks.hpp"

namespace squatch::host::live {

class TunerRender {
 public:
  TunerRender(Core& core, uint32_t channel, OutMode mode, double rate)
      : core_(core), channel_(channel), mode_(mode), nsPerFrame_(1e9 / rate) {}

  /// `firstFrameNs`: the host time of the first input frame, in the backend's clock; each block is stamped
  /// with the time of its last frame. `in` or `out` may be empty (an output-only or input-only device).
  void render(audio::Chunks in, audio::Chunks out, uint64_t firstFrameNs) {
    audio::clearAll(out);  // the HAL does not promise zeroed output buffers
    const uint32_t frames = audio::framesOf(in);
    for (uint32_t done = 0; done < frames;) {
      const uint32_t n = audio::gatherChannel(in, channel_, done, scratch_.data(), static_cast<uint32_t>(scratch_.size()));
      const auto stamp = firstFrameNs + static_cast<uint64_t>(static_cast<double>(done + n) * nsPerFrame_);
      core_.processBlock(scratch_.data(), static_cast<int>(n), stamp);
      if (mode_ == OutMode::Thru) audio::scatterMono(out, 2, done, scratch_.data(), n);
      done += n;
    }
  }

 private:
  Core& core_;
  uint32_t channel_;  // 0-based
  OutMode mode_;
  double nsPerFrame_;
  std::array<float, 2048> scratch_{};
};

}  // namespace squatch::host::live
