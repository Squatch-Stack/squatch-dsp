// SPDX-License-Identifier: MIT
// The glitch oracle: four measurements that turn "it does not click" into a number, for the tests
// of every block that switches, fades or swaps something while audio runs.
//
// Adapted from namespace namchain::glitch in pedal-science/namchain by Rik Hemsley (MIT), pinned
// to e208bb8:
//   https://github.com/pedal-science/namchain/blob/e208bb869069b15a7331b452e03b7dc88726ffee/include/namchain/glitch.h
//   https://github.com/pedal-science/namchain/blob/e208bb869069b15a7331b452e03b7dc88726ffee/src/glitch/glitch.cpp
// Copyright (c) 2026 Rik Hemsley; the licence text is in THIRD_PARTY_NOTICES.md. Taken: the four
// rules, their defaults and their reasoning. Changed here: one header in the tests' style, the
// rules rewritten rather than copied, and the span checks report through Report::error.
//
// Three signals over one span: y, the render with the edit applied, and the two oracles it must
// agree with (the same input through the chain as before the edit, and as after), plus the edits.

//   1. continuity   a step, a polarity flip, a mute, an unramped parameter: a second difference
//                   inside an edit larger than kappa times the largest the oracles produce outside.
//   2. dropout      a run of digital black (-90 dBFS, over 1 ms) where the chain should be audible (the
//                   oracle before an edit up to it, the one after from it, both inside it).
//   3. hf           more than 3 dB above the louder oracle over 5 kHz around an edit (a hard switch puts energy there).
//   4. convergence  the render must BE the old chain before the edit and the new one after (-60 dBFS).
//   5. local        (ours, not namchain's) rule 1 with a LOCAL reference: rule 1's roughest-anywhere reference
//                   lets one pick attack hide a step in a quiet passage. Each frame of an edit is judged
//                   against what the oracles do at and near that frame (see local()), so the tolerance is
//                   the material's own at that instant.
//   6. level        (ours) the short-time level stays under the louder oracle, and Options::carried (the limiter's release): no swell.
// A span whose edits leave no settled region is an error; one on which no rule could be applied is
// not ok either (Report::judged), and each rule says whether it ran (continuityEvaluated, ...).
#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <string>
#include <vector>

