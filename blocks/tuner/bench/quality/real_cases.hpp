// SPDX-License-Identifier: MIT
// Real recordings as quality cases. The audio is read, and each note's offline reference fitted
// (reference.hpp), when a worker loads the case; nothing from any dataset is in this repository.
//
//   EGFxSet (Clean/<pickup>/<string>-<fret>.wav, string 1 = high E): one note per file, the
//     note known from string and fret, its pitch from the offline reference.
//   Guitar-TECHS single notes (P1, P2): one long take per player; the notes and their times
//     from the MIDI file shipped with it (midi/midi_allsinglenotes.mid), pitch from the reference.
//   Our DI session: see owner_cases.hpp.
#pragma once
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "../onsets.hpp"
#include "../wav.hpp"
#include "cases.hpp"
#include "smf.hpp"
#include "synth_cases.hpp"

namespace quality {

/// Reference and contour for a note whose name is known. The note reference is fitted over
/// 0.5-1.7 s after the onset (past most of the pluck glide, before the note sinks into the
/// noise). A note too short for a 0.4 s stable window keeps its name only ("note"). A reference more than 50 c from the label is kept but
/// marked, so a mislabelled or badly tuned string is visible rather than silently wrong.
/// Off for a frames-only run (another platform): the truth comes from the reference run.
inline bool& fitReferences() {
  static bool on = true;
  return on;
}

inline void fitSegment(const std::vector<float>& x, double fs, Segment& g, double searchCents = 60.0) {
  if (g.midi <= 0) return;
  g.truth = "note";
  if (!fitReferences()) return;
  const double a = g.onset + 0.5, b = std::min(g.end - 0.05, g.onset + 1.7);
  if (b - a < 0.4) return;
  const NoteRef r = referenceNote(x, fs, a, b, {midiHz(g.midi), searchCents, 8});
  if (!r.fit.ok) return;
  g.refHz = r.fit.f0;
  g.refB = r.fit.B;
  g.refUncCents = r.fit.sigmaCents;
  g.refDriftCents = r.driftCents;
  g.contour = contour(x, fs, g.onset, g.end, r.fit.f0);
  if (!g.contour.empty()) g.truth = "ref";
}

inline bool loadWav(Case& c) {
  bench::Audio a;
  if (!bench::readWav(c.file, a)) {
    std::fprintf(stderr, "cannot read %s\n", c.file.c_str());
    return false;
  }
  c.x = std::move(a.x);
  c.fs = a.rate;
  return true;
}

/// The first onset, or where the energy peaks when the note starts at the very beginning.
inline double firstOnset(const std::vector<float>& x, double fs) {
  const std::vector<double> on = bench::findOnsets(x, static_cast<float>(fs), 0.3);
  if (!on.empty()) return on.front();
  const std::vector<double> e = bench::hopEnergies(x, 256);
  const size_t peak = static_cast<size_t>(std::max_element(e.begin(), e.end()) - e.begin());
  return std::max(0.0, static_cast<double>(peak) * 256.0 / fs - 0.02);
}

inline std::vector<Case> egfxCases(const std::string& clean) {
  namespace fs = std::filesystem;
  static const int kOpen[7] = {0, 64, 59, 55, 50, 45, 40};
  std::vector<Case> out;
  if (!fs::is_directory(clean)) return out;
  for (const auto& pickup : fs::directory_iterator(clean)) {
    if (!pickup.is_directory()) continue;
    for (const auto& f : fs::directory_iterator(pickup.path())) {
      int string = 0, fret = 0;
      if (f.path().extension() != ".wav" || std::sscanf(f.path().stem().c_str(), "%d-%d", &string, &fret) != 2 || string < 1 || string > 6) continue;
      Case c;
      c.dataset = "egfxset";
      c.family = pickup.path().filename().string();
      c.value = "s" + std::to_string(string) + "f" + std::to_string(fret);
      c.id = "egfx-" + c.family + "-" + f.path().stem().string();
      c.file = f.path().string();
      c.source = "EGFxSet (Pedroza et al., ISMIR 2022), CC BY 4.0";
      const int midi = kOpen[string] + fret;
      c.load = [midi](Case& self) {
        if (!loadWav(self)) return false;
        Segment g;
        g.onset = firstOnset(self.x, self.fs);
        g.end = static_cast<double>(self.x.size()) / self.fs;
        g.midi = midi;
        fitSegment(self.x, self.fs, g);
        self.segments = {g};
        return true;
      };
      out.push_back(std::move(c));
    }
  }
  std::sort(out.begin(), out.end(), [](const Case& a, const Case& b) { return a.id < b.id; });
  return out;
}

/// The MIDI's lag behind the audio: the median distance from each MIDI start to the nearest
/// detected onset (within 0.3 s). Reported, and applied.
inline double midiOffset(const std::vector<MidiNote>& notes, const std::vector<float>& x, double fs) {
  const std::vector<double> on = bench::findOnsets(x, static_cast<float>(fs), 0.2);
  std::vector<double> d;
  for (const MidiNote& n : notes) {
    double best = 1e9;
    for (double t : on) best = std::fabs(t - n.start) < std::fabs(best) ? t - n.start : best;
    if (std::fabs(best) < 0.3) d.push_back(best);
  }
  if (d.empty()) return 0.0;
  std::nth_element(d.begin(), d.begin() + static_cast<long>(d.size() / 2), d.end());
  return d[d.size() / 2];
}

inline std::vector<Segment> midiSegments(const std::vector<MidiNote>& notes, double offset, double fileEnd) {
  std::vector<Segment> out;
  for (size_t i = 0; i < notes.size(); ++i) {
    Segment g;
    g.onset = notes[i].start + offset;
    const double next = i + 1 < notes.size() ? notes[i + 1].start + offset : fileEnd;
    g.end = std::min(next, std::max(notes[i].end + offset, g.onset + 0.3));
    g.midi = notes[i].pitch;
    out.push_back(g);
  }
  return out;
}

inline std::vector<Case> guitarTechsCases(const std::string& root) {
  std::vector<Case> out;
  for (const char* p : {"P1", "P2"}) {
    const std::string dir = root + "/" + p + "_singlenotes/" + p + "_singlenotes";
    const std::string midi = dir + "/midi/midi_allsinglenotes.mid";
    if (!std::filesystem::exists(midi)) continue;
    Case c;
    c.dataset = "guitar-techs";
    c.family = p;
    c.value = "singlenotes";
    c.id = std::string("gtechs-") + p + "-singlenotes";
    c.file = dir + "/audio/directinput/directinput_allsinglenotes.wav";
    c.source = "Guitar-TECHS (ICASSP 2025), CC BY 4.0";
    c.load = [midi](Case& self) {
      if (!loadWav(self)) return false;
      const std::vector<MidiNote> notes = readMidi(midi);
      const double off = midiOffset(notes, self.x, self.fs);
      std::fprintf(stderr, "%s: %zu MIDI notes, MIDI-to-audio offset %+.3f s\n", self.id.c_str(), notes.size(), off);
      self.segments = midiSegments(notes, off, static_cast<double>(self.x.size()) / self.fs);
      for (Segment& g : self.segments) fitSegment(self.x, self.fs, g);
      return true;
    };
    out.push_back(std::move(c));
  }
  return out;
}

}  // namespace quality
