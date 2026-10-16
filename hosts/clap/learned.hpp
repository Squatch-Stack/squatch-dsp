// SPDX-License-Identifier: MIT
// A block's learned state (Block v2: saveState(), loadState(), stateSerial()) across the plugin's
// threads, so something a block learned while audio ran (a noise profile) is saved with the
// plugin's state and restored with it.
//
// One blob per block instance (dual mono runs two). The audio thread owns the blocks; the main
// thread saves and loads states. Two lock-free hand-offs (dsp::Latest) connect them:
// - out: after a buffer in which an instance's stateSerial() moved, the audio thread copies every
//   instance's blob; the main thread takes the newest when it saves or activates.
// - in: a loaded state's blobs, which the audio thread adopts before its next buffer.
// Each hand-off carries the number of the state load it reflects, so a learn the audio thread
// copied out before it saw a newer load never overwrites that load on the main thread.
//
// A loaded state without learned state keeps the current one, as the LV2 plugin does: a preset
// saved before any learn does not make the block forget what it learned where it is. A block
// that learns nothing costs one virtual call per instance per buffer.
#pragma once
#include <algorithm>
#include <cstdint>
#include <squatch/dsp/block.hpp>
#include <squatch/dsp/latest.hpp>
#include <string>
#include <vector>

#include "engine.hpp"

namespace squatch::clap {

struct LearnedBlobs {
  int bytes[kChannels] = {};  // 0: nothing learned
  unsigned char data[kChannels][dsp::kMaxBlockState] = {};
  std::uint32_t load = 0;  // the state load these reflect
};

class Learned {
 public:
  /// Main thread: the blobs a save writes, by instance (an empty string: nothing learned).
  std::vector<std::string> saved() {
    absorb();
    std::vector<std::string> out;
    for (int i = 0; i < kChannels; ++i)
      out.emplace_back(reinterpret_cast<const char*>(main_.data[i]), static_cast<std::size_t>(main_.bytes[i]));
    return out;
  }

  /// Main thread: a loaded state's blobs (validated by the caller). None keeps the current state.
  /// While active the audio thread adopts them before its next buffer; otherwise activate() does.
  void load(const std::vector<std::string>& blobs, bool active) {
    bool any = false;
    for (const std::string& b : blobs) any = any || !b.empty();
    if (!any) return;
    main_ = LearnedBlobs{};
    for (std::size_t i = 0; i < blobs.size() && i < static_cast<std::size_t>(kChannels); ++i) {
      main_.bytes[i] = static_cast<int>(blobs[i].size());
      std::copy(blobs[i].begin(), blobs[i].end(), main_.data[i]);
    }
    main_.load = ++loads_;
    if (!active) return;
    in_.back() = main_;
    in_.publish();
  }

  /// Main thread, audio stopped (activate): the newest known state into the prepared blocks.
  template <class BankT>
  void activate(BankT& bank) {
    absorb();          // a learn from the previous activation
    in_.update();      // a hand-off the audio thread never took is already in main_
    adopt(bank, main_);
    applied_ = loads_;
  }

  /// Audio thread, before processing: a newly loaded state.
  template <class BankT>
  void receive(BankT& bank) {
    if (!in_.update()) return;
    adopt(bank, in_.front());
    applied_ = in_.front().load;
  }

  /// Audio thread, after processing: hands a changed state to the main thread.
  template <class BankT>
  void publish(BankT& bank) {
    bool moved = false;
    for (int i = 0; i < bank.instances; ++i) {
      const std::uint32_t s = bank.blocks[i].stateSerial();
      moved = moved || s != serial_[i];
      serial_[i] = s;
    }
    if (!moved) return;
    LearnedBlobs& b = out_.back();
    b.load = applied_;
    for (int i = 0; i < kChannels; ++i)
      b.bytes[i] = i < bank.instances ? bank.blocks[i].saveState(b.data[i], dsp::kMaxBlockState) : 0;
    out_.publish();
  }

 private:
  // An instance with no blob of its own takes the first one (a mono state in a stereo layout).
  template <class BankT>
  void adopt(BankT& bank, const LearnedBlobs& from) {
    for (int i = 0; i < bank.instances; ++i) {
      const int k = from.bytes[i] > 0 ? i : 0;
      if (from.bytes[k] > 0) bank.blocks[i].loadState(from.data[k], from.bytes[k]);
      serial_[i] = bank.blocks[i].stateSerial();
    }
  }

  // Main thread: the audio thread's newest copy, unless a later load has replaced it.
  void absorb() {
    if (out_.update() && out_.front().load == loads_) main_ = out_.front();
  }

  dsp::Latest<LearnedBlobs> out_, in_;
  LearnedBlobs main_;                     // main thread
  std::uint32_t loads_ = 0;               // main thread
  std::uint32_t applied_ = 0;             // audio thread (main thread while inactive)
  std::uint32_t serial_[kChannels] = {};  // audio thread (main thread while inactive)
};

}  // namespace squatch::clap