namespace glitch {

struct Edit {
  std::size_t start = 0, end = 0;  // end: one past the last frame the transition may touch; == start is legal
};

struct Options {
  double sampleRate = 48000.0;
  double kappa = 2.0;
  double dropoutDb = -90.0, dropoutMaxMs = 1.0;
  double hfCornerHz = 5000.0, hfWindowMs = 10.0, hfAllowanceDb = 3.0;
  // Energy above the corner more than this far below the louder oracle's whole signal is not judged:
  // behind a cabinet it is the noise floor, and a 4 dB difference between two noise floors is not a click.
  double hfFloorDb = -50.0;
  double convergenceDb = -60.0, guardMs = 5.0;
  double continuityReachMs = 100.0;  // rule 1 takes its reference from the oracles this far either side of each edit, not from the whole span
  double levelWindowMs = 5.0, levelAllowanceDb = 1.5;  // rule 6
  double localReachMs = 1.0, fadeMinMs = 0.5, roughWeight = 2.0;  // rule 5
  std::size_t roughFrames = 8;
  // Rule 1 reads the whole span as its reference. On a smooth passage that makes the kink where any
  // linear fade ends a finding, and next to a pick attack it lets a step through, so material with
  // dynamics is judged by rule 5 alone: set this false.
  bool globalContinuity = true;
  const std::vector<float>* carried = nullptr;  // rule 6's third reference, the signals' length; not owned
  // A run of digital black only counts where the chain that should be playing is above this: an
  // oracle a hair over -90 dBFS against a render a hair under it is the same silence.
  double audibleDb = -70.0;
};

struct Report {
  bool ok = true;
  std::string error;                 // set when the span could not be judged; ok is then false
  std::vector<std::string> findings; // one line per failed rule, with the numbers
  bool continuityEvaluated = false, localEvaluated = false, hfEvaluated = false, convergenceEvaluated = false, levelEvaluated = false;
  double maxSwellDb = 0.0, maxDipDb = 0.0;  // rule 6: the render's short-time level above the louder oracle, below the quieter one
  bool judged() const { return continuityEvaluated || localEvaluated || hfEvaluated || convergenceEvaluated || levelEvaluated; }
  double referenceD2 = 0.0, maxEditD2 = 0.0;
  double maxDropoutMs = 0.0, maxHfExcessDb = 0.0;
  double localRatio = 0.0;       // rule 5: the worst frame's second difference over what it may be; above 1 fails
  std::size_t localFrame = 0;
  double worstConvergenceDb = -1000.0;  // -1000: bit-identical, or nothing settled to compare
  bool failed(const char* rule) const {
    for (const std::string& f : findings)
      if (f.compare(0, std::char_traits<char>::length(rule), rule) == 0) return true;
    return false;
  }
};

namespace detail {

using Region = std::pair<std::size_t, std::size_t>;  // [first, last)

inline std::size_t frames(double ms, double rate) { return ms <= 0.0 ? 0 : static_cast<std::size_t>(ms * rate / 1000.0 + 0.5); }
inline double toDb(double lin) { return lin > 0.0 ? 20.0 * std::log10(lin) : -1e9; }
inline double fromDb(double db) { return std::pow(10.0, db / 20.0); }

inline std::vector<Region> merged(std::vector<Region> r) {
  std::sort(r.begin(), r.end());
  std::vector<Region> out;
  for (const Region& x : r) {
    if (x.second <= x.first) continue;
    if (!out.empty() && x.first <= out.back().second) out.back().second = std::max(out.back().second, x.second);
    else out.push_back(x);
  }
  return out;
}

inline bool within(const std::vector<Region>& regions, std::size_t n) {
  for (const Region& r : regions)
    if (n >= r.first && n < r.second) return true;
  return false;
}

struct Peak {
  double value = 0.0;
  std::size_t frame = 0;
};

// Largest |v[n] - 2 v[n-1] + v[n-2]| over the frames inside (or outside) the regions.
inline Peak maxSecondDifference(const std::vector<float>& v, const std::vector<Region>& regions, bool inside) {
  Peak p;
  for (std::size_t n = 2; n < v.size(); ++n) {
    if (within(regions, n) != inside) continue;
    const double d2 = std::fabs(double(v[n]) - 2.0 * v[n - 1] + v[n - 2]);
    if (d2 > p.value) p = {d2, n};
  }
  return p;
}

// Second-order Butterworth high pass, forward then backward: zero phase, so a delay between the
// signals does not turn into an apparent difference in energy.
inline std::vector<double> highPass(const std::vector<float>& x, double corner, double rate) {
  const double w0 = 2.0 * 3.14159265358979323846 * corner / rate, c = std::cos(w0);
  const double alpha = std::sin(w0) / (2.0 * 0.70710678118654752440), a0 = 1.0 + alpha;
  const double b0 = (1.0 + c) / 2.0 / a0, b1 = -(1.0 + c) / a0, a1 = -2.0 * c / a0, a2 = (1.0 - alpha) / a0;
  std::vector<double> y(x.begin(), x.end());
  for (int pass = 0; pass < 2; ++pass) {
    double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
    for (double& s : y) {
      const double in = s, out = b0 * in + b1 * x1 + b0 * x2 - a1 * y1 - a2 * y2;
      x2 = x1, x1 = in, y2 = y1, y1 = out, s = out;
    }
    std::reverse(y.begin(), y.end());
  }
  return y;
}

inline double rms(const std::vector<double>& x, const Region& r) {
  if (r.second <= r.first) return 0.0;
  double s = 0.0;
  for (std::size_t i = r.first; i < r.second; ++i) s += x[i] * x[i];
  return std::sqrt(s / double(r.second - r.first));
}

inline double rms(const std::vector<float>& x, const Region& r) {
  if (r.second <= r.first) return 0.0;
  double s = 0.0;
  for (std::size_t i = r.first; i < r.second; ++i) s += double(x[i]) * x[i];
  return std::sqrt(s / double(r.second - r.first));
}

inline Peak maxAbsDifference(const std::vector<float>& a, const std::vector<float>& b, const Region& r) {
  Peak p;
  for (std::size_t i = r.first; i < r.second; ++i) {
    const double d = std::fabs(double(a[i]) - b[i]);
    if (d > p.value) p = {d, i};
  }
  return p;
}

inline std::string num(double v) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "%.4g", v);
  return buf;
}

// What every rule is given: the three signals, the edits, the options and the guard in frames.
struct Span {
  const std::vector<float>&y, &before, &after;
  const std::vector<Edit>& edits;
  const Options& o;
  std::size_t guard;
};

