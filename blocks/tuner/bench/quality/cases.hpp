// SPDX-License-Identifier: MIT
// A quality case: audio, where its notes are, and what each note truly is.
//
// Truth kinds, per segment (a note from its onset to its end):
//   exact    synthetic: the true pitch at every moment (synth.hpp truthAt, glide/vibrato/bend in)
//   ref      a labelled real note (EGFxSet string/fret, Guitar-TECHS MIDI, a known take): the
//            note is known; the pitch is the offline reference (reference.hpp), as a contour
//   annotated  a labelled real note whose pitch over time is annotated with the data (GuitarSet's
//            per-string pitch contours), held in `contour` as the reference's is
//   note     the note is known but no reference could be fitted (too short, too quiet)
//   none     nothing known (free play, chords): steadiness only
// The truth series has one row per display frame: truth_hz (0 = unknown), truth_midi (0 = no
// note expected; the note nearest truth_hz when that is known), steady (the truth moved < kSteadyCents over the 300 ms up to this frame), and
// the segment index (-1 = between notes).
#pragma once
#include <cmath>
#include <functional>
#include <string>
#include <vector>

#include "reference.hpp"

namespace quality {

constexpr double kSteadyCents = 0.2;  // truth change over 300 ms still counted as steady
constexpr double kSteadyWindow = 0.3;

struct Segment {
  double onset = 0.0, end = 0.0;  // seconds in the file
  int midi = 0;                   // expected note, 0 = unknown
  std::string truth = "none";     // exact | ref | note | none
  double refHz = 0.0;             // settled pitch (exact f0, or the offline note reference)
  double refUncCents = 0.0;       // the reference fit's standard error (cents)
  double refDriftCents = 0.0;     // half the change between the halves of its window (cents)
  double refB = 0.0;
  std::function<double(double)> exact;  // exact truth at seconds after onset (synthetic only)
  std::vector<ContourPoint> contour;    // offline reference contour (ref), or the annotated one (annotated)
};

struct Case {
  std::string id, dataset, family, value, file, source;
  std::vector<float> x;
  double fs = 48000.0;
  std::vector<Segment> segments;
  std::function<bool(Case&)> load;  // fills x (and, for real audio, the segments); false: skip
};

struct TruthRow {
  double tMs = 0.0, hz = 0.0;
  int midi = 0, segment = -1;
  bool steady = false;
};

namespace truthdetail {

/// The contour at time t (linear between points), or 0 outside it.
inline double contourAt(const std::vector<ContourPoint>& c, double t) {
  if (c.empty() || t < c.front().t || t > c.back().t) return 0.0;
  size_t i = 1;
  while (i < c.size() && c[i].t < t) ++i;
  if (i >= c.size()) return c.back().hz;
  const double u = (t - c[i - 1].t) / std::max(1e-9, c[i].t - c[i - 1].t);
  return c[i - 1].hz * std::pow(c[i].hz / c[i - 1].hz, u);
}

inline double hzAt(const Segment& s, double t) {
  if (s.truth == "exact") return s.exact(t - s.onset);
  if (s.truth == "ref" || s.truth == "annotated") return contourAt(s.contour, t);
  return 0.0;
}

/// Steady: the truth exists over the whole 300 ms up to t and moves less than kSteadyCents.
inline bool steadyAt(const Segment& s, double t) {
  if (t - kSteadyWindow < s.onset) return false;
  double lo = 1e9, hi = -1e9;
  for (int i = 0; i <= 6; ++i) {
    const double hz = hzAt(s, t - kSteadyWindow * i / 6.0);
    if (hz <= 0.0) return false;
    lo = std::min(lo, hz);
    hi = std::max(hi, hz);
  }
  return cents(hi, lo) < kSteadyCents;
}

}  // namespace truthdetail

/// The truth at each display frame time.
inline std::vector<TruthRow> truthSeries(const Case& c, const std::vector<double>& tMs) {
  std::vector<TruthRow> out;
  out.reserve(tMs.size());
  for (double ms : tMs) {
    TruthRow r;
    r.tMs = ms;
    const double t = ms / 1000.0;
    for (size_t i = 0; i < c.segments.size(); ++i) {
      const Segment& s = c.segments[i];
      if (t < s.onset || t >= s.end) continue;
      r.segment = static_cast<int>(i);
      r.hz = truthdetail::hzAt(s, t);
      // The note the truth is on now: a +49.9 c string gliding sharp is briefly the next note.
      r.midi = r.hz > 0.0 && s.midi > 0 ? static_cast<int>(std::lround(69.0 + 12.0 * std::log2(r.hz / 440.0))) : s.midi;
      r.steady = truthdetail::steadyAt(s, t);
    }
    out.push_back(r);
  }
  return out;
}

}  // namespace quality
