// SPDX-License-Identifier: MIT
// Real-time safety: no allocation, free or lock inside process(), processChannels(), set() or
// get(), for every block, across many random block sizes (always including 1 and maxBlock),
// sample rates, parameter values (out of range and NaN included), in-place calls and, for the
// blocks with a stereo path, stereo. Two deliberately bad blocks prove the watch can see a
// violation at all.
#include <atomic>
#include <mutex>
#include <thread>
#include <squatch/dsp/blocks/cabinet.hpp>
#include <squatch/dsp/blocks/comp.hpp>
#include <squatch/dsp/blocks/claw.hpp>
#include <squatch/dsp/blocks/crunch.hpp>
#include <squatch/dsp/blocks/eq.hpp>
#include <squatch/dsp/blocks/gate.hpp>
#include <squatch/dsp/blocks/hall.hpp>
#include <squatch/dsp/blocks/limiter.hpp>
#include <squatch/dsp/blocks/trail_od.hpp>
#include <squatch/dsp/blocks/tuner.hpp>
#include <squatch/dsp/chain.hpp>

#include "check.hpp"
#include "rt_watch.hpp"
#include "signals.hpp"

using namespace squatch::dsp;

namespace {

constexpr int kMaxBlock = 512;
alignas(64) unsigned char gMem[1 << 20];
volatile float gSink = 0.0f;  // keeps the get() calls from being optimised away

struct Subject {
  const char* name;
  Block& block;
  Span<const ParamDesc> params;
  Layout layout = Layout::Mono;
};

// Every value a host would read after a buffer: parameters for state, meters for display.
float readAll(const Subject& s) {
  float sum = 0.0f;
  for (const ParamDesc& d : s.params) sum += s.block.get(d.id);
  return sum;
}

void run(const Subject& s, float* in, float* dst, int n) {
  if (s.layout == Layout::Mono) return s.block.process(in, dst, n);
  static float right[kMaxBlock], rightOut[kMaxBlock];
  copy(in, right, n);
  const float* ins[2] = {in, right};
  float* outs[2] = {dst, dst == in ? right : rightOut};
  s.block.processChannels(AudioIO{ins, outs, inputChannels(s.layout), 2}, n);
}

float randomValue(const ParamDesc& d, sig::Rng& rng) {
  switch (rng.range(0, 7)) {
    case 0: return NAN;
    case 1: return d.max * 10.0f + 1.0f;
    case 2: return d.min * 10.0f - 1.0f;
    default: return d.min + (d.max - d.min) * 0.5f * (rng.uniform() + 1.0f);
  }
}

int blockSize(int call, int maxBlock, sig::Rng& rng) {
  if (call % 5 == 0) return 1;
  if (call % 5 == 1) return maxBlock;
  return rng.range(1, maxBlock);
}

// One process() call, and sometimes a set() before it, inside the watch.
rt::Counts oneCall(const Subject& s, int call, int maxBlock, sig::Rng& rng) {
  static float in[kMaxBlock], out[kMaxBlock];
  const int n = blockSize(call, maxBlock, rng);
  for (int i = 0; i < n; ++i) in[i] = 2.0f * rng.uniform() * (call % 3 == 0 ? 0.001f : 1.0f);
  const bool setParam = !s.params.empty() && call % 3 == 1;
  const ParamDesc& d = setParam ? s.params[rng.next() % s.params.size()] : ParamDesc{};
  const float v = setParam ? randomValue(d, rng) : 0.0f;
  float* dst = call % 2 ? in : out;
  rt::arm();
  if (setParam) s.block.set(d.id, v);
  run(s, in, dst, n);
  gSink = gSink + readAll(s);
  return rt::disarm();
}

rt::Counts exercise(const Subject& s, double rate, int maxBlock, int calls) {
  Arena mem(gMem, sizeof gMem);
  s.block.prepare(Setup{rate, maxBlock, s.layout}, mem);
  sig::Rng rng{static_cast<std::uint32_t>(rate) ^ static_cast<std::uint32_t>(maxBlock)};
  rt::Counts sum;
  for (int call = 0; call < calls; ++call) {
    const rt::Counts c = oneCall(s, call, maxBlock, rng);
    sum.news += c.news, sum.deletes += c.deletes, sum.mallocs += c.mallocs;
    sum.frees += c.frees, sum.locks += c.locks;
  }
  return sum;
}

rt::Counts sweep(const Subject& s) {
  const double rates[] = {44100.0, 48000.0, 96000.0};
  const int blocks[] = {1, 16, 128, kMaxBlock};
  rt::Counts sum;
  for (double rate : rates)
    for (int mb : blocks) {
      const rt::Counts c = exercise(s, rate, mb, 1500);
      sum.news += c.news, sum.deletes += c.deletes, sum.mallocs += c.mallocs;
      sum.frees += c.frees, sum.locks += c.locks;
    }
  return sum;
}

void report(const char* name, const rt::Counts& c) {
  CHECK(c.total() == 0, "%s: %ld new, %ld delete, %ld malloc, %ld free, %ld lock", name, c.news, c.deletes,
        c.mallocs, c.frees, c.locks);
}

struct Allocates final : Block {
  void prepare(const Setup&, Arena&) override {}
  void process(const float* in, float* out, int n) override {
    static int* volatile sink;
    sink = new int(n);
    out[0] = in[0] + static_cast<float>(*sink);
    delete sink;
  }
  void reset() override {}
  void set(ParamId, float) override {}
  float get(ParamId) const override { return 0.0f; }
};

struct Locks final : Block {
  std::mutex m;
  float v = 0.0f;
  void prepare(const Setup&, Arena&) override {}
  void process(const float* in, float* out, int n) override { copy(in, out, n); }
  void reset() override {}
  void set(ParamId, float value) override {
    std::lock_guard<std::mutex> hold(m);
    v = value;
  }
  float get(ParamId) const override { return v; }
};

const ParamDesc kAnyParam[] = {{0, "x", "", 0.0f, 1.0f, 0.0f, 0.0f}};

}  // namespace

