// SPDX-License-Identifier: MIT
// Squatch Claw (the distortion board): quiet on silence, clips when driven, the Filter knob darkens, and,
// when SQUATCH_CLAW_SPICE names a directory of ngspice renders of the board, the block against
// them case by case.
//
// Those renders (our board tooling's SPICE script: in_N.wav, out_N.wav, float32 at 48 kHz) are
// made with an LM308 macromodel that may be read but not published, so they never enter this
// repository; without the variable the correlation is reported as skipped.
#include <squatch/dsp/blocks/claw.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "check.hpp"
#include "signals.hpp"

using namespace squatch::dsp;
using namespace sig;

namespace {

std::vector<float> run(Claw& b, const std::vector<float>& in) {
  std::vector<float> out(in.size());
  for (size_t i = 0; i < in.size(); i += 64) {
    const int len = static_cast<int>(std::min<size_t>(64, in.size() - i));
    b.process(&in[i], &out[i], len);
  }
  return out;
}

// A mono float32 WAV's samples, or nothing.
std::vector<float> readWav(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  const std::vector<char> b((std::istreambuf_iterator<char>(f)), {});
  size_t i = 12;
  while (i + 8 <= b.size() && std::memcmp(&b[i], "data", 4) != 0) {
    std::uint32_t len;
    std::memcpy(&len, &b[i + 4], 4);
    i += 8 + len;
  }
  if (i + 8 > b.size()) return {};
  std::uint32_t len;
  std::memcpy(&len, &b[i + 4], 4);
  std::vector<float> x(std::min<size_t>(len, b.size() - i - 8) / 4);
  std::memcpy(x.data(), &b[i + 8], x.size() * 4);
  return x;
}

// Error-to-signal ratio from `from` on, with the block's output advanced by `lag` samples.
double esrAt(const std::vector<float>& y, const std::vector<float>& ref, size_t from, int lag) {
  double e = 0.0, s = 0.0;
  for (size_t i = from; i + lag < y.size() && i < ref.size(); ++i) {
    const double d = y[i + lag] - ref[i];
    e += d * d;
    s += static_cast<double>(ref[i]) * ref[i];
  }
  return s > 0.0 ? e / s : 1.0;
}

}  // namespace

TEST(clawIsQuietOnSilenceAndClipsWhenDriven) {
  static Claw c;
  Arena none;
  c.prepare(Setup{48000.0, 64}, none);
  c.set(Claw::Distortion, 1.0f);
  const std::vector<float> zero(9600, 0.0f);
  const float quiet = peak(run(c, zero));
  CHECK(quiet < 1e-6f, "silence in, %g out", quiet);
  const std::vector<float> out = run(c, sine(110.0, 48000, 48000.0, 0.1));
  std::printf("  claw: 0.1 V A2 at full distortion, %.3f V out, clip %g\n", peak(out), c.get(Claw::Clip));
  CHECK(c.get(Claw::Clip) == 1.0f, "full distortion on 0.1 V clips");
  for (float v : out) CHECK(std::isfinite(v), "output stays finite");
  CHECK(peak(out) < 2.0f, "the output is bounded by the diodes and the buffer, %.3f V", peak(out));
}

TEST(clawFilterDarkens) {
  // Filter is a lowpass on the clipped signal: turning it up takes the top off.
  const std::vector<float> in = sine(220.0, 48000, 48000.0, 0.2);
  double hf[2];
  for (int k = 0; k < 2; ++k) {
    static Claw c;
    Arena none;
    c.prepare(Setup{48000.0, 64}, none);
    c.set(Claw::Distortion, 0.8f);
    c.set(Claw::Filter, k == 0 ? 0.0f : 0.8f);
    const std::vector<float> out = run(c, in);
    hf[k] = amplitudeAt(out, 220.0 * 15, 48000.0, 24000, 48000);  // the 15th harmonic, 3.3 kHz
  }
  std::printf("  claw: 15th harmonic %.1f dB at Filter 0, %.1f dB at Filter 0.8\n", db(hf[0]), db(hf[1]));
  CHECK(hf[1] < hf[0] * 0.3, "Filter 0.8 is at least 10 dB darker at 3.3 kHz");
}

