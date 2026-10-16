// SPDX-License-Identifier: MIT
// The tuner's display readings at 30 frames a second of AUDIO time, as one CSV row each: the
// contract every platform emits for the tuner quality suite (blocks/tuner/bench/quality), so two
// platforms' rows over the same recording can be diffed line by line:
//
//   t_ms,note,octave,cents,hz,valid,shown_cents
//
//   t_ms         the frame's audio position, ms from the start (three decimals): frame k is taken
//                after exactly floor(k * rate / 30) samples, or at the positions schedule() names
//   note,octave  the note on screen, live or held after the tuner lets go; empty when none is
//   cents        the live reading's steady cents at the chosen A4, empty when the tuner is not reading
//   hz           the live steadyHz (the number's source), empty when not reading
//   valid        1 while the tuner reads, else 0
//   shown_cents  the number on screen to 0.1 c: the live reading, or for 1.5 s of audio after the
//                tuner lets go the hold's median (hold.hpp); empty for the page's --.- or no note
//
// The display rules are the web page's (tuner-view.js, hold.js) and the phone app's. Frames are
// placed by sample count, never by a clock, and the tuner's reading does not depend on how its
// input was split into blocks, so a host's block size cannot change the rows.
#pragma once
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "display.hpp"
#include "hold.hpp"

namespace squatch::host {

struct FrameRow {
  enum class Shown { None, Number, NoNumber };
  double tMs = 0.0;
  DisplayReading d{};       // the live reading (d.valid: the tuner reads now)
  NoteName note{};          // the note on screen, live or held (when shown != None)
  Shown shown = Shown::None;
  float shownCents = 0.0f;  // rounded to 0.1 c when shown == Number
};

inline constexpr const char* kFrameCsvHeader = "t_ms,note,octave,cents,hz,valid,shown_cents";

/// The number as every display prints it, one rule for all: the page's formatCents (tuner-view.js,
/// also the phone's screen), Math.round(10 c) / 10 on the reading's full-precision value. Halves
/// go up, towards +inf (-4.25 shows -4.2, 4.25 shows 4.3), and -0.0 prints as 0.0. Computed in
/// double, as JavaScript does, with 10 c rounded before the half is added: a compiler may fuse the
/// two into one multiply-add (clang does; g++ does across statements), which JavaScript never does,
/// hence the volatile. Then floor(10 c + 0.5) is exactly Math.round. (Rounding half away from zero
/// here, and a page rounding a value already printed to three decimals, showed 21 of 5,730 Pi
/// frames 0.1 c apart.)
inline float roundTenth(double c) {
  const volatile double tenths = c * 10.0;
  const double v = std::floor(tenths + 0.5) / 10.0;
  return v == 0.0 ? 0.0f : static_cast<float>(v);
}

class TunerFrames {
 public:
  static constexpr int kFramesPerSecond = 30;

  void reset(std::uint32_t sampleRate, float a4) {
    rate_ = sampleRate;
    a4_ = a4;
    samples_ = 0;
    index_ = 1;
    last_ = DisplayReading{};
    hold_.reset();
    at_ = nullptr;
    atCount_ = 0;
  }

  /// After reset(): take the frames at these sample positions (ascending; the array must outlive
  /// the run) instead of 30 a second, e.g. exactly where another platform read its display.
  void schedule(const std::uint64_t* positions, std::size_t count) {
    at_ = positions;
    atCount_ = count;
  }

  /// Samples until the next frame is due (at least 1).
  std::uint64_t untilFrame() const { return boundary(index_) - samples_; }
  /// `n` samples (no more than untilFrame()) went into the tuner.
  void advance(std::uint64_t n) { samples_ += n; }
  bool due() const { return samples_ >= boundary(index_); }
  std::uint64_t samples() const { return samples_; }
  std::uint64_t frames() const { return index_ - 1; }

