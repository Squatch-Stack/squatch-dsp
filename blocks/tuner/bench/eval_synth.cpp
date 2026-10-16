// SPDX-License-Identifier: MIT
// Synthetic accuracy and lock-time matrix. Prints Markdown tables; every number is measured.
//
//   eval_synth [trials-per-note] [--full-rate]
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <future>
#include <random>

#include "measure.hpp"
#include "synth.hpp"

using namespace bench;

namespace {

constexpr float kFs = 48000.0f;

struct Scenario {
  const char* name;
  const char* what;
  std::function<void(ToneSpec&, std::mt19937&)> shape;
  double steadyFrom = 0.5, until = 1.5;  // steady-state window, seconds after the pluck
};

double logUniform(std::mt19937& r, double a, double b) {
  return std::exp(std::uniform_real_distribution<double>(std::log(a), std::log(b))(r));
}

// A realistic electric-guitar pluck: stiff string, pluck/pickup comb, decay, a little glide.
void guitar(ToneSpec& s, std::mt19937& r) {
  std::uniform_real_distribution<double> u(0.0, 1.0);
  s.inharmonicity = logUniform(r, 3e-5, 3e-4);
  s.pluckPos = 0.12 + 0.18 * u(r);
  s.pickupPos = 0.08 + 0.17 * u(r);
  s.t60 = 3.0 + 5.0 * u(r);
  s.glideCents = 3.0;
  s.snrDb = 60.0;
  s.pickNoiseDb = 0.0;
}

std::vector<Scenario> scenarios() {
  return {
      {"clean", "harmonic, no glide, 60 dB SNR", [](ToneSpec& s, std::mt19937&) { s.snrDb = 60.0; }},
      {"guitar", "B 3e-5..3e-4, pluck/pickup comb, T60 3-8 s, 3 c glide, 60 dB SNR, pick noise", guitar},
      {"weak-fund", "guitar with the fundamental 20 dB down",
       [](ToneSpec& s, std::mt19937& r) { guitar(s, r); s.fundamentalGainDb = -20.0; }},
      {"noisy", "guitar at 30 dB SNR plus 60/180 Hz hum at -30 dB",
       [](ToneSpec& s, std::mt19937& r) { guitar(s, r); s.snrDb = 30.0; s.humDb = -30.0; }},
      {"hard-pluck", "guitar with a 15 cent pitch glide",
       [](ToneSpec& s, std::mt19937& r) { guitar(s, r); s.glideCents = 15.0; }},
      {"short", "guitar with T60 0.6 s (dead string, high fret); window 0.25-0.5 s",
       [](ToneSpec& s, std::mt19937& r) { guitar(s, r); s.t60 = 0.6; }, 0.25, 0.5},
  };
}

struct Note {
  const char* name;
  double hz;
};

const std::vector<Note>& notes() {
  static const std::vector<Note> n = {{"E1", 41.2034}, {"F#1", 46.2493}, {"B1", 61.7354}, {"D2", 73.4162},
                                      {"E2", 82.4069}, {"A2", 110.0},     {"D3", 146.832}, {"G3", 195.998},
                                      {"B3", 246.942}, {"E4", 329.628},   {"E5", 659.255}, {"E6", 1318.51}};
  return n;
}

using AlgoFactory = std::function<std::unique_ptr<Algo>()>;

std::vector<AlgoFactory> algorithms(bool fullRate) {
  std::vector<AlgoFactory> a = {
      [] { return std::make_unique<CoarseAlgo<Yin, Limits::kMaxWindow>>("YIN (12k, median-5)", 12000.0f, true, 0.85f); },
      [] { return std::make_unique<CoarseAlgo<Mpm, Limits::kMaxWindow>>("MPM (12k, median-5)", 12000.0f, true, 0.8f); },
      [] { return makePhaseVocoder("MPM + phase vocoder", 6, true); },
      [] { return std::make_unique<SquatchAlgo>("Squatch strobe (hz, now)", TunerConfig{}); },
      [] { return std::make_unique<SquatchAlgo>("Squatch strobe (steadyHz)", TunerConfig{}, true); },
      [] {
        TunerConfig c;
        c.fit.fitInharmonicity = false;
        return std::make_unique<SquatchAlgo>("Squatch strobe, no B fit", c);
      },
      [] {
        TunerConfig c;
        c.fit.lagCompensation = false;
        return std::make_unique<SquatchAlgo>("Squatch strobe, line fit only", c);
      },
  };
  if (std::getenv("BOXCAR")) {
    a.push_back([] {
      TunerConfig c;
      c.minBoxcarSeconds = static_cast<float>(std::atof(std::getenv("BOXCAR")));
      return std::make_unique<SquatchAlgo>(std::string("Squatch strobe, boxcar >= ") + std::getenv("BOXCAR") + " s", c);
    });
  }
  if (fullRate) {
    a.push_back([] { return std::make_unique<CoarseAlgo<YinT<2048>, 8192>>("YIN (48k, median-5)", 48000.0f, true, 0.85f); });
    a.push_back([] { return std::make_unique<CoarseAlgo<MpmT<2048>, 8192>>("MPM (48k, median-5)", 48000.0f, true, 0.8f); });
  }
  return a;
}

struct Cell {
  Summary all;
  std::vector<Summary> perNote;
};

Cell runCell(const Scenario& sc, const AlgoFactory& make, int trials) {
  Cell cell;
  cell.perNote.resize(notes().size());
  for (size_t ni = 0; ni < notes().size(); ++ni) {
    for (int t = 0; t < trials; ++t) {
      std::mt19937 rng(static_cast<unsigned>(1000003u * ni + 7919u * t + std::strlen(sc.name)));
      ToneSpec s;
      s.f0 = notes()[ni].hz * std::exp2(std::uniform_real_distribution<double>(-50.0, 50.0)(rng) / 1200.0);
      sc.shape(s, rng);
      const std::vector<float> x = synthesize(s, kFs, rng);
      std::unique_ptr<Algo> a = make();
      a->configure(kFs, 38.0f);
      const Trace tr = runTrace(*a, x, kFs, s.preroll);
      const Truth truth = [&s](double t) { return truthAt(s, t); };
      cell.all.add(tr, truth, s.f0, sc.steadyFrom, sc.until);
      cell.perNote[ni].add(tr, truth, s.f0, sc.steadyFrom, sc.until);
    }
  }
  return cell;
}

std::string ms(const std::vector<double>& v, double q) {
  char b[32];
  if (v.empty()) return "-";
  std::snprintf(b, sizeof b, "%.0f", 1000.0 * quantile(v, q));
  return b;
}

void printRow(const std::string& name, const Summary& s) {
  const double n = s.trials();
  std::printf("| %s | %.3f | %.3f | %.3f | %.3f | %s / %s | %.0f%% | %s / %s | %.0f%% | %.2f%% | %.0f%% |\n",
              name.c_str(), s.bias(), s.rms(), s.p95abs(), quantile(s.jitter, 0.5), ms(s.lock1, 0.5).c_str(),
              ms(s.lock1, 0.9).c_str(), 100.0 * s.never1 / n, ms(s.lock01, 0.5).c_str(), ms(s.lock01, 0.9).c_str(),
              100.0 * s.never01 / n, 100.0 * s.mean(s.gross), 100.0 * s.mean(s.coverage));
}

void printHeader() {
  std::printf("| algorithm | bias c | RMS c | p95 abs c | jitter c | lock ±1c p50/p90 ms | never ±1c | "
              "lock ±0.1c p50/p90 ms | never ±0.1c | gross >50c | coverage |\n");
  std::printf("|---|---|---|---|---|---|---|---|---|---|---|\n");
}

void printPerNote(const char* scenario, const std::vector<std::string>& names, const std::vector<Cell>& cells) {
  std::printf("\n### Per note (%s): RMS steady error c / lock ±1 c p50 ms\n\n| note |", scenario);
  for (const auto& n : names) std::printf(" %s |", n.c_str());
  std::printf("\n|---|");
  for (size_t i = 0; i < names.size(); ++i) std::printf("---|");
  std::printf("\n");
  for (size_t ni = 0; ni < notes().size(); ++ni) {
    std::printf("| %s |", notes()[ni].name);
    for (const Cell& c : cells) std::printf(" %.3f / %s |", c.perNote[ni].rms(), ms(c.perNote[ni].lock1, 0.5).c_str());
    std::printf("\n");
  }
}

}  // namespace

