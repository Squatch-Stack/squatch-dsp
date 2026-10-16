// SPDX-License-Identifier: MIT
// An offline reference pitch for real recordings that have no labelled tuning.
//
// Deliberately NOT the tuner's own method (heterodyne strobe + phase-slope fit), so the
// reference cannot share its errors:
//  1. Blackman-Harris window over a long stretch of the note (sidelobes -92 dB, so hum and the
//     neighbouring partials cannot pull a peak).
//  2. Each partial's frequency is the maximum of the window's continuous spectrum (the DTFT,
//     evaluated exactly at any frequency, no bins): a coarse grid at a quarter of the window's
//     resolution, then golden-section search. For a stationary sinusoid in white noise this is
//     the maximum-likelihood estimate; an exponential decay does not bias it (the spectrum of a
//     real, non-negative window is largest at zero offset).
//  3. Partials are found in order, each predicted from the fit of those before it, and kept
//     only with 10 dB of local SNR (against the spectrum half-way to the next partial) and
//     when no 50 or 60 Hz mains harmonic lies within the window's main lobe of it (2 / T Hz
//     either side: at 0.4 s, E2's 3rd partial at 247 Hz sits on 60 Hz's 4th at 240 Hz).
//  4. Stiff-string fit f_k = k f0 sqrt(1 + B k^2): weighted least squares of f_k / k on k^2
//     gives B, then f0 is the weighted mean of f_k / (k sqrt(1 + B k^2)). Weights k^2 * SNR_k
//     (capped at 60 dB): a partial's frequency error scales as 1 / sqrt(SNR), divided by k.
//  5. Uncertainty: the fit's standard error from its own residuals (3+ partials), or the
//     single-tone Cramer-Rao bound otherwise. referenceNote() also reports half the difference
//     between the two halves of the window, which is the string's own drift.
// Tested against synthetic strings with known truth in test_quality.cpp.
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace quality {

struct RefFit {
  bool ok = false;
  double f0 = 0.0, B = 0.0;
  double sigmaCents = 0.0;  // standard error of f0 from the fit
  int used = 0;             // partials in the fit
  double power = 0.0;       // summed power of the partials used (for choosing between notes)
  std::array<double, 8> partialHz{}, partialPower{};  // each partial used (the first `used`)
};

namespace refdetail {

constexpr double kPi = 3.14159265358979323846;

struct Windowed {
  std::vector<double> y;  // x * Blackman-Harris
  double fs = 48000.0;
  double seconds() const { return static_cast<double>(y.size()) / fs; }
};

inline Windowed window(const float* x, size_t n, double fs) {
  Windowed w;
  w.fs = fs;
  w.y.resize(n);
  const double m = static_cast<double>(n - 1);
  for (size_t i = 0; i < n; ++i) {
    const double p = 2.0 * kPi * static_cast<double>(i) / m;
    w.y[i] = x[i] * (0.35875 - 0.48829 * std::cos(p) + 0.14128 * std::cos(2 * p) - 0.01168 * std::cos(3 * p));
  }
  return w;
}

/// |DTFT|^2 of the windowed signal at f Hz, by a rotating phasor (double precision).
inline double power(const Windowed& w, double f) {
  const double a = -2.0 * kPi * f / w.fs, cr = std::cos(a), ci = std::sin(a);
  double zr = 1.0, zi = 0.0, re = 0.0, im = 0.0;
  for (double v : w.y) {
    re += v * zr;
    im += v * zi;
    const double t = zr * cr - zi * ci;
    zi = zr * ci + zi * cr;
    zr = t;
  }
  return re * re + im * im;
}

struct Peak {
  double f = 0.0, p = 0.0;
};

/// The spectrum's maximum in [lo, hi]: grid at a quarter of the resolution, then golden section.
inline Peak peakIn(const Windowed& w, double lo, double hi) {
  const double step = 0.25 / w.seconds();
  Peak best{lo, -1.0};
  for (double f = lo; f <= hi; f += step) {
    const double p = power(w, f);
    if (p > best.p) best = {f, p};
  }
  double a = std::max(lo, best.f - step), b = std::min(hi, best.f + step);
  const double g = 0.6180339887498949;
  double c = b - g * (b - a), d = a + g * (b - a), pc = power(w, c), pd = power(w, d);
  for (int it = 0; it < 40 && b - a > 1e-7; ++it) {
    if (pc > pd) { b = d; d = c; pd = pc; c = b - g * (b - a); pc = power(w, c); }
    else { a = c; c = d; pc = pd; d = a + g * (b - a); pd = power(w, d); }
  }
  const double f = 0.5 * (a + b);
  return {f, power(w, f)};
}

/// True when a 50 or 60 Hz harmonic is within the main lobe (+-2 / T Hz) of f.
inline bool nearMains(double f, double seconds) {
  const double lobe = 2.0 / seconds;
  for (double mains : {50.0, 60.0}) {
    const double n = std::round(f / mains);
    if (n >= 1.0 && std::fabs(f - n * mains) < lobe) return true;
  }
  return false;
}

#ifndef REF_MIN_SNR
#define REF_MIN_SNR 100.0
#endif
constexpr double kMinSnr = REF_MIN_SNR;  // 20 dB: a weaker partial adds more error than it removes
constexpr double kPriorB = 1e-4;

/// The peak's power over the median spectrum at ten points between it and its neighbours. One
/// point is a single draw of an exponential distribution, far too noisy to weight by; the median
/// also ignores the odd point that lands on a hum line.
inline double localSnr(const Windowed& w, const Peak& pk, double gap) {
  double p[10];
  for (int i = 0; i < 5; ++i) {
    const double d = (0.3 + 0.1 * i) * gap;
    p[2 * i] = power(w, pk.f - d);
    p[2 * i + 1] = power(w, pk.f + d);
  }
  std::nth_element(p, p + 5, p + 10);
  return p[5] > 0.0 ? pk.p / (p[5] / 0.69315) : 1e9;  // median of an exponential = mean ln 2
}

struct Partial {
  int k;
  double f, snr;
};

struct Lsq {
  double f0 = 0.0, c = 0.0, sigma = 0.0;
};

/// Weighted least squares y_k = f0 + c k^2 (y_k = f_k / k), with f0's standard error.
inline Lsq fitLine(const std::vector<Partial>& ps) {
  double sw = 0, sx = 0, sy = 0, sxx = 0, sxy = 0;
  for (const Partial& p : ps) {
    const double w = p.k * p.k * std::min(p.snr, 1e6), x = p.k * p.k, y = p.f / p.k;
    sw += w; sx += w * x; sy += w * y; sxx += w * x * x; sxy += w * x * y;
  }
  const double det = sw * sxx - sx * sx;
  Lsq r;
  if (ps.size() < 3 || det <= 0.0) return r;
  r.c = (sw * sxy - sx * sy) / det;
  r.f0 = (sy - r.c * sx) / sw;
  double ss = 0;
  for (const Partial& p : ps) {
    const double e = p.f / p.k - r.f0 - r.c * p.k * p.k;
    ss += p.k * p.k * std::min(p.snr, 1e6) * e * e;
  }
  r.sigma = std::sqrt(ss / static_cast<double>(ps.size() - 2) * sxx / det);
  return r;
}

/// f0 and B from the partials found so far (B = kPriorB with fewer than 3).
inline RefFit solve(const std::vector<Partial>& ps, double seconds) {
  RefFit r;
  if (ps.empty()) return r;
  const Lsq l = fitLine(ps);
  // With fewer than 3 partials B cannot be fitted: assume 1e-4, a typical guitar string, rather
  // than 0 (which would read a 2nd partial 0.26 c sharp of the fundamental on such a string).
  r.B = l.f0 > 0.0 ? std::clamp(2.0 * l.c / l.f0, 0.0, 5e-3) : kPriorB;
  double sw = 0, swy = 0, snr = 0;
  for (const Partial& p : ps) {
    const double w = p.k * p.k * std::min(p.snr, 1e6);
    sw += w;
    swy += w * p.f / (p.k * std::sqrt(1.0 + r.B * p.k * p.k));
    snr = std::max(snr, p.snr);
  }
  r.f0 = swy / sw;
  r.used = static_cast<int>(ps.size());
  // Cramer-Rao for one sinusoid: sd(f) = sqrt(6 / SNR) / (2 pi T), as a floor.
  const double crlb = std::sqrt(6.0 / std::max(snr, 1.0)) / (2.0 * kPi * seconds);
  r.sigmaCents = 1731.234 * std::max(l.sigma, crlb) / r.f0;  // 1200 / ln 2
  r.ok = true;
  return r;
}

}  // namespace refdetail

/// Where to look: near f0Guess, within searchCents for the first partial found.
struct Search {
  double f0Guess = 0.0;
  double searchCents = 60.0;
  int maxK = 8;
};

/// A stretch of audio.
struct Span {
  const float* x;
  size_t n;
  double fs;
};

namespace refdetail {

struct Found {
  bool ok = false;
  Partial partial{};
  double power = 0.0;
};

/// Partial k: predicted from the fit so far, found as the spectrum's peak, kept if clear.
inline Found findPartial(const Windowed& w, const RefFit& fit, const Search& q, int k) {
  const double pred = fit.ok ? k * fit.f0 * std::sqrt(1.0 + fit.B * k * k) : k * q.f0Guess;
  const double span = std::exp2((fit.ok ? 25.0 : q.searchCents) / 1200.0);
  const Peak pk = peakIn(w, pred / span, pred * span);
  const double snr = localSnr(w, pk, fit.ok ? fit.f0 : q.f0Guess);
  if (snr < kMinSnr || nearMains(pk.f, w.seconds())) return {};
  return {true, {k, pk.f, snr}, pk.p};
}

}  // namespace refdetail

/// The reference over one stretch: partials 1..maxK of a note near the guess.
inline RefFit fitWindow(const Span& a, const Search& q) {
  using namespace refdetail;
  const Windowed w = window(a.x, a.n, a.fs);
  std::vector<Partial> ps;
  RefFit fit;
  double total = 0.0;
  for (int k = 1; k <= q.maxK && k * q.f0Guess * std::exp2(q.searchCents / 1200.0) < 0.45 * a.fs; ++k) {
    const Found f = findPartial(w, fit, q, k);
    if (!f.ok) continue;
    ps.push_back(f.partial);
    total += f.power;
    fit = solve(ps, w.seconds());
    fit.partialPower[static_cast<size_t>(fit.used - 1)] = f.power;
  }
  for (size_t i = 0; i < ps.size() && i < fit.partialHz.size(); ++i) fit.partialHz[i] = ps[i].f;
  fit.power = total;
  return fit;
}

inline double cents(double hz, double ref) { return 1200.0 * std::log2(hz / ref); }

/// A note's reference over [a, b) seconds: the whole stretch, plus half the difference of its
/// two halves as the string's own drift (both in cents).
struct NoteRef {
  RefFit fit;
  double driftCents = 0.0;  // half the difference between the two halves: the string's own drift
  double uncertaintyCents() const { return std::sqrt(fit.sigmaCents * fit.sigmaCents + driftCents * driftCents); }
};

inline NoteRef referenceNote(const std::vector<float>& x, double fs, double a, double b, const Search& q) {
  NoteRef r;
  const size_t i0 = static_cast<size_t>(std::max(0.0, a * fs)), i1 = std::min(x.size(), static_cast<size_t>(b * fs));
  if (i1 <= i0 + static_cast<size_t>(0.2 * fs)) return r;
  r.fit = fitWindow({&x[i0], i1 - i0, fs}, q);
  if (!r.fit.ok) return r;
  const size_t h = (i1 - i0) / 2;
  const Search near{r.fit.f0, 20.0, q.maxK};
  const RefFit p = fitWindow({&x[i0], h, fs}, near), o = fitWindow({&x[i0 + h], h, fs}, near);
  if (p.ok && o.ok) r.driftCents = 0.5 * std::fabs(cents(o.f0, p.f0));
  return r;
}

/// A reference contour: causal windows of kContourWindow s ending every kContourHop s in [a, b].
struct ContourPoint {
  double t, hz, sigmaCents;
};

constexpr double kContourWindow = 0.6, kContourHop = 0.1;

inline std::vector<ContourPoint> contour(const std::vector<float>& x, double fs, double a, double b, double f0Guess) {
  std::vector<ContourPoint> out;
  Search q{f0Guess, 60.0, 8};
  const size_t n = static_cast<size_t>(kContourWindow * fs);
  for (double t = a + kContourWindow; t <= b; t += kContourHop) {
    const size_t i1 = std::min(x.size(), static_cast<size_t>(t * fs));
    if (i1 < n) continue;
    const RefFit f = fitWindow({&x[i1 - n], n, fs}, q);
    if (!f.ok) continue;
    out.push_back({t, f.f0, f.sigmaCents});
    q = {f.f0, 30.0, 8};  // follow a moving note (a bend, a peg) without jumping to a neighbour
  }
  return out;
}

}  // namespace quality