TEST(rtWatchSeesViolations) {
  Allocates a;
  Locks l;
  const rt::Counts ca = exercise({"allocates", a, {}}, 48000.0, 64, 20);
  const rt::Counts cl = exercise({"locks", l, kAnyParam}, 48000.0, 64, 30);
  CHECK(ca.news == 20 && ca.deletes == 20, "operator new/delete seen: %ld/%ld", ca.news, ca.deletes);
  if (rt::mallocMeasured()) CHECK(ca.mallocs == 20 && ca.frees == 20, "malloc seen: %ld", ca.mallocs);
  if (rt::locksMeasured()) CHECK(cl.locks == 10, "locks seen: %ld", cl.locks);
  std::printf("     rt watch: operator new/delete, malloc/free %s, locks %s\n",
              rt::mallocMeasured() ? "measured" : "NOT measured (no sanitizer allocator)",
              rt::locksMeasured() ? "measured" : "NOT measured on this platform");
}

// A loader thread may allocate and lock while the audio thread is watched; only the audio
// thread's own traffic counts.
TEST(rtWatchIgnoresOtherThreads) {
  std::atomic<int> step{0};
  std::thread loader([&] {
    while (step.load() == 0) {
    }
    std::mutex m;
    for (int i = 0; i < 10; ++i) {
      std::lock_guard<std::mutex> hold(m);
      delete new int(i);
    }
    step.store(2);
  });
  rt::arm();
  step.store(1);
  while (step.load() != 2) {
  }
  const rt::Counts c = rt::disarm();
  loader.join();
  CHECK(c.total() == 0, "another thread's traffic was counted: %ld new, %ld lock", c.news, c.locks);
}

TEST(rtBlocksNeverAllocateOrLock) {
  static Limiter limiter;
  static Gate gate;
  static Eq eq;
  static TrailOD trailOd;
  report("limiter", sweep({"limiter", limiter, Limiter::params()}));
  report("trail od", sweep({"trail od", trailOd, TrailOD::params()}));
  report("gate", sweep({"gate", gate, Gate::params()}));
  report("eq", sweep({"eq", eq, Eq::params()}));
  static Compressor comp;
  static Crunch crunch;
  static Hall hall;
  report("comp", sweep({"comp", comp, Compressor::params()}));
  report("crunch", sweep({"crunch", crunch, Crunch::params()}));
  report("hall", sweep({"hall", hall, Hall::params()}));
  static Claw claw;
  report("claw", sweep({"claw", claw, Claw::params()}));
  CHECK(hall.ready(), "the hall's delay memory fit the test arena");
}

TEST(rtCabinetNeverAllocatesOrLocks) {
  static Cabinet cab;
  static float ir[3000];
  sig::Rng rng{11};
  for (float& v : ir) v = 0.1f * rng.uniform();
  cab.configure(ir, 3000);
  report("cabinet", sweep({"cabinet", cab, Cabinet::params()}));
}

TEST(rtTunerNeverAllocatesOrLocks) {
  static TunerBlock tuner;
  report("tuner", sweep({"tuner", tuner, TunerBlock::params()}));
}

TEST(rtStereoNeverAllocatesOrLocks) {
  static Limiter limiter;
  static Gate gate;
  static TunerBlock tuner;
  static Eq eq;
  report("limiter stereo", sweep({"limiter", limiter, Limiter::params(), Layout::Stereo}));
  report("gate stereo", sweep({"gate", gate, Gate::params(), Layout::Stereo}));
  report("tuner stereo", sweep({"tuner", tuner, TunerBlock::params(), Layout::Stereo}));
  report("eq mono to stereo", sweep({"eq", eq, Eq::params(), Layout::MonoToStereo}));
}

TEST(rtChainAndQueueNeverAllocateOrLock) {
  static Gate gate;
  static Eq eq;
  static Limiter limiter;
  static Chain chain;
  static ParamQueue<64> queue;
  if (chain.size() == 0) chain.add(gate), chain.add(eq), chain.add(limiter);
  Arena mem(gMem, sizeof gMem);
  chain.prepare(Setup{48000.0, 128}, mem);
  sig::Rng rng{7};
  static float buf[1000];
  rt::Counts sum;
  for (int call = 0; call < 3000; ++call) {
    const int n = call % 4 == 0 ? 1 : rng.range(1, 1000);  // past maxBlock too: the chain splits
    for (int i = 0; i < n; ++i) buf[i] = rng.uniform();
    queue.push({static_cast<std::uint16_t>(call % 3), static_cast<ParamId>(call % 5), rng.uniform()});
    rt::arm();
    chain.apply(queue);
    chain.process(buf, buf, n);
    const rt::Counts c = rt::disarm();
    sum.news += c.news, sum.mallocs += c.mallocs, sum.locks += c.locks, sum.frees += c.frees;
  }
  report("chain", sum);
}