inline std::string checkSpan(const std::vector<Edit>& edits, std::size_t n, std::size_t guard) {
  for (std::size_t i = 0; i < edits.size(); ++i) {
    const Edit& e = edits[i];
    if (e.start > e.end) return "edit " + std::to_string(i) + " ends before it starts";
    if (i > 0 && e.start < edits[i - 1].end) return "edits " + std::to_string(i - 1) + " and " + std::to_string(i) + " overlap";
    if (e.end > n) return "edit " + std::to_string(i) + " reaches past the span";
    if (e.start < guard || e.end + guard > n) return "edit " + std::to_string(i) + " leaves no settled region inside the span";
  }
  return {};
}

// Largest second difference of v over [from, to) outside `skip`.
inline double maxD2Outside(const std::vector<float>& v, std::size_t from, std::size_t to, const Region& skip) {
  double worst = 0.0;
  for (std::size_t n = std::max<std::size_t>(from, 2); n < to && n < v.size(); ++n)
    if (n < skip.first || n >= skip.second) worst = std::max(worst, std::fabs(double(v[n]) - 2.0 * v[n - 1] + v[n - 2]));
  return worst;
}

// Rule 1 for one edit: its reference is the roughest the oracles get within continuityReachMs of it, so a pick attack
// elsewhere in the span loosens nothing here.
inline void continuityOne(Report& r, const Span& s, const Edit& e) {
  const Region touched{e.start > 2 ? e.start - 2 : 0, std::min(s.y.size(), e.end + 2)};
  const std::size_t reach = frames(s.o.continuityReachMs, s.o.sampleRate);
  const std::size_t from = touched.first > reach ? touched.first - reach : 0, to = std::min(s.y.size(), touched.second + reach);
  double ref = std::max(maxD2Outside(s.before, from, to, touched), maxD2Outside(s.after, from, to, touched));
  const bool fromRender = ref <= 0.0;
  if (fromRender) ref = maxD2Outside(s.y, from, to, touched);  // silent oracles: the render's own baseline
  if (ref <= 0.0) return;
  double worst = 0.0;
  std::size_t at = 0;
  for (std::size_t n = std::max<std::size_t>(touched.first, 2); n < touched.second; ++n) {
    const double d = std::fabs(double(s.y[n]) - 2.0 * s.y[n - 1] + s.y[n - 2]);
    if (d > worst) worst = d, at = n;
  }
  r.continuityEvaluated = true;
  if (worst / ref > r.maxEditD2 / std::max(r.referenceD2, 1e-300)) r.referenceD2 = ref, r.maxEditD2 = worst;
  if (worst > s.o.kappa * ref * (1.0 + 1e-12))
    r.findings.push_back("continuity: second difference " + num(worst) + " at frame " + std::to_string(at) + " against a limit of " + num(s.o.kappa * ref) +
                         (fromRender ? " (reference from the render itself)" : ""));
}

inline void continuity(Report& r, const Span& s) {
  for (const Edit& e : s.edits) continuityOne(r, s, e);
}

// The largest value of f(m) over the frames within `reach` of n (clamped to the span).
template <typename F>
inline double nearMax(std::size_t n, std::size_t reach, std::size_t size, F f) {
  double worst = 0.0;
  for (std::size_t m = n > reach + 2 ? n - reach : 2; m <= n + reach && m < size; ++m) worst = std::max(worst, f(m));
  return worst;
}

inline double d2(const std::vector<float>& v, std::size_t n) { return double(v[n]) - 2.0 * v[n - 1] + v[n - 2]; }

inline void local(Report& r, const Span& s) {
  const Options& o = s.o;
  const std::size_t reach = std::max<std::size_t>(1, frames(o.localReachMs, o.sampleRate)), near = std::max<std::size_t>(1, o.roughFrames);
  const double curvature = 1.0 / std::max(1.0, o.fadeMinMs * o.sampleRate / 1000.0);
  for (const Edit& e : s.edits) {
    const std::size_t from = e.start > 2 ? e.start - 2 : 2, to = std::min(s.y.size(), e.end + 2);
    for (std::size_t n = from; n < to; ++n) {
      const double apart = nearMax(n, reach, s.y.size(), [&](std::size_t m) { return std::fabs(double(s.after[m]) - s.before[m]); });
      const double rough = nearMax(n, near, s.y.size(), [&](std::size_t m) { return std::max(std::fabs(d2(s.before, m)), std::fabs(d2(s.after, m))); });
      const double a = d2(s.before, n), b = d2(s.after, n), v = d2(s.y, n);
      const double excess = std::max(0.0, std::max(std::min(a, b) - v, v - std::max(a, b)));
      const double ratio = excess / (curvature * apart + o.roughWeight * rough + 1e-9);  // 1e-9: -180 dBFS, so exact silence is not 0 over 0
      r.localEvaluated = r.localEvaluated || rough + apart > 0.0;
      if (ratio > r.localRatio) r.localRatio = ratio, r.localFrame = n;
    }
  }
  if (r.localRatio > 1.0)
    r.findings.push_back("local: second difference " + num(r.localRatio) + " times what the oracles and a " + num(o.fadeMinMs) + " ms fade allow at frame " +
                         std::to_string(r.localFrame));
}

