// SPDX-License-Identifier: MIT
// Real guitar DI: single notes found by onset. Every algorithm runs continuously over the whole
// recording, as a tuner would, and each note is scored afterwards. Real recordings have no
// ground-truth pitch, so every measure here is one that does not need it:
//   - re-pitch consistency: the whole file is resampled by a known ratio (band-limited) and the
//     measured shift of each note's settled pitch is compared with it. Scale errors, octave
//     slips and noise all show up here;
//   - jitter: spread of the reading over 0.5-1.0 s after the onset;
//   - time to a stable reading: until the reading stays within ±1 c / ±0.1 c of the note's own
//     settled value (median over 1.0-1.5 s, per algorithm);
//   - octave slips: settled value more than 50 c from the median of all algorithms;
//   - the pluck's pitch glide, from the Squatch trace (a property of the guitar, not the tuner).
//
//   eval_real <wav> [<wav> ...]        VARIANTS=1 adds Squatch configuration variants
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <future>

#include "measure.hpp"
#include "onsets.hpp"
#include "synth.hpp"
#include "wav.hpp"

using namespace bench;

namespace {

constexpr double kShiftCents = 7.0;  // re-pitch applied for the consistency test
constexpr double kRef = 440.0;       // any fixed reference; differences are what matter
const double kGlideTimes[] = {0.05, 0.1, 0.2, 0.3, 0.5, 0.75};

using AlgoFactory = std::function<std::unique_ptr<Algo>()>;

std::vector<AlgoFactory> algorithms() {
  return {
      [] { return std::make_unique<CoarseAlgo<Yin, Limits::kMaxWindow>>("YIN (12k, median-5)", 12000.0f, true, 0.85f); },
      [] { return std::make_unique<CoarseAlgo<Mpm, Limits::kMaxWindow>>("MPM (12k, median-5)", 12000.0f, true, 0.8f); },
      [] { return makePhaseVocoder("MPM + phase vocoder", 6, true); },
      [] { return std::make_unique<SquatchAlgo>("Squatch strobe (hz, now)", TunerConfig{}); },
      [] { return std::make_unique<SquatchAlgo>("Squatch strobe (steadyHz)", TunerConfig{}, true); },
  };
}

TunerConfig variant(float memory, bool lagComp, int partials, bool fitB) {
  TunerConfig c;
  c.memorySeconds = memory;
  c.fit.lagCompensation = lagComp;
  c.partials = partials;
  c.fit.fitInharmonicity = fitB;
  return c;
}

void addVariants(std::vector<AlgoFactory>& a) {
  a.push_back([] { return std::make_unique<SquatchAlgo>("Squatch steady, memory 0.35 s", variant(0.35f, true, 6, true), true); });
  a.push_back([] { return std::make_unique<SquatchAlgo>("Squatch steady, memory 0.7 s", variant(0.7f, true, 6, true), true); });
  a.push_back([] { return std::make_unique<SquatchAlgo>("Squatch steady, 3 partials", variant(0.5f, true, 3, true), true); });
  a.push_back([] { return std::make_unique<SquatchAlgo>("Squatch steady, no B fit", variant(0.5f, true, 6, false), true); });
}

double medianCents(const Trace& tr, double a, double b, double ref) {
  std::vector<double> v;
  for (size_t i = 0; i < tr.t.size(); ++i)
    if (tr.t[i] >= a && tr.t[i] <= b && tr.e[i].valid) v.push_back(centsErr(tr.e[i], ref));
  return quantile(v, 0.5);
}

struct NoteResult {
  double settled = NAN;  // cents against the 440 Hz grid, median over 1.0-1.5 s
  double shiftErr = NAN;  // measured minus applied re-pitch, cents
  double jitter = NAN;    // sd over 0.5-1.0 s
  double stable1 = -1, stable01 = -1;
  std::vector<double> glide;  // cents above settled at kGlideTimes
};

NoteResult measureNote(const Trace& orig, const Trace& up, double onset, double ratio) {
  NoteResult r;
  const Trace tr = slice(orig, onset, 1.5);
  r.settled = medianCents(tr, 1.0, 1.5, kRef);
  if (std::isnan(r.settled)) return r;
  const double settledHz = kRef * std::exp2(r.settled / 1200.0);
  const Trace tu = slice(up, onset / ratio, 1.5 / ratio);
  r.shiftErr = medianCents(tu, 1.0 / ratio, 1.5 / ratio, kRef) - r.settled - kShiftCents;
  r.jitter = windowStats(tr, [&](double) { return settledHz; }, 0.5, 1.0).sd;
  r.stable1 = lockTime(tr, settledHz, 1.0, 1.5);
  r.stable01 = lockTime(tr, settledHz, 0.1, 1.5);
  for (double t : kGlideTimes) r.glide.push_back(medianCents(tr, t - 0.011, t + 0.011, settledHz));
  return r;
}

struct Recording {
  Audio orig, up;             // the file, and a copy re-pitched by +kShiftCents
  std::vector<double> onsets;  // seconds
};

std::vector<Recording> loadRecordings(const std::vector<std::string>& paths) {
  std::vector<Recording> out;
  for (const std::string& p : paths) {
    Recording r;
    if (!readWav(p, r.orig)) {
      std::fprintf(stderr, "cannot read %s\n", p.c_str());
      continue;
    }
    r.up.rate = r.orig.rate;
    r.up.x = resample(r.orig.x, std::exp2(kShiftCents / 1200.0));
    r.onsets = findOnsets(r.orig.x, r.orig.rate);
    out.push_back(std::move(r));
  }
  return out;
}

std::vector<NoteResult> runAlgo(const AlgoFactory& make, const std::vector<Recording>& recs) {
  const double ratio = std::exp2(kShiftCents / 1200.0);
  std::vector<NoteResult> v;
  for (const Recording& r : recs) {
    auto run = [&](const Audio& a) {
      std::unique_ptr<Algo> alg = make();
      alg->configure(a.rate, 38.0f);
      return runTrace(*alg, a.x, a.rate, 0.0);
    };
    const Trace orig = run(r.orig), up = run(r.up);
    for (double onset : r.onsets) v.push_back(measureNote(orig, up, onset, ratio));
  }
  return v;
}

void printSummary(const std::string& name, const std::vector<NoteResult>& rs, const std::vector<double>& consensus) {
  std::vector<double> shift, absShift, jit, s1, s01;
  int slips = 0, n = 0, never1 = 0, never01 = 0;
  for (size_t i = 0; i < rs.size(); ++i) {
    const NoteResult& r = rs[i];
    if (std::isnan(r.settled) || std::isnan(consensus[i])) continue;
    ++n;
    if (std::fabs(r.settled - consensus[i]) > 50.0) {
      ++slips;
      continue;
    }
    if (!std::isnan(r.shiftErr)) {
      shift.push_back(r.shiftErr);
      absShift.push_back(std::fabs(r.shiftErr));
    }
    jit.push_back(r.jitter);
    if (r.stable1 >= 0) s1.push_back(r.stable1); else ++never1;
    if (r.stable01 >= 0) s01.push_back(r.stable01); else ++never01;
  }
  std::printf("| %s | %d | %d | %.3f | %.3f | %.3f | %.3f | %.0f / %.0f | %d | %.0f / %.0f | %d |\n", name.c_str(), n, slips,
              quantile(shift, 0.5), quantile(absShift, 0.5), quantile(absShift, 0.95), quantile(jit, 0.5),
              1000 * quantile(s1, 0.5), 1000 * quantile(s1, 0.9), never1, 1000 * quantile(s01, 0.5),
              1000 * quantile(s01, 0.9), never01);
}

void printGlide(const std::vector<NoteResult>& sq) {
  std::printf("\nPitch after the pluck relative to the settled pitch (1.0-1.5 s), Squatch strobe trace, cents:\n\n"
              "| after | p10 | p50 | p90 |\n|---|---|---|---|\n");
  for (size_t g = 0; g < sizeof kGlideTimes / sizeof kGlideTimes[0]; ++g) {
    std::vector<double> v;
    for (const auto& r : sq)
      if (r.glide.size() > g && !std::isnan(r.glide[g])) v.push_back(r.glide[g]);
    std::printf("| %.0f ms | %.2f | %.2f | %.2f |\n", 1000 * kGlideTimes[g], quantile(v, 0.1), quantile(v, 0.5),
                quantile(v, 0.9));
  }
}

}  // namespace