// The Filter knob end to end (2026-10-11): a model-library render found the block "silent at Filter 0,
// level rising steeply with Filter". The block was right; the caller passed the contract's STABLE ids
// (Distortion 1, Filter 2, Volume 3) to Block::set, which takes the parameter INDEX (0, 1, 2), so its
// "Filter" wrote Volume. These tests pin both the block's Filter law and that id trap.
namespace {
double peakAtFilter(float filt) {
  static Claw c;
  Arena none;
  c.prepare(Setup{48000.0, 64}, none);
  c.set(Claw::Distortion, 0.5f);
  c.set(Claw::Filter, filt);
  c.set(Claw::Volume, 1.0f);
  const std::vector<float> out = run(c, sine(220.0, 48000, 48000.0, 0.25));
  return peak(std::vector<float>(out.begin() + 24000, out.end()));
}
}  // namespace

TEST(clawFilterIsLoudestAtZeroAndDarkensMonotonically) {
  // The Claw's Filter is 1k into 3.3 nF at 0 ohms (48 kHz corner, bright) and 100k into it at full
  // (about 480 Hz): so the level falls with every step clockwise, and is never silent. SPICE of the
  // board: peak 0.558, 0.438, 0.359, 0.303, 0.262 V at 0, .25, .5, .75, 1 (Distortion 10 k, 0.25 V).
  double prev = 1e9;
  for (float f : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f}) {
    const double p = peakAtFilter(f);
    std::printf("  claw: Filter %.2f -> peak %.3f V\n", f, p);
    CHECK(p > 0.1, "Filter %.2f is not silent (%.4f V)", f, p);
    CHECK(p < prev, "Filter %.2f is darker (quieter) than the step before (%.4f >= %.4f)", f, p, prev);
    prev = p;
  }
  CHECK(std::fabs(peakAtFilter(0.0f) - 0.536) < 0.02, "Filter 0 peak matches SPICE-checked 0.536 V (Distortion 0.5)");
}

TEST(clawParamIndexIsNotTheStableId) {
  // Block::set takes the index: the contract's stable id is one higher. Resolve through the contract.
  const ParamDesc* f = findStable(Claw::params(), 2u);
  CHECK(f != nullptr && f->id == Claw::Filter, "stable id 2 is the Filter descriptor");
  CHECK(f->stableId != static_cast<std::uint32_t>(f->id), "Filter's stable id (%u) is not its index (%u)", f->stableId,
        static_cast<unsigned>(f->id));
  // Negative control: set() by stable id lands on the next parameter, which is the failure that read
  // as "silent at Filter 0": the caller's Filter wrote Volume.
  static Claw c;
  Arena none;
  c.prepare(Setup{48000.0, 64}, none);
  c.set(static_cast<ParamId>(f->stableId), 0.0f);
  CHECK(c.get(Claw::Volume) == 0.0f, "writing the stable id 2 moved Volume (the trap), not Filter");
  CHECK(c.get(Claw::Filter) == Claw::kParams[Claw::Filter].def, "Filter itself did not move");
  c.set(f->id, 0.9f);
  CHECK(c.get(Claw::Filter) == 0.9f, "writing the descriptor's id moves Filter");
}

