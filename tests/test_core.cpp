// SPDX-License-Identifier: MIT
// The core: arena, smoother, parameter descriptors, spans, queue, denormals, the snapshot handoff
// and the chain.
#include <cstdint>
#include <squatch/dsp/arena.hpp>
#include <squatch/dsp/chain.hpp>
#include <squatch/dsp/denormal.hpp>
#include <squatch/dsp/latest.hpp>
#include <squatch/dsp/param.hpp>
#include <squatch/dsp/param_queue.hpp>

#include "check.hpp"
#include "signals.hpp"

using namespace squatch::dsp;

TEST(arenaAlignsZeroesAndCounts) {
  alignas(64) static unsigned char mem[256];
  for (unsigned char& b : mem) b = 0xAB;
  Arena a(mem, sizeof mem);
  char* c = a.alloc<char>(3);
  float* f = a.alloc<float>(10);
  CHECK(c && f, "allocations inside capacity");
  CHECK(reinterpret_cast<std::uintptr_t>(f) % Arena::kAlign == 0, "aligned to %zu", Arena::kAlign);
  bool zero = true;
  for (int i = 0; i < 10; ++i) zero = zero && f[i] == 0.0f;
  CHECK(zero, "memory is zeroed");
  CHECK(a.used() == 16 + 40 && a.required() == a.used() && !a.failed(), "used %zu", a.used());
}

TEST(arenaReportsExhaustionAtPrepareTime) {
  alignas(64) static unsigned char mem[64];
  Arena a(mem, sizeof mem);
  CHECK(a.alloc<float>(8) != nullptr, "first fits");
  CHECK(a.alloc<float>(16) == nullptr && a.failed(), "second does not");
  CHECK(a.alloc<float>(1) == nullptr, "nothing after a failure: the host must not start");
  CHECK(a.required() >= 32 + 64 + 4, "required %zu covers every request", a.required());
  a.rewind();
  CHECK(!a.failed() && a.used() == 0, "rewind forgets");
  CHECK(Arena().alloc<float>(1) == nullptr, "a measuring arena hands out nothing");
  CHECK(a.alloc<double>(SIZE_MAX / 4) == nullptr && a.failed(), "overflowing counts fail cleanly");
}

TEST(smootherArrivesExactly) {
  Smoother s;
  s.prepare(1000.0, 10.0f);  // 10 steps
  s.jump(0.0f);
  s.setTarget(1.0f);
  float v = 0.0f;
  for (int i = 0; i < 9; ++i) v = s.next();
  CHECK(s.smoothing() && std::fabs(v - 0.9f) < 1e-6f, "after 9 steps %f", v);
  CHECK(s.next() == 1.0f && !s.smoothing(), "exactly the target after 10");
  s.setTarget(3.0f);
  CHECK(std::fabs(s.advance(5) - 2.0f) < 1e-6f, "advance halfway %f", s.value());
  CHECK(s.advance(100) == 3.0f && !s.smoothing(), "advance past the end lands");
  Smoother instant;
  instant.prepare(48000.0, 0.0f);
  instant.setTarget(5.0f);
  CHECK(instant.value() == 5.0f && !instant.smoothing(), "no smoothing time jumps");
}

TEST(paramClampsAndFinds) {
  const ParamDesc ps[] = {{7, "Level", "dB", -60.0f, 6.0f, -12.0f, 10.0f}};
  CHECK(ps[0].clamp(100.0f) == 6.0f && ps[0].clamp(-100.0f) == -60.0f, "clamped to range");
  CHECK(ps[0].clamp(NAN) == -12.0f, "NaN becomes the default");
  CHECK(findParam(ps, 7) == &ps[0] && findParam(ps, 8) == nullptr, "find by id");
}

TEST(spanHelpers) {
  float a[5] = {1, -4, 2, 0, 3};
  Span<float> s(a);
  Span<const float> cs = s;
  CHECK(cs.size() == 5 && cs[1] == -4.0f, "span views the array");
  CHECK(peakAbs(a, 5) == 4.0f, "peak");
  copy(a, a + 1, 3);  // overlapping
  CHECK(a[1] == 1.0f && a[2] == -4.0f && a[3] == 2.0f, "copy handles overlap");
  clear(a, 5);
  CHECK(peakAbs(a, 5) == 0.0f, "clear");
  CHECK(std::fabs(dbToGain(-6.0205999f) - 0.5f) < 1e-6f && std::fabs(gainToDb(2.0f) - 6.0206f) < 1e-4f, "dB");
}

TEST(queueIsFifoAndBounded) {
  SpscQueue<int, 8> q;
  int v = 0;
  CHECK(!q.pop(v), "starts empty");
  for (int round = 0; round < 3; ++round) {  // wraps the ring
    for (int i = 0; i < 8; ++i) CHECK(q.push(round * 10 + i), "push %d", i);
    CHECK(!q.push(99), "full refuses");
    for (int i = 0; i < 8; ++i) CHECK(q.pop(v) && v == round * 10 + i, "fifo %d", v);
    CHECK(!q.pop(v), "empty again");
  }
}

