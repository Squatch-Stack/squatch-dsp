// SPDX-License-Identifier: MIT
// CPU cost of each block on this machine: nanoseconds per sample at 48 kHz, for block sizes
// 16 to 128, as Markdown. Each figure is the best of 7 runs of 10 s of audio (least disturbed
// by the rest of the machine), with flush-to-zero on, as a host would run it.
//
// A second table uses the measurement method of pedal-science/NAMBench by Rik Hemsley (MIT; see
// protocol.hpp and THIRD_PARTY_NOTICES.md): the tightest 70% of timed passes, core_percent from
// their mean, and the 99th-percentile block as a share of its own deadline. The first table is
// unchanged. `bench long` (make bench-long) times a cabinet over 4k to 240k taps with that method.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <cstdlib>
#include <squatch/dsp/blocks/cabinet.hpp>
#include <squatch/dsp/blocks/comp.hpp>
#include <squatch/dsp/blocks/claw.hpp>
#include <squatch/dsp/blocks/crunch.hpp>
#include <squatch/dsp/blocks/hall.hpp>
#include <squatch/dsp/blocks/trail_od.hpp>
#include <squatch/dsp/blocks/eq.hpp>
#include <squatch/dsp/blocks/gate.hpp>
#include <squatch/dsp/blocks/limiter.hpp>
#include <squatch/dsp/blocks/tuner.hpp>
#include <squatch/dsp/chain.hpp>
#include <squatch/dsp/denormal.hpp>
#include <string>
#include <vector>

#include "../hosts/common/ir_file.hpp"
#include "protocol.hpp"

using namespace squatch::dsp;

