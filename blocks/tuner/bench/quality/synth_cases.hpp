// SPDX-License-Identifier: MIT
// The synthetic grid: one factor at a time around a realistic electric-guitar pluck, on all six
// open strings, plus other tunings and the fretboard up to the 24th fret. Every case has exact
// truth (synth.hpp). Audio is made when a worker loads the case, not up front.
#pragma once
#include <cstdio>
#include <functional>
#include <random>
#include <string>
#include <vector>

#include "../synth.hpp"
#include "cases.hpp"

namespace quality {

struct OpenString {
  const char* name;
  int midi;
  double B, t60;
};

// Stiffness and decay for a typical 10-46 set: wound strings stiffer, plain strings die sooner.
inline const std::vector<OpenString>& openStrings() {
  static const std::vector<OpenString> s = {{"E2", 40, 2e-4, 4.0}, {"A2", 45, 1.5e-4, 3.5}, {"D3", 50, 1e-4, 3.0},
                                            {"G3", 55, 1e-4, 2.5}, {"B3", 59, 8e-5, 2.2}, {"E4", 64, 6e-5, 2.0}};
  return s;
}

inline double midiHz(double midi) { return 440.0 * std::exp2((midi - 69.0) / 12.0); }

/// The baseline pluck: clean DI, a little glide, bridge-ish pickup, -12 dBFS peak.
inline bench::ToneSpec basePluck(int midi, double B, double t60, double offsetCents) {
  bench::ToneSpec s;
  s.f0 = midiHz(midi) * std::exp2(offsetCents / 1200.0);
  s.inharmonicity = B;
  s.t60 = t60;
  s.glideCents = 4.0;
  s.pluckPos = 0.2;
  s.pickupPos = 0.1;
  s.pickNoiseDb = -10.0;
  s.snrDb = 50.0;
  s.preroll = 0.3;
  s.seconds = 3.5;
  s.peakDbfs = -12.0;
  return s;
}

/// A deterministic offset in +-10 c for cases whose tuning is not the factor under test.
inline double offsetFor(const std::string& id) {
  std::seed_seq seq(id.begin(), id.end());
  std::mt19937 r(seq);
  return std::uniform_real_distribution<double>(-10.0, 10.0)(r);
}

/// The audio of a synthetic case, deterministic per case id.
inline std::vector<float> synthAudio(const Case& c, const bench::ToneSpec& s) {
  std::seed_seq seq(c.id.begin(), c.id.end());
  std::mt19937 rng(seq);
  return bench::synthesize(s, c.fs, rng);
}

inline Case synthCase(const std::string& family, const std::string& value, const std::string& note, const bench::ToneSpec& s) {
  Case c;
  c.dataset = "synthetic";
  c.family = family;
  c.value = value;
  c.id = "syn-" + family + "-" + value + "-" + note;
  c.source = "bench/synth.hpp";
  Segment g;
  g.onset = s.preroll;
  g.end = s.preroll + s.seconds;
  g.midi = static_cast<int>(std::lround(69.0 + 12.0 * std::log2(s.f0 / 440.0)));
  g.truth = "exact";
  g.refHz = s.f0;
  g.exact = [s](double t) { return bench::truthAt(s, t); };
  c.segments.push_back(g);
  c.fs = 48000.0;
  c.load = [s](Case& self) {
    self.x = synthAudio(self, s);
    return true;
  };
  return c;
}

using Shape = std::function<void(bench::ToneSpec&)>;

struct Family {
  std::string name;
  std::vector<std::pair<std::string, Shape>> values;
};

inline std::string fmt(const char* f, double v) {
  char b[48];
  std::snprintf(b, sizeof b, f, v);
  return b;
}

inline Family tuningFamily(const char* name, const std::vector<double>& cents) {
  Family f{name, {}};
  for (double c : cents) f.values.push_back({fmt("%+.1fc", c), [c](bench::ToneSpec& s) { s.f0 *= std::exp2(c / 1200.0); }});
  return f;
}

template <class T>
Family sweep(const char* name, const char* label, const std::vector<T>& vals, std::function<void(bench::ToneSpec&, T)> set) {
  Family f{name, {}};
  for (T v : vals) f.values.push_back({fmt(label, static_cast<double>(v)), [set, v](bench::ToneSpec& s) { set(s, v); }});
  return f;
}

inline std::vector<Family> factorFamilies() {
  std::vector<Family> f;
  std::vector<double> fine, coarse;
  for (int i = -20; i <= 20; ++i) fine.push_back(0.1 * i);
  for (int i = -10; i <= 10; ++i) coarse.push_back(i == -10 ? -49.9 : i == 10 ? 49.9 : 5.0 * i);
  f.push_back(tuningFamily("offset-fine", fine));
  f.push_back(tuningFamily("offset-coarse", coarse));
  f.push_back(sweep<double>("stiffness", "B=%.0e", {0.0, 3e-5, 1e-4, 3e-4, 1e-3, 2e-3}, [](bench::ToneSpec& s, double v) { s.inharmonicity = v; }));
  f.push_back(sweep<double>("glide", "%.0fc", {0, 5, 10, 20, 40}, [](bench::ToneSpec& s, double v) { s.glideCents = v; }));
  f.push_back(sweep<double>("decay", "T60=%.1fs", {0.6, 1.5, 4, 8}, [](bench::ToneSpec& s, double v) { s.t60 = v; }));
  f.push_back(sweep<double>("noise", "SNR=%.0fdB", {60, 40, 30, 20, 10}, [](bench::ToneSpec& s, double v) { s.snrDb = v; }));
  f.push_back(sweep<double>("level", "%.0fdBFS", {-60, -50, -40, -30, -20, -12, -6}, [](bench::ToneSpec& s, double v) { s.peakDbfs = v; }));
  return f;
}

inline std::vector<Family> movingFamilies() {
  std::vector<Family> f;
  Family vib{"vibrato", {}};
  for (const auto& v : std::vector<std::pair<double, double>>{{5.5, 5}, {5.5, 15}, {4.0, 30}}) {
    const double hz = v.first, depth = v.second;
    vib.values.push_back({fmt("%.1fHz", hz) + fmt("+-%.0fc", depth), [hz, depth](bench::ToneSpec& s) { s.vibratoHz = hz; s.vibratoCents = depth; s.vibratoStart = 1.0; }});
  }
  f.push_back(vib);
  f.push_back(sweep<double>("bend", "+%.0fc", {50, 100, 200}, [](bench::ToneSpec& s, double v) { s.bendCents = v; s.bendStart = 1.0; s.bendSeconds = 0.4; }));
  Family hum{"hum", {}};
  for (double mains : {50.0, 60.0})
    for (double db : {-40.0, -30.0, -20.0})
      hum.values.push_back({fmt("%.0fHz", mains) + fmt("%.0fdB", db), [mains, db](bench::ToneSpec& s) { s.humHz = mains; s.humHarmonics = 5; s.humDb = db; }});
  f.push_back(hum);
  Family pickup{"pickup", {}};
  const std::vector<std::pair<const char*, std::pair<double, double>>> pos = {
      {"neck", {0.25, -1}}, {"middle", {0.155, -1}}, {"bridge", {0.062, -1}}, {"neck+middle", {0.25, 0.155}}, {"middle+bridge", {0.155, 0.062}}};
  for (const auto& p : pos) {
    const double q1 = p.second.first, q2 = p.second.second;
    pickup.values.push_back({p.first, [q1, q2](bench::ToneSpec& s) { s.pickupPos = q1; s.pickupPos2 = q2; }});
  }
  f.push_back(pickup);
  return f;
}

/// Other tunings and the fretboard: low B (7-string), drop D, bass E1 and 8-string F#1, frets up
/// to 24 on the low and high E and the 24th on the B.
inline std::vector<Case> rangeCases() {
  std::vector<Case> out;
  const std::vector<std::pair<std::string, int>> notes = {
      {"lowB1", 35}, {"dropD2", 38}, {"bassE1", 28}, {"8strFs1", 30}, {"E2fret3", 43}, {"E2fret5", 45}, {"E2fret7", 47}, {"E2fret12", 52},
      {"E2fret17", 57}, {"E2fret24", 64}, {"E4fret5", 69}, {"E4fret12", 76}, {"E4fret17", 81}, {"E4fret19", 83}, {"E4fret24", 88}, {"B3fret24", 83}};
  for (const auto& [name, midi] : notes) {
    const double B = midi < 45 ? 2.5e-4 : midi < 64 ? 1.2e-4 : 8e-5, t60 = midi < 45 ? 4.5 : midi < 70 ? 2.5 : 1.2;
    for (int rep = 0; rep < 2; ++rep) {
      const std::string id = name + "-" + std::to_string(rep);
      const bench::ToneSpec s = basePluck(midi, B, t60, offsetFor(id));
      out.push_back(synthCase("range", name, std::to_string(rep), s));
    }
  }
  return out;
}

inline std::vector<Case> synthCases() {
  std::vector<Case> out;
  std::vector<Family> fams = factorFamilies();
  for (Family& m : movingFamilies()) fams.push_back(std::move(m));
  for (const Family& f : fams)
    for (const auto& [value, shape] : f.values)
      for (const OpenString& o : openStrings()) {
        const bool tuning = f.name.rfind("offset", 0) == 0;
        bench::ToneSpec s = basePluck(o.midi, o.B, o.t60, tuning ? 0.0 : offsetFor(f.name + value + o.name));
        shape(s);
        out.push_back(synthCase(f.name, value, o.name, s));
      }
  for (Case& c : rangeCases()) out.push_back(std::move(c));
  return out;
}


}  // namespace quality
