// SPDX-License-Identifier: MIT
// The display path exactly as the apps read it, for scoring what a player actually sees.
//
//   input -> TunerBlock (blocks of `block` frames) -> Latest<TunerSnapshot> -> polled at 30 Hz
//         -> toDisplay() (hosts/common/display.hpp) -> the number on screen
//
// The last step follows the phone app's TunerDisplay, itself a
// port of the web page's tuner-view.js; the release hold is hosts/common/hold.hpp (hold.js in C++,
// the one C++ port, checked against hold.js by hold_parity.mjs), and the rounding is
// tuner_frames.hpp's roundTenth:
//  - while the tuner reads (valid), the number is that reading's cents, rounded to 0.1;
//  - for 1.5 s after it lets go, the note stays and the number is the median of the last 0.5 s of
//    confident (unc <= 0.5 c) readings on that note, or "--.-" when they are too few (< 250 ms),
//    or disagree by more than 2 c;
//  - those times are the readings' own audio positions (TunerSnapshot.samples), as hold.js
//    states: a poll that finds no newer snapshot sees the same position and adds nothing.
// Rounding is the page's (formatCents: Math.round, halves up), on the full-precision value, as
// every display now rounds (tuner_frames.hpp roundTenth; hold_parity.mjs checks it digit for digit).
#pragma once
#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include <squatch/dsp/blocks/tuner.hpp>

#include "hosts/common/display.hpp"
#include "hosts/common/hold.hpp"
#include "hosts/common/tuner_frames.hpp"

namespace quality {

/// One display frame. t_ms is the audio position of the snapshot shown (TunerSnapshot.samples).
struct Frame {
  double tMs = 0.0;
  bool live = false;       // the tuner reads now (the reading is valid)
  bool noteShown = false;  // a note name is on screen (live, or held after release)
  int midi = 0, octave = 0;
  const char* name = "";
  float cents = NAN, hz = NAN;  // the live reading (NaN when not live)
  float unc = NAN;
  float shown = NAN;            // the number on screen, rounded to 0.1 (NaN: "--.-" or nothing)
};

/// tuner-view.js / TunerDisplay: the note and the number, from each polled reading, taken at audio
/// position `samples` of a source at `rate`.
class DisplayModel {
 public:
  Frame update(const squatch::host::DisplayReading& d, float unc, std::uint64_t samples, double rate) {
    hold_.add({d.midi, d.cents, unc, d.valid, samples, rate});
    if (d.valid) last_ = d;
    Frame f;
    f.live = d.valid;
    f.noteShown = d.valid || (last_.valid && hold_.holding(samples));
    if (!f.noteShown) return f;
    f.midi = last_.midi;
    f.octave = last_.note.octave;
    f.name = last_.note.name;
    if (f.live) {
      f.cents = d.cents;
      f.hz = d.steadyHz;
      f.unc = unc;
    }
    f.shown = squatch::host::roundTenth(f.live ? d.cents : hold_.held(last_.midi));
    return f;
  }

 private:
  squatch::host::Hold hold_;
  squatch::host::DisplayReading last_{};
};

struct RunOptions {
  double fs = 48000.0;
  int block = 16;        // frames per audio callback
  double pollHz = 30.0;  // display reads per second
  float a4 = 440.0f;
};

/// Runs a whole recording through TunerBlock and the display model; one Frame per poll.
inline std::vector<Frame> runDisplayPath(const std::vector<float>& x, const RunOptions& o) {
  auto t = std::make_unique<squatch::dsp::TunerBlock>();  // ~128 kB: never on the stack
  squatch::dsp::Arena none;
  t->prepare({o.fs, o.block}, none);
  DisplayModel view;
  std::vector<Frame> out;
  std::vector<float> buf(static_cast<size_t>(o.block));
  const double step = o.fs / o.pollHz;
  double nextTick = step;
  for (size_t i = 0; i + static_cast<size_t>(o.block) <= x.size(); i += static_cast<size_t>(o.block)) {
    std::copy(x.begin() + static_cast<long>(i), x.begin() + static_cast<long>(i) + o.block, buf.begin());
    t->process(buf.data(), buf.data(), o.block);
    const double done = static_cast<double>(i) + o.block;
    for (; nextTick <= done; nextTick += step) {
      t->readings().update();
      const squatch::dsp::TunerSnapshot& s = t->readings().front();
      const double tMs = 1000.0 * static_cast<double>(s.samples) / o.fs;
      Frame f = view.update(squatch::host::toDisplay(s.reading, o.a4), s.reading.uncertainty, s.samples, o.fs);
      f.tMs = tMs;
      out.push_back(f);
    }
  }
  return out;
}

/// The same path read at given audio positions (samples from the start), as another platform's
/// log was: the input goes in blocks of up to `block`, cut short to land exactly on each
/// position, and the display is read there. For comparing with a device's own frame times.
inline std::vector<Frame> runDisplayPathAt(const std::vector<float>& x, const RunOptions& o, const std::vector<size_t>& at) {
  auto t = std::make_unique<squatch::dsp::TunerBlock>();
  squatch::dsp::Arena none;
  t->prepare({o.fs, o.block}, none);
  DisplayModel view;
  std::vector<Frame> out;
  std::vector<float> buf(static_cast<size_t>(o.block));
  size_t pos = 0;
  for (size_t target : at) {
    while (pos < target) {
      const size_t n = std::min(static_cast<size_t>(o.block), target - pos);
      for (size_t i = 0; i < n; ++i) buf[i] = pos + i < x.size() ? x[pos + i] : 0.0f;
      t->process(buf.data(), buf.data(), static_cast<int>(n));
      pos += n;
    }
    t->readings().update();
    const squatch::dsp::TunerSnapshot& s = t->readings().front();
    const double ms = 1000.0 * static_cast<double>(target) / o.fs;
    Frame f = view.update(squatch::host::toDisplay(s.reading, o.a4), s.reading.uncertainty, s.samples, o.fs);
    f.tMs = ms;
    out.push_back(f);
  }
  return out;
}

}  // namespace quality