namespace {

constexpr double kRate = 48000.0;
constexpr int kSizes[] = {16, 32, 64, 128};
alignas(64) unsigned char gMem[1 << 26];  // 64 MB: a 10 s impulse response's spectra fit
volatile float gSink;

// Noise and a 110 Hz tone at about +6 dBFS peak: the gate is open and the limiter working.
std::vector<float> input() {
  std::vector<float> x(48000);
  unsigned s = 1;
  for (std::size_t i = 0; i < x.size(); ++i) {
    s = s * 1664525u + 1013904223u;
    x[i] = 1.5f * std::sin(6.2831853f * 110.0f * float(i) / 48000.0f) + 0.5f * (float(s >> 8) / 8388608.0f - 1.0f);
  }
  return x;
}

double timeOnce(Block& b, const std::vector<float>& x, int size) {
  static float out[128];
  const int total = int(kRate) * 10;
  const auto start = std::chrono::steady_clock::now();
  for (int done = 0; done < total; done += size) {
    const int at = done % (int(x.size()) - size);
    b.process(&x[at], out, size);
    gSink = out[size - 1];
  }
  const std::chrono::duration<double, std::nano> ns = std::chrono::steady_clock::now() - start;
  return ns.count() / total;
}

double nsPerSample(Block& b, const std::vector<float>& x, int size) {
  Arena mem(gMem, sizeof gMem);
  b.prepare(Setup{kRate, size}, mem);
  DenormalGuard ftz;
  timeOnce(b, x, size);  // warm up
  double best = 1e30;
  for (int run = 0; run < 7; ++run) best = std::min(best, timeOnce(b, x, size));
  return best;
}

// The three blocks in series, run as one Block so the same timing loop applies.
struct Series final : Block {
  Gate gate;
  Eq eq;
  Limiter limiter;
  void prepare(const Setup& s, Arena& mem) override {
    gate.prepare(s, mem), eq.prepare(s, mem), limiter.prepare(s, mem);
  }
  void process(const float* in, float* out, int n) override {
    gate.process(in, out, n), eq.process(out, out, n), limiter.process(out, out, n);
  }
  void reset() override {}
  void set(ParamId, float) override {}
  float get(ParamId) const override { return 0.0f; }
};

void configure(Gate& g, Eq& e, Limiter& l) {
  g.set(Gate::Threshold, -50.0f);
  e.set(Eq::LowGain, 4.0f), e.set(Eq::MidGain, -3.0f), e.set(Eq::HighGain, 2.0f);
  l.set(Limiter::Ceiling, -1.0f);
}

void row(const char* name, Block& b, const std::vector<float>& x) {
  std::printf("| %s |", name);
  for (int size : kSizes) std::printf(" %.2f |", nsPerSample(b, x, size));
  std::printf(" %.2f%% |\n", nsPerSample(b, x, 128) * kRate / 1e7);
}

// ---- the NAMBench method: core_percent and per-block p99 ----

int gPassSeconds = 10;  // audio per timed pass (the long-response table uses fewer)
int gBlockPasses = 20;  // passes with a clock around every block, for the percentile

// One pass over the input; the seconds it took.
double passSeconds(Block& b, const std::vector<float>& x, int size) {
  return timeOnce(b, x, size) * 1e-9 * kRate * gPassSeconds;  // timeOnce is ns per sample
}

// Every block's time as a percent of its own deadline, over gBlockPasses passes, ascending.
std::vector<double> blockPercents(Block& b, const std::vector<float>& x, int size) {
  static float out[128];
  const double deadlineNs = size / kRate * 1e9;
  const int blocks = int(kRate) * gPassSeconds / size;
  std::vector<double> percents;
  percents.reserve(static_cast<std::size_t>(blocks) * gBlockPasses);
  for (int pass = 0; pass < gBlockPasses; ++pass) {
    b.reset();
    for (int k = 0, at = 0; k < blocks; ++k, at = (at + size) % (int(x.size()) - size)) {
      const auto t0 = std::chrono::steady_clock::now();
      b.process(&x[at], out, size);
      const std::chrono::duration<double, std::nano> ns = std::chrono::steady_clock::now() - t0;
      gSink = out[size - 1];
      percents.push_back(ns.count() / deadlineNs * 100.0);
    }
  }
  std::sort(percents.begin(), percents.end());
  return percents;
}

struct Measured {
  bench::Result r;
  std::vector<double> p;  // every block's time, percent of its deadline, ascending
};

Measured methodMeasure(Block& b, const std::vector<float>& x, int size) {
  Arena mem(gMem, sizeof gMem);
  b.prepare(Setup{kRate, size}, mem);
  DenormalGuard ftz;
  // A pass of a cheap block is a few milliseconds, where timer and scheduler jitter exceed 3%:
  // repeat the audio until a pass lasts at least 20 ms.
  const int reps = std::max(1, int(std::ceil(0.02 / passSeconds(b, x, size))));
  const auto pass = [&] {
    double sum = 0.0;
    for (int i = 0; i < reps; ++i) sum += passSeconds(b, x, size);
    return sum;
  };
  Measured m;
  m.r = bench::measure(bench::Config{}, gPassSeconds * reps, pass, [&] { b.reset(); });
  if (m.r.ok) m.p = blockPercents(b, x, size);
  return m;
}

void methodRow(const char* name, Block& b, const std::vector<float>& x, int size) {
  const Measured m = methodMeasure(b, x, size);
  const bench::Result& r = m.r;
  if (!r.ok) return void(std::printf("| %s | %d | no run agreed within 3%% in %d attempts | | | | |\n", name, size, r.attempts));
  std::printf("| %s | %d | %.3f%% | %.3f%% | %.3f%% | %.2f%% | %d |%s\n", name, size, r.corePercent,
              bench::quantileSorted(m.p, 0.99), m.p.back(), r.spread * 100.0, r.attempts, r.hitDeadline ? " cut short" : "");
}

// The pedal end's blocks, set up once and shared by the two tables.
struct Pedal {
  Gate gate;
  Eq eq;
  Limiter limiter;
  Series series;
  TunerBlock tuner;
  Cabinet cab;
  std::vector<float> ir = input();  // 200 ms of noise and tone as a stand-in response
  Pedal() {
    ir.resize(9600);
    cab.configure(ir.data(), 9600);
    configure(gate, eq, limiter);
    configure(series.gate, series.eq, series.limiter);
  }
};

Pedal& pedal() {
  static Pedal p;
  return p;
}

void methodTable(const std::vector<float>& x) {
  Pedal& s = pedal();
  std::printf("\nSame blocks, measured with the NAMBench method (pedal-science_NAMBench, MIT;\n");
  std::printf("see THIRD_PARTY_NOTICES.md). A pass is %d s of audio, repeated until it lasts 20 ms.\n", gPassSeconds);
  std::printf("The kept passes are the tightest 70%% of those timed in 1 s; their spread must be within 3%%,\n");
  std::printf("else the attempt is voided and rerun. core_percent = mean pass time / audio time.\n");
  std::printf("p99 and max are one block's time as a percent of its own deadline (block size / 48 kHz)\n");
  std::printf("over %d passes; above 100%% a callback missed its deadline.\n\n", gBlockPasses);
  std::printf("| block | size | core_percent | block p99 | block max | spread | attempts |\n|---|---:|---:|---:|---:|---:|---:|\n");
  struct { const char* name; Block* b; } rows[] = {{"limiter (limiting)", &s.limiter}, {"gate (open)", &s.gate}, {"eq (3 bands)", &s.eq},
      {"gate + eq + limiter", &s.series}, {"tuner (tracking 110 Hz)", &s.tuner}, {"cabinet (200 ms response, partition 64)", &s.cab}};
  for (const auto& row : rows)
    for (int size : kSizes) methodRow(row.name, *row.b, x, size);
}

// ---- long responses: where the cabinet's engine matters ----

struct Variant {
  const char* name;
  void (*configure)(Cabinet&, const float*, int);
};
// Without the cut filters, so the rows time the convolution engines alone.
const Variant kVariants[] = {
    {"old: partition 64", [](Cabinet& c, const float* ir, int n) { c.configure(ir, n, 64, false, Cabinet::Engine::Partitioned); }},
    {"auto (long plan above 16384)", [](Cabinet& c, const float* ir, int n) { c.configure(ir, n, 64, false); }},
};

std::vector<float> decayingNoise(int taps) {
  std::vector<float> ir(static_cast<std::size_t>(taps));
  unsigned s = 7;
  for (std::size_t i = 0; i < ir.size(); ++i) {
    s = s * 1664525u + 1013904223u;
    ir[i] = (float(s >> 8) / 8388608.0f - 1.0f) * std::exp(-4.0f * float(i) / float(ir.size()));
  }
  return ir;
}

}  // namespace

