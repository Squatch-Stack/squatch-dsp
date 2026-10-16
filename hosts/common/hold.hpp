// SPDX-License-Identifier: MIT
// What a display holds on screen after the tuner lets go of a note: hosts/web/hold.js in C++,
// for displays that cannot run the page (a pedal's own screen, the ESP32-S3 tuner probe, the
// offline CSV harness). Same constants and the same rules, on the same audio clock, so every host
// shows the same thing; hold.js's header states the rule and hold_parity.mjs checks it.
//
// The last valid reading is the worst one to keep: the core withholds a reading once the note has
// sunk toward the room, so the frame just before that sits right at the threshold. Instead the
// display keeps the last half second of confident readings on the same note and holds their
// median. If they are too few, span too little time or disagree, it holds the note name only and
// the number is NaN (the page shows --.-), so a one-frame blip is never presented as a measurement.
//
// Time is the reading's position in the audio (`samples` at `rate`); an age is
// 1000 * (newer - older) / rate ms from the exact integer difference, as hold.js computes it.
//
// Fixed storage, no allocation: at 30 readings a second the 500 ms window holds 16 at most. It keeps
// 64, so it behaves as hold.js up to 128 readings a second; beyond that the oldest are dropped first.
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace squatch::host {

struct HoldReading {
  int midi = 0;
  float cents = 0.0f;         // steady cents against the note (DisplayReading::cents)
  float unc = 0.0f;           // the core's uncertainty of those cents
  bool valid = true;          // the tuner reads (an invalid reading still moves the clock)
  std::uint64_t samples = 0;  // where in the audio the reading was taken
  double rate = 48000.0;      // of that audio
};

class Hold {
 public:
  static constexpr double kWindowMs = 500.0;   // readings older than this are forgotten
  static constexpr double kMinSpanMs = 250.0;  // a held value must rest on at least this much of the note
  static constexpr float kMaxUnc = 0.5f;       // cents: only confident readings count
  static constexpr float kMaxSpread = 2.0f;    // cents: the kept readings must agree this well
  static constexpr double kHoldMs = 1500.0;    // a let-go note stays on screen this long
  static constexpr int kCapacity = 64;

  /// Start afresh: nothing kept, nothing seen.
  void reset() {
    count_ = 0;
    seen_ = haveValid_ = false;
  }

  /// Every reading as it arrives, valid or not (hold.js add()).
  void add(const HoldReading& r) {
    see(r);
    if (!r.valid || (haveValid_ && r.samples == lastValid_)) return;
    haveValid_ = true;
    lastValid_ = r.samples;
    if (count_ > 0 && at(count_ - 1).r.midi != r.midi) count_ = 0;
    if (r.unc <= kMaxUnc) push(r);
    while (count_ > 0 && ageMs(r.samples, at(0).r.samples) > kWindowMs) drop();
  }

  /// Whether a let-go note is still on screen when the newest reading is at `samples`.
  bool holding(std::uint64_t samples) const { return haveValid_ && ageMs(samples, lastValid_) < kHoldMs; }

  /// The cents to hold for `last` (the newest valid reading): the median of the kept readings on
  /// its note, or NaN when they are too few, too short or disagree. In double, as hold.js computes
  /// it: the mean of the middle two of an even count is then exact, so the page and this round the
  /// same number (tuner_frames.hpp roundTenth).
  double held(int midi) const {
    std::array<float, kCapacity> cents{};
    int n = 0;
    std::uint64_t first = 0, lastS = 0;
    for (int i = 0; i < count_; ++i) {
      if (at(i).r.midi != midi) continue;
      if (n == 0) first = at(i).r.samples;
      lastS = at(i).r.samples;
      cents[static_cast<std::size_t>(n++)] = at(i).r.cents;
    }
    std::sort(cents.begin(), cents.begin() + n);
    const bool agree = n > 0 && cents[static_cast<std::size_t>(n - 1)] - cents[0] <= kMaxSpread;
    if ((n ? ageMs(lastS, first) : 0.0) < kMinSpanMs || !agree) return std::nan("");
    const auto mid = static_cast<std::size_t>(n / 2);
    return n % 2 ? static_cast<double>(cents[mid]) : 0.5 * (static_cast<double>(cents[mid - 1]) + static_cast<double>(cents[mid]));
  }

 private:
  struct Entry {
    HoldReading r{};
  };
  // The clock: an earlier position than the newest seen means the source restarted.
  void see(const HoldReading& r) {
    if (seen_ && r.samples < newest_) reset();
    seen_ = true;
    newest_ = r.samples;
    rate_ = r.rate;
  }
  // ms from `older` to `newer`: the integer difference first (exact), as hold.js does. A position
  // before `older` (asked before add() has seen the restart) is a negative age, as in hold.js.
  double ageMs(std::uint64_t newer, std::uint64_t older) const {
    if (newer < older) return -1.0;
    return 1000.0 * static_cast<double>(newer - older) / rate_;
  }
  const Entry& at(int i) const { return ring_[static_cast<std::size_t>((head_ + i) % kCapacity)]; }
  void push(const HoldReading& r) {
    if (count_ == kCapacity) drop();
    ring_[static_cast<std::size_t>((head_ + count_) % kCapacity)] = Entry{r};
    ++count_;
  }
  void drop() {
    head_ = (head_ + 1) % kCapacity;
    --count_;
  }

  std::array<Entry, kCapacity> ring_{};
  int head_ = 0, count_ = 0;
  std::uint64_t newest_ = 0, lastValid_ = 0;
  double rate_ = 48000.0;
  bool seen_ = false, haveValid_ = false;
};

}  // namespace squatch::host