TEST(denormalGuardFlushes) {
  volatile float tiny = 1e-38f, scale = 1e-3f;
  CHECK(tiny * scale != 0.0f, "without the guard the product is a denormal");
  {
    DenormalGuard g;
    if (DenormalGuard::supported()) CHECK(tiny * scale == 0.0f, "flushed to zero under the guard");
  }
  CHECK(tiny * scale != 0.0f, "the guard restores the previous mode");
  CHECK(flushDenormal(1e-35f) == 0.0f && flushDenormal(1e-20f) == 1e-20f, "flush helper");
}

namespace {

// Two fields, so a value the producer is writing shows up as a half-written one.
struct Reading {
  long seq = 0;
  long twice = 0;
};

bool intact(const Reading& r, long seq) { return r.seq == seq && r.twice == 2 * seq; }

// Adds a constant and reports a latency: enough to test the chain's plumbing.
struct Offset final : Block {
  float add = 0.0f;
  int delay = 0;
  int calls = 0, largest = 0;
  void prepare(const Setup&, Arena&) override {}
  void process(const float* in, float* out, int n) override {
    for (int i = 0; i < n; ++i) out[i] = in[i] + add;
    ++calls;
    largest = n > largest ? n : largest;
  }
  void reset() override { calls = 0; }
  void set(ParamId, float v) override { add = v; }
  float get(ParamId) const override { return add; }
  int latency() const override { return delay; }
};

}  // namespace

TEST(chainRunsInSeriesAndSplitsLongCalls) {
  Offset a, b;
  a.add = 1.0f, b.add = 10.0f, a.delay = 3, b.delay = 4;
  Chain c;
  CHECK(c.add(a) && c.add(b), "add");
  Arena none;
  c.prepare(Setup{48000.0, 32}, none);
  float in[100], out[100];
  for (int i = 0; i < 100; ++i) in[i] = float(i);
  c.process(in, out, 100);
  CHECK(out[0] == 11.0f && out[99] == 110.0f, "series: %f %f", out[0], out[99]);
  CHECK(a.largest == 32 && a.calls == 4, "split into maxBlock pieces: %d calls, largest %d", a.calls, a.largest);
  CHECK(c.latency() == 7, "latencies add: %d", c.latency());
  c.process(out, out, 100);
  CHECK(out[5] == 27.0f, "in place");
  CHECK(c.set(1, 0, -10.0f) && !c.set(2, 0, 1.0f) && !c.set(-1, 0, 1.0f), "set by index");
  ParamQueue<16> q;
  q.push({0, 0, 5.0f});
  c.apply(q);
  CHECK(a.add == 5.0f, "queued change applied");
  Chain empty;
  empty.process(in, out, 100);
  CHECK(out[42] == 42.0f, "an empty chain passes through");
}

TEST(latestHandsOverTheNewestValueWhole) {
  Latest<Reading> box;
  CHECK(!box.update(), "nothing to take before the first publish");
  box.back() = Reading{1, 2};
  box.publish();
  CHECK(box.update(), "a published value is there to take");
  CHECK(intact(box.front(), 1), "the value that was published");
  CHECK(!box.update(), "the same value is not taken twice");
  const Reading& held = box.front();  // what a display is showing
  for (long i = 2; i <= 5; ++i) box.back() = Reading{i, 2 * i}, box.publish();  // read too slowly to keep up
  CHECK(intact(held, 1), "the consumer's value is untouched while the producer publishes");
  CHECK(box.update() && intact(box.front(), 5), "only the newest survives");
  box.back() = Reading{6, 12};
  box.publish();
  CHECK(intact(held, 1), "still untouched: the producer is not writing the slot in front");
  CHECK(box.update() && intact(box.front(), 6), "and the next value arrives whole");
}

TEST(latestNeverGoesBackwards) {
  Latest<Reading> box;
  long last = 0;
  int torn = 0, stale = 0;
  for (long i = 1; i <= 20000; ++i) {  // a display that takes a value only every fifth publish
    box.back() = Reading{i, 2 * i};
    box.publish();
    if (i % 5 || !box.update()) continue;
    const Reading& f = box.front();
    torn += f.twice != 2 * f.seq;              // half written
    stale += f.seq <= last || f.seq > i;       // older than the one before it, or not published yet
    last = f.seq;
  }
  CHECK(torn == 0, "%d values arrived half written", torn);
  CHECK(stale == 0, "%d values older than the last one, or from the future", stale);
  CHECK(last == 20000, "the last publish is the last value taken (%ld)", last);
}
