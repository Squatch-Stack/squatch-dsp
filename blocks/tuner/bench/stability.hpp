// SPDX-License-Identifier: MIT
// Scoring for eval_stability: room conditions, a per-hop run of the tuner, and per-note measures.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "squatch/tuner/tuner.hpp"
#include "wav.hpp"

namespace bench {

struct NoteSpan {
  double start, end, refHz;  // refHz 0 = unknown (filled from the clean run, or none)
};

struct Case {
  std::string name;
  std::vector<float> x;
  float fs = 48000.0f;
  std::vector<NoteSpan> notes;
  bool synthetic() const { return name.rfind("synth", 0) == 0; }
};

struct Condition {
  const char* name;
  double snrDb;      // room noise under the note; <= 0 means none
  double reverbMix;  // 0 = dry
  bool laptopHp;     // 150 Hz second-order high-pass, like a laptop microphone
};

inline std::vector<Condition> conditions(bool haveRoom) {
  std::vector<Condition> c = {{"clean", 0, 0, false}};
  if (!haveRoom) return c;
  c.push_back({"room noise, 20 dB SNR", 20, 0, false});
  c.push_back({"room noise, 10 dB SNR", 10, 0, false});
  c.push_back({"reverb + room 20 dB", 20, 0.35, false});
  c.push_back({"laptop mic: HP + reverb + room 12 dB", 12, 0.35, true});
  return c;
}

inline std::vector<float> firstSeconds(const Audio& a, double secs, float fs) {
  std::vector<float> out;
  const double step = a.rate / fs;
  for (double i = 0; i < a.x.size() && out.size() < static_cast<size_t>(secs * fs); i += step) out.push_back(a.x[static_cast<size_t>(i)]);
  double m = 0;
  for (float v : out) m += v;
  m /= std::max<size_t>(1, out.size());
  for (float& v : out) v -= static_cast<float>(m);
  return out;
}

/// Small-room reverb: four combs into two all-passes (Schroeder), about 0.4 s RT60 at 48 kHz.
inline void reverb(std::vector<float>& x, double mix) {
  if (mix <= 0) return;
  const int comb[4] = {1557, 1617, 1491, 1422}, ap[2] = {225, 556};
  std::vector<std::vector<float>> cb(4), ab(2);
  for (int i = 0; i < 4; ++i) cb[i].assign(static_cast<size_t>(comb[i]) * 2, 0.0f);
  for (int i = 0; i < 2; ++i) ab[i].assign(static_cast<size_t>(ap[i]) * 2, 0.0f);
  size_t ci[4] = {0, 0, 0, 0}, ai[2] = {0, 0};
  for (float& s : x) {
    float wet = 0;
    for (int i = 0; i < 4; ++i) {
      float& d = cb[i][ci[i]];
      const float y = d;
      d = s + 0.72f * y;
      ci[i] = (ci[i] + 1) % cb[i].size();
      wet += y;
    }
    wet *= 0.25f;
    for (int i = 0; i < 2; ++i) {
      float& d = ab[i][ai[i]];
      const float y = -0.5f * wet + d;
      d = wet + 0.5f * y;
      ai[i] = (ai[i] + 1) % ab[i].size();
      wet = y;
    }
    s = static_cast<float>((1.0 - mix) * s + mix * wet);
  }
}

/// Second-order Butterworth high-pass at fc.
inline void highPass(std::vector<float>& x, double fc, double fs) {
  const double w = std::tan(3.141592653589793 * fc / fs), k = 1.0 / (1.0 + std::sqrt(2.0) * w + w * w);
  const double b0 = k, b1 = -2 * k, b2 = k, a1 = 2 * (w * w - 1) * k, a2 = (1 - std::sqrt(2.0) * w + w * w) * k;
  double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
  for (float& s : x) {
    const double y = b0 * s + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
    x2 = x1; x1 = s; y2 = y1; y1 = y;
    s = static_cast<float>(y);
  }
}

/// Room noise looped under the signal at snrDb relative to the note's loud part (first 0.5 s).
inline double addRoom(std::vector<float>& x, const std::vector<float>& room, double snrDb, const std::vector<NoteSpan>& notes, float fs) {
  if (snrDb <= 0 || room.empty()) return 0.0;
  double sig = 0, n = 0, rn = 0;
  for (const auto& sp : notes)
    for (size_t i = static_cast<size_t>(sp.start * fs); i < std::min(x.size(), static_cast<size_t>((sp.start + 0.5) * fs)); ++i) { sig += double(x[i]) * x[i]; n += 1; }
  for (float v : room) rn += double(v) * v;
  if (n == 0 || rn == 0) return 0.0;
  const double g = std::sqrt(sig / n) / std::sqrt(rn / room.size()) * std::pow(10.0, -snrDb / 20.0);
  for (size_t i = 0; i < x.size(); ++i) x[i] += static_cast<float>(g * room[i % room.size()]);
  return g * std::sqrt(rn / room.size());
}

/// A condition's signal, and for every tuner hop whether the note itself is still at least 6 dB
/// above the room noise added under it (the clean signal's RMS over the hop vs the noise RMS).
struct Rendered {
  std::vector<float> x;
  std::vector<bool> audible;  // per hop of `block` samples
};

inline Rendered render(const Case& c, const Condition& k, const std::vector<float>& room) {
  Rendered out;
  std::vector<float> dry = c.x;
  reverb(dry, k.reverbMix);
  if (k.laptopHp) highPass(dry, 150.0, c.fs);
  out.x = dry;
  const double noise = addRoom(out.x, room, k.snrDb, c.notes, c.fs);
  const size_t block = static_cast<size_t>(std::lround(c.fs * 64.0 / 12000.0));
  for (size_t i = 0; i + block <= dry.size(); i += block) {
    double e = 0;
    for (size_t j = 0; j < block; ++j) e += double(dry[i + j]) * dry[i + j];
    out.audible.push_back(noise == 0.0 ? true : std::sqrt(e / block) > 2.0 * noise);
  }
  return out;
}

struct Hop {
  double t;
  squatch::tuner::TunerReading r;
};

inline std::vector<Hop> runHops(const std::vector<float>& x, float fs) {
  static squatch::tuner::Tuner tuner;
  squatch::tuner::TunerConfig cfg;
  cfg.sampleRate = fs;
  tuner.configure(cfg);
  tuner.reset();
  const int block = static_cast<int>(std::lround(fs * 64.0 / 12000.0));
  std::vector<Hop> out;
  for (size_t i = 0; i + static_cast<size_t>(block) <= x.size(); i += static_cast<size_t>(block)) {
    tuner.process(&x[i], block);
    out.push_back({static_cast<double>(i + static_cast<size_t>(block)) / fs, tuner.reading()});
  }
  return out;
}

inline double centsOf(double hz, double ref) { return 1200.0 * std::log2(hz / ref); }

inline double median(std::vector<double> v) {
  if (v.empty()) return NAN;
  std::nth_element(v.begin(), v.begin() + static_cast<long>(v.size() / 2), v.end());
  return v[v.size() / 2];
}

inline double quantile(std::vector<double> v, double q) {
  if (v.empty()) return NAN;
  std::sort(v.begin(), v.end());
  return v[static_cast<size_t>(q * static_cast<double>(v.size() - 1))];
}

/// Settled pitch of a note in one run: median steadyHz over 1.0-2.0 s after the onset.
inline double settledHz(const std::vector<Hop>& h, const NoteSpan& sp) {
  std::vector<double> v;
  for (const Hop& p : h)
    if (p.r.valid && p.t >= sp.start + 1.0 && p.t <= std::min(sp.end, sp.start + 2.0)) v.push_back(p.r.steadyHz);
  return median(v);
}

inline void fillReferences(std::vector<Case>& cases) {
  for (Case& c : cases) {
    if (c.synthetic()) continue;
    const std::vector<Hop> h = runHops(c.x, c.fs);
    for (NoteSpan& sp : c.notes) {
      const double s = settledHz(h, sp);
      sp.refHz = std::isfinite(s) ? s : 0.0;
    }
  }
}

struct NoteScore {
  int flickers = 0, restarts = 0, jumps = 0;
  double p95 = NAN, worst = NAN, settle = NAN, wrongFrac = 0, coverage = 0, holdErr = NAN;
  int buriedHops = 0, buriedReports = 0;  // hops where the note is under the room noise, and readings shown there
};

struct Totals {
  int notes = 0, flickers = 0, restarts = 0, jumps = 0, buriedHops = 0, buriedReports = 0;
  std::vector<double> p95, worst, settle, wrong, coverage, hold;
};

inline int midiOf(double hz) { return static_cast<int>(std::lround(69 + 12 * std::log2(hz / 440.0))); }

/// Runs of valid readings on one note shorter than 0.25 s.
inline void countFlickers(const std::vector<Hop>& h, size_t a, size_t b, NoteScore& s) {
  size_t runStart = 0;
  bool in = false;
  for (size_t i = a; i < b; ++i) {
    const bool v = h[i].r.valid;
    if (v && !in) { runStart = i; in = true; }
    if (in && (!v || i + 1 == b || h[i + 1].r.midi != h[i].r.midi)) {
      if (h[i].t - h[runStart].t < 0.25) ++s.flickers;
      in = false;
    }
  }
}

/// Re-starts of the same note inside one note span that no pluck caused: noteSeconds falls back
/// while the pitch stays within the same semitone and the input did not jump 8 dB (a real
/// re-pluck does). The synthetic and DI notes are each plucked once, so every restart there is
/// spurious; a live take has real re-plucks, so this is not reported for sessions.
inline void countRestarts(const std::vector<Hop>& h, size_t a, size_t b, NoteScore& s) {
  for (size_t i = a + 1; i < b; ++i) {
    const auto &r = h[i].r, &p = h[i - 1].r;
    if (p.noteSeconds > 0.2 && r.noteSeconds + 0.05 < p.noteSeconds && r.midi == p.midi && r.level < 2.5f * p.level) ++s.restarts;
  }
}

inline void countRuns(const std::vector<Hop>& h, size_t a, size_t b, NoteScore& s) {
  countFlickers(h, a, b, s);
  countRestarts(h, a, b, s);
}

/// Per-note accumulators for the settled measures.
struct Settle {
  std::vector<double> dev;
  double lastBad, prev = NAN, refHz;
  int valid = 0, wrong = 0, inWindow = 0, refMidi;
};

/// One hop inside the note's window: buried hops are counted apart; audible ones are scored.
inline void settleHop(const Hop& p, bool audible, Settle& st, NoteScore& s) {
  if (!audible) { ++s.buriedHops; if (p.r.valid) ++s.buriedReports; st.prev = NAN; return; }
  ++st.inWindow;
  if (!p.r.valid) return;
  ++st.valid;
  if (p.r.midi != st.refMidi) { ++st.wrong; st.lastBad = p.t; st.prev = NAN; return; }
  const double d = centsOf(p.r.hz, st.refHz);
  st.dev.push_back(std::fabs(d));
  if (std::fabs(d) > 1.0) st.lastBad = p.t;
  if (std::isfinite(st.prev) && std::fabs(d - st.prev) > 3.0) ++s.jumps;
  st.prev = d;
}

inline void measureSettled(const std::vector<Hop>& h, const std::vector<bool>& audible, const NoteSpan& sp, NoteScore& s) {
  Settle st;
  st.lastBad = sp.start;
  st.refHz = sp.refHz;
  st.refMidi = midiOf(sp.refHz);
  for (size_t i = 0; i < h.size(); ++i)
    if (h[i].t >= sp.start + 0.3 && h[i].t <= sp.end - 0.3) settleHop(h[i], i >= audible.size() || audible[i], st, s);
  s.p95 = quantile(st.dev, 0.95);
  s.worst = st.dev.empty() ? NAN : *std::max_element(st.dev.begin(), st.dev.end());
  s.settle = st.lastBad - sp.start;
  s.wrongFrac = st.valid ? static_cast<double>(st.wrong) / st.valid : 0;
  s.coverage = st.inWindow ? static_cast<double>(st.valid) / st.inWindow : 0;
}

/// What a display that holds the last valid reading shows once the note has been let go.
inline void measureHold(const std::vector<Hop>& h, size_t a, size_t b, const NoteSpan& sp, NoteScore& s) {
  for (size_t i = b; i-- > a;)
    if (h[i].r.valid && h[i].r.midi == midiOf(sp.refHz)) { s.holdErr = std::fabs(centsOf(h[i].r.hz, sp.refHz)); return; }
}

inline NoteScore scoreNote(const std::vector<Hop>& h, const std::vector<bool>& audible, const NoteSpan& sp) {
  NoteScore s;
  size_t a = 0, b = h.size();
  while (a < h.size() && h[a].t < sp.start) ++a;
  for (size_t i = a; i < h.size(); ++i) if (h[i].t > sp.end + 0.5) { b = i; break; }
  countRuns(h, a, b, s);
  measureSettled(h, audible, sp, s);
  measureHold(h, a, b, sp, s);
  return s;
}

inline void add(Totals& t, const NoteScore& s) {
  ++t.notes;
  t.flickers += s.flickers; t.restarts += s.restarts; t.jumps += s.jumps;
  t.buriedHops += s.buriedHops; t.buriedReports += s.buriedReports;
  auto push = [](std::vector<double>& v, double x) { if (std::isfinite(x)) v.push_back(x); };
  push(t.p95, s.p95); push(t.worst, s.worst); push(t.settle, s.settle); push(t.hold, s.holdErr);
  t.wrong.push_back(s.wrongFrac); t.coverage.push_back(s.coverage);
}

inline Totals scoreSet(const std::vector<Case>& cases, const std::string& set, const Condition& k, const std::vector<float>& room) {
  Totals t;
  for (const Case& c : cases) {
    if (c.synthetic() != (set == "synth")) continue;
    const Rendered rd = render(c, k, room);
    const std::vector<Hop> h = runHops(rd.x, c.fs);
    for (const NoteSpan& sp : c.notes)
      if (sp.refHz > 0) add(t, scoreNote(h, rd.audible, sp));
  }
  return t;
}

inline void printHeader() {
  std::printf("| set | condition | notes | flickers | same-note restarts | jumps > 3 c | p95 |dev| c | worst |dev| c (median) | settle ±1 c, s | wrong note | coverage while audible | shown while buried | held value error c (median / p90) |\n");
  std::printf("|---|---|--:|--:|--:|--:|--:|--:|--:|--:|--:|--:|--:|\n");
}

inline void printRow(const char* set, const Condition& k, const Totals& t) {
  const double buried = t.buriedHops ? 100.0 * t.buriedReports / t.buriedHops : 0.0;
  std::printf("| %s | %s | %d | %d | %d | %d | %.2f | %.1f | %.2f | %.1f%% | %.0f%% | %.0f%% | %.2f / %.2f |\n", set, k.name, t.notes, t.flickers, t.restarts, t.jumps,
              median(t.p95), median(t.worst), median(t.settle), 100 * median(t.wrong), 100 * median(t.coverage), buried, median(t.hold), quantile(t.hold, 0.9));
}

/// A live take with no reference: flickers and same-note restarts over the whole recording.
inline void printSession(const std::string& path, const Audio& a) {
  const std::vector<Hop> h = runHops(a.x, a.rate);
  NoteScore s;
  countRuns(h, 0, h.size(), s);
  int jumps = 0;
  for (size_t i = 1; i < h.size(); ++i)
    if (h[i].r.valid && h[i - 1].r.valid && h[i].r.midi == h[i - 1].r.midi && h[i].r.noteSeconds > 0.3 && std::fabs(h[i].r.cents - h[i - 1].r.cents) > 3.0) ++jumps;
  std::printf("\n**Session `%s`** (%.0f s, no reference): %d flickers, %d jumps > 3 c after the first 0.3 s of a note.\n",
              path.substr(path.find_last_of('/') + 1).c_str(), a.x.size() / a.rate, s.flickers, jumps);
}

}  // namespace bench