// Runs of digital black in y as [first, last) pairs.
inline std::vector<Region> blackRuns(const std::vector<float>& y, double floor) {
  std::vector<Region> runs;
  std::size_t i = 0;
  while (i < y.size()) {
    if (std::fabs(y[i]) > floor) {
      ++i;
      continue;
    }
    std::size_t j = i;
    while (j < y.size() && std::fabs(y[j]) <= floor) ++j;
    runs.push_back({i, j});
    i = j;
  }
  return runs;
}

// The longest black run that contains a frame where the chain should be audible; value is its length.
// Which oracle says so depends on where the frame is: the one before an edit up to it, the one after
// it from its end, and inside it both (a fade between two audible chains must not go quiet, but one
// towards a gated or muted chain may). Judging against either oracle everywhere fails a gate that
// closes: the chain before it is still audible.
inline bool shouldBeAudible(const Span& s, std::size_t e, std::size_t i, double audible) {
  const bool before = std::fabs(s.before[i]) > audible, after = std::fabs(s.after[i]) > audible;
  if (e >= s.edits.size()) return after;
  return i >= s.edits[e].start ? before && after : before;
}

inline Peak longestBlackRun(const Span& s) {
  const double floor = fromDb(s.o.dropoutDb), audible = fromDb(s.o.audibleDb);
  std::vector<std::size_t> energy(s.y.size() + 1, 0);  // prefix count: "should there be sound in a..b" is one subtraction
  std::size_t e = 0;                                    // the first edit that has not ended
  for (std::size_t i = 0; i < s.y.size(); ++i) {
    while (e < s.edits.size() && i >= s.edits[e].end) ++e;
    energy[i + 1] = energy[i] + shouldBeAudible(s, e, i, audible);
  }
  Peak worst;
  for (const Region& run : blackRuns(s.y, floor)) {
    const bool heard = energy[run.second] > energy[run.first];
    if (heard && double(run.second - run.first) > worst.value) worst = {double(run.second - run.first), run.first};
  }
  return worst;
}

inline void dropout(Report& r, const Span& s) {
  const Peak run = longestBlackRun(s);
  r.maxDropoutMs = run.value / s.o.sampleRate * 1000.0;
  if (run.value > double(frames(s.o.dropoutMaxMs, s.o.sampleRate)))
    r.findings.push_back("dropout: digital black for " + num(r.maxDropoutMs) + " ms from frame " + std::to_string(run.frame) + " where the chain should be audible");
}

inline void highFrequency(Report& r, const Span& s) {
  const std::vector<float>&y = s.y, &before = s.before, &after = s.after;
  const std::vector<Edit>& edits = s.edits;
  const Options& o = s.o;
  if (edits.empty()) return;
  const std::size_t win = frames(o.hfWindowMs, o.sampleRate);
  std::vector<Region> regions;
  for (const Edit& e : edits) regions.push_back({e.start > win ? e.start - win : 0, std::min(y.size(), e.end + win)});
  regions = merged(regions);
  const std::vector<double> yh = highPass(y, o.hfCornerHz, o.sampleRate), bh = highPass(before, o.hfCornerHz, o.sampleRate),
                            ah = highPass(after, o.hfCornerHz, o.sampleRate);
  for (const Region& g : regions) {
    const double oracle = std::max(rms(bh, g), rms(ah, g));
    if (oracle <= 0.0) continue;  // nothing to compare against: unmeasurable, not a pass
    if (rms(yh, g) < std::max(rms(before, g), rms(after, g)) * fromDb(o.hfFloorDb)) continue;  // below the floor: noise, not a transition
    r.hfEvaluated = true;
    const double excess = toDb(rms(yh, g) / oracle);
    r.maxHfExcessDb = std::max(r.maxHfExcessDb, excess);
    if (excess > o.hfAllowanceDb)
      r.findings.push_back("hf: " + num(excess) + " dB above the louder oracle over frames " + std::to_string(g.first) + ".." + std::to_string(g.second));
  }
}