TEST(clawMatchesTheBoardInSpice) {
  const char* dir = std::getenv("SQUATCH_CLAW_SPICE");
  if (!dir || !*dir) {
    std::printf("  claw vs SPICE: skipped (set SQUATCH_CLAW_SPICE to the board's SPICE renders)\n");
    return;
  }
  // plugins/spice_rat.py's CASES: Distortion and Filter as the deck's resistances, and the
  // tolerance each earns. Full distortion is held to less: its edges are a race between the slew
  // limit and the diodes, which the macromodel's input stage shapes and this model does not.
  struct Case {
    double dist, filt, limitDb;
  };
  const Case cases[] = {{100e3, 0, -26}, {10e3, 0, -26}, {1e3, 0, -30}, {100, 0, -19},
                        {1e3, 30e3, -30}, {100, 60e3, -21}, {1e3, 0, -26}, {10e3, 10e3, -26}};
  int seen = 0;
  for (int n = 0; n < 8; ++n) {
    const std::string at = std::string(dir) + "/";
    const std::vector<float> in = readWav(at + "in_" + std::to_string(n) + ".wav");
    const std::vector<float> ref = readWav(at + "out_" + std::to_string(n) + ".wav");
    CHECK(in.size() > 24000 && ref.size() == in.size(), "case %d: renders read (%zu, %zu samples)", n, in.size(),
          ref.size());
    if (in.size() <= 24000 || ref.size() != in.size()) continue;
    ++seen;
    const Case& c = cases[n];
    static Claw b;
    Arena none;
    b.prepare(Setup{48000.0, 64}, none);
    b.set(Claw::Distortion, static_cast<float>(std::log10(100e3 / c.dist) / 3.0));
    b.set(Claw::Filter, static_cast<float>(c.filt / 100e3));
    b.set(Claw::Volume, 1.0f);
    const std::vector<float> y = run(b, in);
    // The oversampling filters delay the block by about 4 samples, which SPICE does not; score it
    // at the lag that suits it best, and require that lag to be that latency.
    int lag = 0;
    double best = 1.0;
    for (int l = 0; l <= 8; ++l) {
      const double e = esrAt(y, ref, 4800, l);
      if (e < best) best = e, lag = l;
    }
    const double got = 10.0 * std::log10(best);
    std::printf("  claw vs SPICE case %d (dist %6.0f, filter %6.0f): %.1f dB at lag %d (limit %.0f dB)\n", n,
                c.dist, c.filt, got, lag, c.limitDb);
    CHECK(got < c.limitDb, "case %d: %.1f dB against a limit of %.0f dB", n, got, c.limitDb);
    CHECK(lag >= 3 && lag <= 6, "case %d: best at lag %d, not the oversampling latency", n, lag);
  }
  CHECK(seen == 8, "all 8 SPICE cases were compared (%d)", seen);
}

TEST(clawFilterSweepMatchesTheBoardInSpice) {
  // SQUATCH_CLAW_SPICE_SWEEP: a directory of SPICE renders of the board (in_N/out_N.wav, N = 0..9) made by the
  // same deck at Distortion 10 k (N 0..4) and 1 k (N 5..9) with Filter 0, .25, .5, .75, 1 of 100 k.
  // Private for the same reason as the cases above; the 2026-10-11 errors are in docs/CLAW-FILTER-2026-10.md.
  const char* dir = std::getenv("SQUATCH_CLAW_SPICE_SWEEP");
  if (!dir || !*dir) {
    std::printf("  claw filter sweep vs SPICE: skipped (set SQUATCH_CLAW_SPICE_SWEEP)\n");
    return;
  }
  for (int n = 0; n < 10; ++n) {
    const std::string at = std::string(dir) + "/";
    const std::vector<float> in = readWav(at + "in_" + std::to_string(n) + ".wav");
    const std::vector<float> ref = readWav(at + "out_" + std::to_string(n) + ".wav");
    CHECK(in.size() > 24000 && ref.size() == in.size(), "sweep %d: renders read", n);
    if (in.size() <= 24000 || ref.size() != in.size()) continue;
    const double dist = n < 5 ? 10e3 : 1e3;
    static Claw b;
    Arena none;
    b.prepare(Setup{48000.0, 64}, none);
    b.set(Claw::Distortion, static_cast<float>(std::log10(100e3 / dist) / 3.0));
    b.set(Claw::Filter, static_cast<float>((n % 5) * 0.25));
    b.set(Claw::Volume, 1.0f);
    const std::vector<float> y = run(b, in);
    double best = 1.0;
    for (int l = 0; l <= 8; ++l) best = std::min(best, esrAt(y, ref, 4800, l));
    double ry = 0.0, rr = 0.0;
    for (size_t i = 4800; i < y.size() - 8; ++i) ry += static_cast<double>(y[i + 4]) * y[i + 4], rr += static_cast<double>(ref[i]) * ref[i];
    const double got = 10.0 * std::log10(best), lvl = 10.0 * std::log10(ry / rr);
    std::printf("  claw filter sweep %d (dist %5.0f, filter %.2f): %.1f dB, level %+.2f dB\n", n, dist, (n % 5) * 0.25, got, lvl);
    CHECK(got < -30.0, "sweep %d: %.1f dB against -30 dB", n, got);
    CHECK(std::fabs(lvl) < 0.3, "sweep %d: level off by %.2f dB", n, lvl);
  }
}