// `make bench-long`: a cabinet over 4k to 240k taps (85 ms to 5 s at 48 kHz), the NAMBench method only.
void longTable(const char* cpu, const char* compiler) {
  gPassSeconds = 2, gBlockPasses = 6;
  const std::vector<float> ir = decayingNoise(240000), x = input();
  const char* flags = std::getenv("BENCH_FLAGS");
  const char* notes = std::getenv("BENCH_NOTES");
  std::printf("<!-- SPDX-License-Identifier: MIT -->\n# squatch-dsp cabinet benchmark, long responses\n\n");
  std::printf("%s, %s,\n`%s`. `make bench-long` writes this file.\n\n", cpu, compiler, flags ? flags : "-O3 -fno-exceptions -fno-rtti");
  if (notes && *notes) std::printf("%s\n\n", notes);
  std::printf("The NAMBench method as in bench.md (see THIRD_PARTY_NOTICES.md), with passes of %d s of audio and %d\n", gPassSeconds, gBlockPasses);
  std::printf("passes for the block percentile. core_percent is the share of one core; p99 and max are one block's\n");
  std::printf("time as a percent of its own deadline (above 100%% a callback was late). The cut filters are off.\n\n");
  std::printf("| engine | taps | size | core_percent | block p99 | block max | spread | attempts |\n|---|---:|---:|---:|---:|---:|---:|---:|\n");
  for (int taps : {4096, 16384, 48000, 240000})
    for (const Variant& v : kVariants)
      for (int size : {32, 128}) {
        static Cabinet cab;
        v.configure(cab, ir.data(), taps);
        char name[64];
        std::snprintf(name, sizeof name, "%s | %d", v.name, taps);
        methodRow(name, cab, x, size);
      }
}