int main(int argc, char** argv) {
  const int trials = argc > 1 ? std::atoi(argv[1]) : 8;
  const bool fullRate = argc > 2 && !std::strcmp(argv[2], "--full-rate");
  const auto algos = algorithms(fullRate);
  const bool perNoteAll = std::getenv("PER_NOTE") != nullptr;
  std::printf("# Squatch Tuner: synthetic accuracy\n\n"
              "Synthetic matrix: %zu notes E1..E6 x %d trials, detune uniform ±50 c, 48 kHz input.\n"
              "Steady window 0.5-1.5 s after the pluck unless stated.\n",
              notes().size(), trials);
  for (const Scenario& sc : scenarios()) {
    std::vector<std::future<Cell>> jobs;
    for (const auto& make : algos) jobs.push_back(std::async(std::launch::async, runCell, std::cref(sc), std::cref(make), trials));
    std::vector<Cell> cells;
    std::vector<std::string> names;
    for (size_t i = 0; i < jobs.size(); ++i) {
      cells.push_back(jobs[i].get());
      names.push_back(algos[i]()->name());
    }
    std::printf("\n## %s: %s\n\n", sc.name, sc.what);
    printHeader();
    for (size_t i = 0; i < cells.size(); ++i) printRow(names[i], cells[i].all);
    if (perNoteAll || !std::strcmp(sc.name, "guitar")) printPerNote(sc.name, names, cells);
    std::fflush(stdout);
  }
  return 0;
}