// Rule 6. The short-time level of the render inside an edit must stay between the two oracles': not above the louder one (the +3 dB
// swell of an equal-power fade on correlated material). The dip below the quieter one, where they are about as loud, is measured
// (maxDipDb) and not judged: any fade between uncorrelated signals dips up to 3 dB, the clean fades of the unit tests included.
inline void level(Report& r, const Span& s) {
  const std::size_t w = std::max<std::size_t>(8, frames(s.o.levelWindowMs, s.o.sampleRate));
  for (const Edit& e : s.edits) {
    for (std::size_t from = e.start; from + w <= std::min(s.y.size(), e.end); from += w) {
      const Region g{from, from + w};
      const double ry = rms(s.y, g), ra = rms(s.before, g), rb = rms(s.after, g), rc = s.o.carried ? rms(*s.o.carried, g) : 0.0, hi = std::max({ra, rb, rc}), lo = std::min(ra, rb);
      if (hi < fromDb(-80.0)) continue;
      r.levelEvaluated = true;
      r.maxSwellDb = std::max(r.maxSwellDb, toDb(ry / hi));
      if (lo >= 0.5 * hi) r.maxDipDb = std::max(r.maxDipDb, toDb(lo / std::max(ry, 1e-12)));
    }
  }
  if (r.maxSwellDb > s.o.levelAllowanceDb) r.findings.push_back("level: swell of " + num(r.maxSwellDb) + " dB above the louder oracle");
}

// One settled region against the oracle that should be playing there; empty ones (back to back edits) are not judged.
inline double settled(Report& r, const Span& s, const std::vector<float>& oracle, const Region& g, bool next) {
  if (g.second <= g.first) return 0.0;
  const char* which = next ? "next" : "previous";
  const double limit = fromDb(s.o.convergenceDb);
  const Peak p = maxAbsDifference(s.y, oracle, g);
  r.convergenceEvaluated = r.convergenceEvaluated || rms(oracle, g) > 0.0;  // silence is not a comparison
  if (p.value > limit) r.findings.push_back(std::string("convergence: not the ") + which + " chain at frame " + std::to_string(p.frame) + ", |difference| " + num(p.value));
  return p.value;
}

inline void convergence(Report& r, const Span& s) {
  const std::size_t guard = s.guard;
  double worstAbs = 0.0;
  for (std::size_t i = 0; i < s.edits.size(); ++i) {
    const std::size_t from = i == 0 ? 0 : s.edits[i - 1].end + guard, to = i + 1 == s.edits.size() ? s.y.size() : s.edits[i + 1].start - guard;
    worstAbs = std::max(worstAbs, settled(r, s, s.before, {from, s.edits[i].start - guard}, false));
    worstAbs = std::max(worstAbs, settled(r, s, s.after, {s.edits[i].end + guard, to}, true));
  }
  if (worstAbs > 0.0) r.worstConvergenceDb = toDb(worstAbs);
}

}  // namespace detail

/// Judge one span. The three signals have the same length; edits are in order and do not overlap.
inline Report analyse(const std::vector<float>& y, const std::vector<float>& before, const std::vector<float>& after,
                      const std::vector<Edit>& edits, const Options& o = {}) {
  Report r;
  const auto reject = [&r](std::string why) {
    r.ok = false, r.error = std::move(why);
    return r;
  };
  const bool same = before.size() == y.size() && after.size() == y.size() && (!o.carried || o.carried->size() == y.size());
  if (y.empty() || !same) return reject("the three signals must have the same, non-zero length");
  const std::size_t guard = detail::frames(o.guardMs, o.sampleRate);
  const std::string bad = detail::checkSpan(edits, y.size(), guard);
  if (!bad.empty()) return reject(bad);
  const detail::Span span{y, before, after, edits, o, guard};
  if (o.globalContinuity) detail::continuity(r, span);
  detail::local(r, span);
  detail::level(r, span);
  detail::dropout(r, span);
  detail::highFrequency(r, span);
  detail::convergence(r, span);
  r.ok = r.findings.empty() && r.judged();
  if (!r.judged()) r.error = "no rule could be applied to this span (no signal, nothing settled), so it was not judged";
  return r;
}

}  // namespace glitch
