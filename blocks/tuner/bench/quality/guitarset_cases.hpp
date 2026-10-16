// SPDX-License-Identifier: MIT
// GuitarSet (Xi et al., ISMIR 2018; v1.1.0, CC BY 4.0) as quality cases:
// the first set that follows pitch over time in real playing. Nothing from it is in this repository.
//
// Layout, inside the DI corpus folder:
//   public/guitarset/annotation/<track>.jams
//   public/guitarset/audio_hex-pickup_debleeded/<track>_hex_cln.wav   6 channels, 44.1 kHz
// Channel i is the JAMS annotations' data_source i, 0 = the low E. (Checked on
// 00_Rock1-130-A_solo: each string's notes put the most energy in its own channel.)
//
// One case per track and string that has notes: that string's hex channel alone, as a tuner on
// that string would hear it, at the file's own rate. Truth per note (JAMS note_midi, sorted by time):
//   - the note is round(value), from its start to its end or the next note's start;
//   - the pitch over time is the string's JAMS pitch_contour (pYIN, 5.8 ms hop) inside the note:
//     truth kind "annotated". Points more than 250 c from the note (pYIN octave slips) are
//     dropped, and a gap of more than 0.1 s ends the note's contour;
//   - when a note lasts 0.9 s or more, its settled pitch is also fitted by the offline reference
//     (reference.hpp) over 0.5-1.7 s, as on every real set, so the note error means the same here.
// The contour is good to a few cents, not a tenth: it is for note identity, time to lock, and
// tracking through bends, vibrato and fast lines. The sub-cent claims stay with the synthetic set.
// Strings are numbered as EGFxSet's: family "s1" is the high E, "s6" the low E.
#pragma once
#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "json_lite.hpp"
#include "real_cases.hpp"