  /// The row for the frame that is due, from the tuner's newest reading. The reading was taken
  /// after samples() samples: that is its audio position, the clock the hold runs on (hold.hpp).
  FrameRow take(const tuner::TunerReading& r) {
    FrameRow row;
    row.tMs = static_cast<double>(samples_) * 1000.0 / static_cast<double>(rate_);
    row.d = toDisplay(r, a4_);
    ++index_;
    hold_.add({row.d.midi, row.d.cents, r.uncertainty, row.d.valid, samples_, static_cast<double>(rate_)});
    if (row.d.valid) {
      last_ = row.d;
      return shown(row, row.d.cents);
    }
    if (last_.valid && hold_.holding(samples_)) return shown(row, hold_.held(last_.midi));
    return row;
  }

 private:
  std::uint64_t boundary(std::uint64_t k) const {
    if (!at_) return k * rate_ / kFramesPerSecond;
    return k - 1 < atCount_ ? at_[k - 1] : ~std::uint64_t{0};
  }
  FrameRow shown(FrameRow row, double cents) const {
    row.note = last_.note;
    row.shown = std::isnan(cents) ? FrameRow::Shown::NoNumber : FrameRow::Shown::Number;
    row.shownCents = std::isnan(cents) ? 0.0f : roundTenth(cents);
    return row;
  }

  std::uint64_t rate_ = 48000, samples_ = 0, index_ = 1;
  float a4_ = 440.0f;
  DisplayReading last_{};  // the newest valid reading: the note a hold shows
  Hold hold_;
  const std::uint64_t* at_ = nullptr;
  std::size_t atCount_ = 0;
};

/// One CSV row (no newline) into `buf`; returns the length, as snprintf does.
inline int formatFrameCsv(char* buf, std::size_t cap, const FrameRow& row) {
  char note[16] = ",", live[48] = ",,0", shown[16] = "";
  if (row.shown != FrameRow::Shown::None) std::snprintf(note, sizeof note, "%s,%d", row.note.name, row.note.octave);
  if (row.d.valid)
    std::snprintf(live, sizeof live, "%.4f,%.4f,1", static_cast<double>(row.d.cents), static_cast<double>(row.d.steadyHz));
  if (row.shown == FrameRow::Shown::Number) std::snprintf(shown, sizeof shown, "%.1f", static_cast<double>(row.shownCents));
  return std::snprintf(buf, cap, "%.3f,%s,%s,%s", row.tMs, note, live, shown);
}

/// FNV-1a over a frame's raw reading (validity, midi, and the bits of cents, hz and steadyHz):
/// two hosts with equal hashes gave bit-identical readings, beyond the CSV's printed precision.
inline std::uint32_t hashFrame(std::uint32_t h, const FrameRow& row) {
  const float f[3] = {row.d.cents, row.d.hz, row.d.steadyHz};
  unsigned char bytes[sizeof f + 2];
  std::memcpy(bytes, f, sizeof f);
  bytes[sizeof f] = row.d.valid ? 1 : 0;
  bytes[sizeof f + 1] = static_cast<unsigned char>(row.d.midi);
  for (unsigned char b : bytes) h = (h ^ b) * 16777619u;
  return h;
}
inline constexpr std::uint32_t kFrameHashSeed = 2166136261u;

/// Runs `n` samples through a tuner in pieces that end exactly on frame boundaries. The sink has
/// `process(const float*, int)` (feeds the tuner), `reading()` (its newest TunerReading) and
/// `emit(const FrameRow&)` (receives each frame).
template <class Sink>
void feedFrames(TunerFrames& frames, const float* in, std::uint64_t n, Sink& sink) {
  while (n > 0) {
    const std::uint64_t take = n < frames.untilFrame() ? n : frames.untilFrame();
    if (take > 0) {
      sink.process(in, static_cast<int>(take));
      frames.advance(take);
      in += take;
      n -= take;
    }
    while (frames.due()) sink.emit(frames.take(sink.reading()));
  }
}

}  // namespace squatch::host