void header(int argc, char** argv) {
  char date[16];
  const std::time_t now = std::time(nullptr);
  std::strftime(date, sizeof date, "%Y-%m-%d", std::localtime(&now));
  std::printf("<!-- SPDX-License-Identifier: MIT -->\n# squatch-dsp CPU benchmark\n\n");
  const char* flags = std::getenv("BENCH_FLAGS");
  const char* notes = std::getenv("BENCH_NOTES");
  std::printf("%s, %s, `%s`, %s.\n`make bench` writes this file.\n\n", argc > 1 ? argv[1] : "unknown CPU",
              argc > 2 ? argv[2] : "unknown compiler", flags ? flags : "-O3 -fno-exceptions -fno-rtti", date);
  if (notes && *notes) std::printf("%s\n\n", notes);
  std::printf("Nanoseconds per sample at 48 kHz, by block size (best of 7 runs of 10 s of audio, one core).\n");
  std::printf("At 48 kHz one core has 20833 ns per sample; the last column is that share at 128.\n\n");
  std::printf("| block | 16 | 32 | 64 | 128 | %% of a core |\n|---|---:|---:|---:|---:|---:|\n");
}

// The pedal end: dynamics, the tuner and the cabinet.
void pedalRows(const std::vector<float>& x) {
  Pedal& p = pedal();
  row("limiter (limiting)", p.limiter, x);
  row("gate (open)", p.gate, x);
  row("eq (3 bands)", p.eq, x);
  row("gate + eq + limiter", p.series, x);
  row("tuner (tracking 110 Hz)", p.tuner, x);
  row("cabinet (200 ms response, partition 64)", p.cab, x);
}

// The amp end, and the algorithmic room.
void ampRows(const std::vector<float>& x) {
  static Compressor comp;
  static TrailOD od;
  static Crunch crunch;
  static Hall hall;
  static Claw claw;
  comp.set(Compressor::Threshold, -24.0f);
  od.set(TrailOD::Drive, 0.7f);
  crunch.set(Crunch::Gain, 0.75f);
  hall.set(Hall::Mix, 0.25f);
  claw.set(Claw::Distortion, 0.8f);
  row("comp (4:1)", comp, x);
  row("trail od (4x oversampled)", od, x);
  row("crunch (high-gain preamp, 4x oversampled)", crunch, x);
  row("claw (op-amp distortion circuit model, 4x oversampled)", claw, x);
  row("hall (FDN reverb, 8 lines)", hall, x);
}

// A real impulse response, when the environment names one (never checked in: third-party files
// stay outside the repository), run at `partition`.
void irRow(const std::vector<float>& x, const char* env, const char* what, int partition) {
  const char* path = std::getenv(env);
  if (!path) return;
  static std::vector<std::vector<float>> keep;
  const squatch::host::Response r = squatch::host::loadResponse(path, kRate, 60000.0, true);
  if (!r.error.empty()) return (void)std::printf("| %s | %s |\n", what, r.error.c_str());
  keep.push_back(r.taps);
  static Cabinet cabs[4];
  static int used = 0;
  Cabinet& c = cabs[used++];
  c.configure(keep.back().data(), int(keep.back().size()), partition);
  char name[160];
  std::snprintf(name, sizeof name, "%s (%.2f s, %zu taps, partition %d)", what, keep.back().size() / kRate,
                keep.back().size(), partition);
  row(name, c, x);
}

int main(int argc, char** argv) {
  if (argc > 3 && std::string(argv[3]) == "long") return longTable(argv[1], argv[2]), 0;
  const std::vector<float> x = input();
  header(argc, argv);
  pedalRows(x);
  ampRows(x);
  // SQUATCH_BENCH_CAB: a cabinet IR; SQUATCH_BENCH_LONG_IR: a reverb/delay IR, at the default
  // partition and at 1024.
  irRow(x, "SQUATCH_BENCH_CAB", "cab IR", 64);
  irRow(x, "SQUATCH_BENCH_LONG_IR", "reverb IR", 64);
  irRow(x, "SQUATCH_BENCH_LONG_IR", "reverb IR", 1024);
  methodTable(x);
  return 0;
}