namespace quality {

/// One channel of a multichannel PCM or float WAV (bench::readWav keeps only the first).
inline bool readWavChannel(const std::string& path, int channel, bench::Audio& out) {
  using namespace bench::wavdetail;
  const std::vector<unsigned char> b = slurp(path);
  if (b.size() < 44 || std::memcmp(b.data(), "RIFF", 4) || std::memcmp(b.data() + 8, "WAVE", 4)) return false;
  Format f;
  for (size_t pos = 12; pos + 8 <= b.size();) {
    const uint32_t len = u32(&b[pos + 4]);
    const unsigned char* d = &b[pos + 8];
    if (!std::memcmp(&b[pos], "fmt ", 4)) f = parseFmt(d, len, out);
    if (!std::memcmp(&b[pos], "data", 4) && f.channels > channel && f.bits >= 16) {
      const size_t bytes = std::min<size_t>(len, b.size() - pos - 8), width = static_cast<size_t>(f.bits / 8);
      const size_t frame = static_cast<size_t>(f.channels) * width, skip = static_cast<size_t>(channel) * width;
      out.x.resize(bytes / frame);
      for (size_t i = 0; i < out.x.size(); ++i) out.x[i] = sampleAt(d + i * frame + skip, f.bits, f.isFloat);
      return true;
    }
    pos += 8 + len + (len & 1);
  }
  return false;
}

namespace guitarset {

constexpr double kMaxGap = 0.1;          // s: a longer gap in the contour ends the note's truth
constexpr double kFirstPoint = 0.2;      // s: the contour must start this soon after the note
constexpr double kMaxBendCents = 250.0;  // points further from the note are pYIN errors

struct Note {
  double t, dur, midi;  // JAMS note_midi: start, duration, the note's mean pitch as a MIDI number
};

struct StringLabels {
  std::vector<Note> notes;
  std::vector<ContourPoint> contour;  // voiced pYIN points only
};

using Labels = std::array<StringLabels, 6>;  // by data_source, 0 = low E

/// Calls f(time, duration, value) for each observation of a JAMS annotation, in either JAMS layout:
/// a list of {time, duration, value} objects, or columns {time: [...], duration: [...], value: [...]}.
template <class F>
void observations(const json::Value& data, F f) {
  if (data.kind == json::Value::Array) {
    for (const json::Value& o : data.items) f(o["time"].number(), o["duration"].number(), o["value"]);
    return;
  }
  const json::Value& t = data["time"];
  const json::Value& d = data["duration"];
  const json::Value& v = data["value"];
  for (size_t i = 0; i < t.size() && i < v.size(); ++i) f(t.items[i].number(), i < d.size() ? d.items[i].number() : 0.0, v.items[i]);
}

/// The per-string notes and pitch contours of one JAMS file; false if it cannot be read.
/// One annotation (a note_midi or pitch_contour layer) into its string's labels.
inline void addAnnotation(const json::Value& a, StringLabels& sl) {
  const std::string& ns = a["namespace"].text();
  if (ns == "note_midi") {
    observations(a["data"], [&sl](double t, double d, const json::Value& v) { sl.notes.push_back({t, d, v.number()}); });
  } else if (ns == "pitch_contour") {
    observations(a["data"], [&sl](double t, double, const json::Value& v) {
      const double hz = v["frequency"].number();
      if (v["voiced"].b && hz > 0.0) sl.contour.push_back({t, hz, 0.0});
    });
  }
}

/// The string an annotation belongs to (its data_source, "0".."5"), or -1.
inline int annotationString(const json::Value& a) {
  const std::string& src = a["annotation_metadata"]["data_source"].text();
  return src.size() == 1 && src[0] >= '0' && src[0] <= '5' ? src[0] - '0' : -1;
}

inline bool readLabels(const std::string& jams, Labels& out) {
  const json::Value root = json::parseFile(jams);
  const json::Value& anns = root["annotations"];
  if (anns.kind != json::Value::Array) return false;
  for (const json::Value& a : anns.items) {
    const int s = annotationString(a);
    if (s >= 0) addAnnotation(a, out[static_cast<size_t>(s)]);
  }
  for (StringLabels& sl : out) {
    std::sort(sl.notes.begin(), sl.notes.end(), [](const Note& a, const Note& b) { return a.t < b.t; });
    std::sort(sl.contour.begin(), sl.contour.end(), [](const ContourPoint& a, const ContourPoint& b) { return a.t < b.t; });
  }
  return true;
}

/// The annotated contour inside one note: from its first point (within kFirstPoint of the start)
/// until the note ends or the contour has a gap longer than kMaxGap.
inline std::vector<ContourPoint> noteContour(const std::vector<ContourPoint>& all, double onset, double end, double midi) {
  std::vector<ContourPoint> out;
  const double centre = midiHz(midi);
  double last = onset + kFirstPoint - kMaxGap;
  auto it = std::lower_bound(all.begin(), all.end(), onset, [](const ContourPoint& p, double t) { return p.t < t; });
  for (; it != all.end() && it->t < end; ++it) {
    if (it->t - last > kMaxGap) break;
    if (std::fabs(cents(it->hz, centre)) > kMaxBendCents) continue;
    out.push_back(*it);
    last = it->t;
  }
  return out;
}

/// The note's settled pitch by the offline reference, as fitSegment does, keeping the annotation
/// as the truth over time.
inline void fitNoteReference(const std::vector<float>& x, double fs, Segment& g) {
  if (!fitReferences() || g.end - g.onset < 0.9) return;
  const double a = g.onset + 0.5, b = std::min(g.end - 0.05, g.onset + 1.7);
  const NoteRef r = referenceNote(x, fs, a, b, {midiHz(g.midi), 60.0, 8});
  if (!r.fit.ok) return;
  g.refHz = r.fit.f0;
  g.refB = r.fit.B;
  g.refUncCents = r.fit.sigmaCents;
  g.refDriftCents = r.driftCents;
}

inline std::vector<Segment> segments(const StringLabels& sl, double fileEnd) {
  std::vector<Segment> out;
  for (size_t i = 0; i < sl.notes.size(); ++i) {
    const Note& n = sl.notes[i];
    Segment g;
    g.onset = n.t;
    const double next = i + 1 < sl.notes.size() ? sl.notes[i + 1].t : fileEnd;
    g.end = std::min({n.t + n.dur, next, fileEnd});
    if (g.end <= g.onset) continue;
    g.midi = static_cast<int>(std::lround(n.midi));
    g.contour = noteContour(sl.contour, g.onset, g.end, n.midi);
    g.truth = g.contour.size() >= 2 ? "annotated" : "note";
    out.push_back(g);
  }
  return out;
}

}  // namespace guitarset

/// Every track in `root` (public/guitarset) with both its hex WAV and its JAMS, one case per string.
inline std::vector<Case> guitarsetCases(const std::string& root) {
  namespace fs = std::filesystem;
  std::vector<Case> out;
  const std::string audio = root + "/audio_hex-pickup_debleeded", suffix = "_hex_cln.wav";
  if (!fs::is_directory(audio)) return out;
  for (const auto& f : fs::directory_iterator(audio)) {
    const std::string name = f.path().filename().string();
    if (name.size() <= suffix.size() || name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0) continue;
    const std::string track = name.substr(0, name.size() - suffix.size());
    guitarset::Labels labels;
    if (!guitarset::readLabels(root + "/annotation/" + track + ".jams", labels)) continue;
    for (int s = 0; s < 6; ++s) {
      const guitarset::StringLabels& sl = labels[static_cast<size_t>(s)];
      if (sl.notes.empty()) continue;
      const std::string num = std::to_string(6 - s);
      Case c;
      c.dataset = "guitarset";
      c.family = "s" + num;
      c.value = track;  // <player>_<style><progression>-<tempo>-<key>_<comp|solo>
      c.id = "gset-" + track + "-s" + num;
      c.file = f.path().string();
      c.source = "GuitarSet v1.1.0 (Xi et al., ISMIR 2018), CC BY 4.0; hex debleeded channel " + std::to_string(s);
      c.load = [s, sl](Case& self) {
        bench::Audio a;
        if (!readWavChannel(self.file, s, a)) {
          std::fprintf(stderr, "cannot read %s\n", self.file.c_str());
          return false;
        }
        self.x = std::move(a.x);
        self.fs = a.rate;
        self.segments = guitarset::segments(sl, static_cast<double>(self.x.size()) / self.fs);
        for (Segment& g : self.segments) guitarset::fitNoteReference(self.x, self.fs, g);
        return true;
      };
      out.push_back(std::move(c));
    }
  }
  std::sort(out.begin(), out.end(), [](const Case& a, const Case& b) { return a.id < b.id; });
  return out;
}

}  // namespace quality