int main(int argc, char** argv) {
  const std::vector<std::string> paths(argv + 1, argv + argc);
  const auto recs = loadRecordings(paths);
  size_t notes = 0;
  for (const auto& r : recs) notes += r.onsets.size();
  std::printf("# Squatch Tuner: real DI accuracy%s\n\n"
              "Real DI: %zu notes that ring at least 1.5 s, from %zu file(s); each algorithm runs continuously\n"
              "over the whole file; re-pitch test +%.1f c.\n\n", std::getenv("VARIANTS") ? ", configuration variants" : "", notes, recs.size(), kShiftCents);
  auto algos = algorithms();
  if (std::getenv("VARIANTS")) addVariants(algos);
  std::vector<std::future<std::vector<NoteResult>>> jobs;
  for (const auto& make : algos) jobs.push_back(std::async(std::launch::async, runAlgo, std::cref(make), std::cref(recs)));
  std::vector<std::vector<NoteResult>> res;
  for (auto& j : jobs) res.push_back(j.get());
  std::vector<double> consensus(notes);
  for (size_t i = 0; i < notes; ++i) {
    std::vector<double> v;
    for (const auto& r : res)
      if (!std::isnan(r[i].settled)) v.push_back(r[i].settled);
    consensus[i] = quantile(v, 0.5);
  }
  std::printf("| algorithm | notes | octave/gross slips | re-pitch error median c | abs p50 c | abs p95 c | jitter p50 c | "
              "stable ±1c p50/p90 ms | never | stable ±0.1c p50/p90 ms | never |\n|---|---|---|---|---|---|---|---|---|---|---|\n");
  for (size_t a = 0; a < algos.size(); ++a) printSummary(algos[a]()->name(), res[a], consensus);
  printGlide(res[3]);  // the "now" trace follows the glide
  return 0;
}
