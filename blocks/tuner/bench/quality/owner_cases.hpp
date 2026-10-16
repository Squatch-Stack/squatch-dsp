// SPDX-License-Identifier: MIT
// Our guided DI session (hosts/pi/takes.json says what each take asked for), recorded
// outside this repository. Standard tuning is assumed; the tuning itself is not known, so a
// labelled note's pitch comes from the offline reference, as for the public sets.
//
// How each take's notes are named. Only takes whose notes are known are labelled: the single
// open strings, the retunes and the chromatic runs. The rest are scored for steadiness only:
// played hard or fast, several open strings ring together (measured: a hard open-E pluck sets
// the other five ringing within 3 to 6 dB of it), and a harmonic's octave is ambiguous.
//   single      one pluck: the first onset, to the end of the take
//   sequence    the notes in order (frets 0-12); onsets that do not match the count are unnamed
//   repeat      every onset is the same note (the retunes)
//   free        onsets only, nothing known: steadiness only
//   idle        nothing is played: any note on screen is a false reading
#pragma once
#include <filesystem>
#include <string>
#include <vector>

#include "real_cases.hpp"

namespace quality {

struct Take {
  const char* label;
  const char* mode;
  std::vector<int> notes;
  double searchCents = 60.0;  // retunes move a quarter-tone: search wider
  double until = 0.0;         // > 0: use only the first `until` seconds
};

inline const std::vector<Take>& takes() {
  static const std::vector<Take> t = {
      {"01-idle-hands-off", "idle", {}},
      {"02-idle-hand-on-strings", "idle", {}},
      {"03-open-E2", "single", {40}},
      {"04-open-A2", "single", {45}},
      {"05-open-D3", "single", {50}},
      {"06-open-G3", "single", {55}},
      {"07-open-B3", "single", {59}},
      {"08-open-E4", "single", {64}},
      {"09-open-soft", "free", {}},  // asked: the six open strings in turn; they ring together
      {"10-open-hard", "free", {}},
      {"11-retune-E2", "repeat", {40}, 90.0},
      {"12-retune-G3", "repeat", {55}, 90.0, 24.8},  // crackle after 24.9 s (notes.json)
      {"13-chromatic-E", "sequence", {40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52}},
      {"14-chromatic-B", "sequence", {59, 60, 61, 62, 63, 64, 65, 66, 67, 68, 69, 70, 71}},
      {"15-harmonics", "free", {}},
      {"16-replucks-A", "free", {}},  // asked: open A; the D string sounds louder (reference)
      {"17-palm-mutes", "free", {}},
      {"18-chords", "free", {}},
      {"19-volume-roll", "free", {}},
      {"20-pickups", "free", {}},
      {"21-free-play", "free", {}},
      {"22-idle-end", "idle", {}},
  };
  return t;
}

/// The take says what was asked for, not what sounded: a hard pluck rings every open string, a
/// fast re-pluck catches the next string. A named note stands only if no other open string or
/// neighbour within two semitones fits with more than a third of its power, counting only the
/// competitor's partials that are not also the named note's (a fourth or fifth away shares
/// some: A2's 3rd is E2's 4th). Octaves are not compared: a stiff string's octave-down fit takes
/// the same partials. Otherwise the segment is "disputed": still scored for steadiness, not for
/// note or pitch.
inline double ownPower(const RefFit& f, const RefFit& named) {
  double p = 0.0;
  for (int i = 0; i < f.used; ++i) {
    bool shared = false;
    for (int j = 0; j < named.used; ++j) shared = shared || std::fabs(cents(f.partialHz[i], named.partialHz[j])) < 25.0;
    if (!shared) p += f.partialPower[i];
  }
  return p;
}

inline void confirmNote(const std::vector<float>& x, double fs, Segment& g) {
  if (g.midi <= 0 || !fitReferences()) return;
  const size_t i0 = static_cast<size_t>((g.onset + 0.15) * fs), i1 = std::min(x.size(), static_cast<size_t>(std::min(g.end, g.onset + 1.5) * fs));
  if (i1 <= i0 + static_cast<size_t>(0.2 * fs)) return;
  auto fit = [&](int m) { return fitWindow({&x[i0], i1 - i0, fs}, {midiHz(m), 40.0, 8}); };
  const RefFit named = fit(g.midi);
  for (int m : {40, 45, 50, 55, 59, 64, g.midi - 2, g.midi - 1, g.midi + 1, g.midi + 2}) {
    if (m == g.midi || (m - g.midi) % 12 == 0) continue;
    const RefFit other = fit(m);
    if (other.ok && ownPower(other, named) > named.power / 3.0) {
      g.truth = "disputed";
      return;
    }
  }
}

/// Segments from the take's onsets, named by its mode.
inline std::vector<Segment> takeSegments(const Take& t, const std::vector<float>& x, double fs) {
  const double end = static_cast<double>(x.size()) / fs;
  const std::string mode = t.mode;
  if (mode == "idle") return {};
  std::vector<double> on = bench::findOnsets(x, static_cast<float>(fs), mode == "repeat" ? 0.3 : 0.6);
  if (mode == "single") on = {firstOnset(x, fs)};  // a note plucked before the take began has no onset
  std::vector<Segment> out;
  for (size_t i = 0; i < on.size(); ++i) {
    Segment g;
    g.onset = on[i];
    g.end = i + 1 < on.size() ? on[i + 1] - 0.02 : end;
    if (mode == "single" || mode == "repeat") g.midi = t.notes[0];
    else if (mode == "sequence" && on.size() == t.notes.size()) g.midi = t.notes[i];
    out.push_back(g);
  }
  return out;
}

inline Case ownerCase(const Take& t, const std::string& dir, const std::string& dataset) {
  Case c;
  c.dataset = dataset;
  c.family = t.mode;
  c.value = t.label;
  c.id = dataset + "-" + t.label;
  c.file = dir + "/" + t.label + ".wav";
  c.source = "owner DI session di-20260924 (not published)";
  c.load = [t](Case& self) {
    if (!loadWav(self)) return false;
    if (t.until > 0.0) self.x.resize(std::min(self.x.size(), static_cast<size_t>(t.until * self.fs)));
    self.segments = takeSegments(t, self.x, self.fs);
    for (Segment& g : self.segments) {
      fitSegment(self.x, self.fs, g, t.searchCents);
      confirmNote(self.x, self.fs, g);
    }
    return true;
  };
  return c;
}

/// Every take on the INSTRUMENT input, and takes 01-10 again from the LINE input.
inline std::vector<Case> ownerCases(const std::string& session) {
  std::vector<Case> out;
  for (const Take& t : takes()) {
    if (std::filesystem::exists(session + "/" + t.label + ".wav")) out.push_back(ownerCase(t, session, "owner"));
    const std::string line = session + "/line-input";
    if (std::filesystem::exists(line + "/" + t.label + ".wav")) out.push_back(ownerCase(t, line, "owner-line"));
  }
  return out;
}

}  // namespace quality
