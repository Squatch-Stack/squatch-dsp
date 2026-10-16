// SPDX-License-Identifier: MIT
// NSynth (Engel et al., ICML 2017; the test split, CC BY 4.0), its guitar notes, as quality cases.
// Nothing from it is in this repository.
//
// Layout, inside the DI corpus folder: public/nsynth/audio/<note_str>.wav (16 kHz mono, 4 s: held
// 3 s, then released), and public/nsynth/examples-guitar.json (each note's metadata, by note_str).
// note_str is <instrument_str>-<pitch>-<velocity>, e.g. guitar_acoustic_010-066-100.
//
// Each note is resampled 16 kHz -> 48 kHz before the tuner hears it, with the bench's resampler
// (bench::resample, synth.hpp; the phone pack uses the same): windowed sinc, 96 input taps with a
// Blackman window, cut-off 0.95 x 8 kHz. Nothing is added above 7.6 kHz, so partials above that are
// absent, as they are in the file.
//
// Truth: the note is the MIDI label. NSynth's notes are sampler playback and not guaranteed to be in
// tune, so the pitch is the offline reference (reference.hpp) searched within 60 c of the label,
// as for EGFxSet. The label's own tuning is visible in cases.jsonl as ref_hz against midi.
//
//   family  the instrument (guitar_acoustic_010, ...)
//   value   p<pitch>v<velocity>[|<quality>,<quality>...] from examples-guitar.json
#pragma once
#include <algorithm>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "json_lite.hpp"
#include "real_cases.hpp"

namespace quality {
namespace nsynth {

constexpr double kRate = 48000.0;
// The tuner's configured range (TunerConfig: minHz 38, maxHz 1400): bass E1 to E6. Notes outside
// it are reported apart and kept out of the verdict (score.py tier "outside").
constexpr int kLowestMidi = 28, kHighestMidi = 88;

struct Name {
  std::string instrument;
  int pitch = 0, velocity = 0;
};

/// "guitar_acoustic_010-066-100" -> {guitar_acoustic_010, 66, 100}; pitch 0 if it does not parse.
inline Name parseName(const std::string& s) {
  Name n;
  const size_t a = s.find('-'), b = a == std::string::npos ? a : s.find('-', a + 1);
  if (b == std::string::npos) return n;
  n.instrument = s.substr(0, a);
  n.pitch = std::atoi(s.substr(a + 1, b - a - 1).c_str());
  n.velocity = std::atoi(s.substr(b + 1).c_str());
  return n;
}

/// note_str -> "fast_decay,percussive" (NSynth's qualities_str), from examples-guitar.json.
inline std::map<std::string, std::string> qualities(const std::string& path) {
  std::map<std::string, std::string> out;
  const json::Value root = json::parseFile(path);
  for (size_t i = 0; i < root.keys.size(); ++i) {
    std::string q;
    for (const json::Value& v : root.items[i]["qualities_str"].items) q += (q.empty() ? "" : ",") + v.text();
    out[root.keys[i]] = q;
  }
  return out;
}

inline std::string value(const Name& n, const std::string& q) {
  char b[32];
  std::snprintf(b, sizeof b, "p%03dv%03d", n.pitch, n.velocity);
  return q.empty() ? b : std::string(b) + "|" + q;
}

}  // namespace nsynth

/// Every guitar note in `root` (public/nsynth).
inline std::vector<Case> nsynthCases(const std::string& root) {
  namespace fs = std::filesystem;
  std::vector<Case> out;
  const std::string audio = root + "/audio";
  if (!fs::is_directory(audio)) return out;
  const std::map<std::string, std::string> q = nsynth::qualities(root + "/examples-guitar.json");
  for (const auto& f : fs::directory_iterator(audio)) {
    if (f.path().extension() != ".wav") continue;
    const std::string stem = f.path().stem().string();
    const nsynth::Name n = nsynth::parseName(stem);
    if (n.pitch <= 0) continue;
    const auto it = q.find(stem);
    Case c;
    c.dataset = "nsynth";
    c.family = n.instrument;
    c.value = nsynth::value(n, it == q.end() ? "" : it->second);
    c.id = "nsynth-" + stem;
    c.file = f.path().string();
    c.source = "NSynth test split (Engel et al., ICML 2017), CC BY 4.0; resampled 16 -> 48 kHz";
    const int midi = n.pitch;
    c.load = [midi](Case& self) {
      if (!loadWav(self)) return false;
      if (self.fs != nsynth::kRate) {
        self.x = bench::resample(self.x, self.fs / nsynth::kRate);
        self.fs = nsynth::kRate;
      }
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
  std::sort(out.begin(), out.end(), [](const Case& a, const Case& b) { return a.id < b.id; });
  return out;
}

}  // namespace quality
