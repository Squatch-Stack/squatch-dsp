// SPDX-License-Identifier: MIT
// What ClapPlugin<B> needs to know about a block beyond the Block interface: whether it reads a
// file named in the saved state, and how. Most blocks read none.
#pragma once
#include <squatch/dsp/blocks/cabinet.hpp>
#include <string>
#include <utility>
#include <vector>

#include "../common/ir_file.hpp"

namespace squatch::clap {

template <class B>
struct PluginTraits {
  static constexpr const char* kFileKey = nullptr;  // the state's key for the file, or none
  struct File {};
  /// Loader or main thread: reads `path` for a host at `rate` into f. False (and why) on failure.
  static bool load(const std::string&, double, File&, std::string&) { return true; }
  /// Before prepare(): hands the file to the block, which copies what it needs in prepare().
  static void configure(B&, const File&) {}
};

/// The cabinet's impulse response: a WAV file, resampled to the host's rate and cut to 500 ms,
/// by the same loader the Pi rig uses. No file, or one that fails to load, passes audio through.
template <>
struct PluginTraits<dsp::Cabinet> {
  static constexpr const char* kFileKey = "ir";
  static constexpr double kMaxMs = 500.0;
  struct File {
    std::vector<float> taps;
  };

  static bool load(const std::string& path, double rate, File& f, std::string& error) {
    f.taps.clear();
    if (path.empty()) return true;
    host::Response r = host::loadResponse(path.c_str(), rate, kMaxMs, true);
    if (!r.error.empty()) return error = path + ": " + r.error, false;
    f.taps = std::move(r.taps);
    return true;
  }

  static void configure(dsp::Cabinet& b, const File& f) {
    b.configure(f.taps.empty() ? nullptr : f.taps.data(), static_cast<int>(f.taps.size()));
  }
};

}  // namespace squatch::clap
