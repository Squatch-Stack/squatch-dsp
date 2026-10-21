// SPDX-License-Identifier: MIT
// Long impulse responses: the FFT cut into steps, the TailConvolver and the Cabinet's long plan,
// each against direct convolution in double. The plans the table gives (up to 240000 taps) are
// checked with sparse responses, whose reference costs a few hundred multiplies per sample, with
// taps placed on every tier boundary. Dense responses are checked on small tiers. Plus: the
// plan table, no latency, no allocation or lock in process, the job done by the time it falls
// due, and silence when the arena is too small. The Cabinet is configured without its cut filters
// except in the test that checks the two features together.
#include <squatch/dsp/blocks/cabinet.hpp>
#include <squatch/dsp/fft.hpp>
#include <squatch/dsp/tail_convolver.hpp>

#include "check.hpp"
#include "rt_watch.hpp"
#include "signals.hpp"

using namespace squatch::dsp;

namespace {

constexpr double kRate = 48000.0;
alignas(64) unsigned char gMem[1 << 22];
alignas(64) unsigned char gBig[24 << 20];

struct Tap {
  int at;
  float value;
};

// y[t] = sum of value * x[t - at], for the taps given.
std::vector<double> convolveSparse(const std::vector<float>& x, const std::vector<Tap>& taps) {
  std::vector<double> y(x.size(), 0.0);
  for (const Tap& tap : taps)
    for (std::size_t t = static_cast<std::size_t>(tap.at); t < x.size(); ++t) y[t] += double(tap.value) * x[t - static_cast<std::size_t>(tap.at)];
  return y;
}

std::vector<Tap> allTaps(const std::vector<float>& h, int from = 0) {
  std::vector<Tap> taps;
  for (int k = from; k < static_cast<int>(h.size()); ++k) taps.push_back({k, h[static_cast<std::size_t>(k)]});
  return taps;
}

std::vector<float> dense(int length, std::uint32_t seed) {
  std::vector<float> h = sig::noise(length, 0.5f, seed);
  for (int i = 0; i < length; ++i) h[static_cast<std::size_t>(i)] *= static_cast<float>(std::exp(-5.0 * i / length));
  return h;
}

// Tap positions that matter for a plan: the ends of the head, of the first tier, the tail's blocks.
std::vector<float> sparse(int length, int tail, std::uint32_t seed, std::vector<Tap>* taps) {
  std::vector<float> h(static_cast<std::size_t>(length), 0.0f);
  sig::Rng rng{seed};
  std::vector<int> at = {0, 1, 1023, 1024, 1025, length - 1, length - 2};
  if (tail > 0) {
    for (int base : {2 * tail, 3 * tail, 4 * tail})
      if (base + 1 < length) at.insert(at.end(), {base - 1, base, base + 1});
  }
  for (int i = 0; i < 70; ++i) at.push_back(rng.range(0, length - 1));
  for (int k : at) {
    if (k < 0 || k >= length) continue;
    h[static_cast<std::size_t>(k)] = 0.1f + 0.8f * float(rng.range(0, 1000)) / 1000.0f;
  }
  for (int k = 0; k < length; ++k)
    if (h[static_cast<std::size_t>(k)] != 0.0f) taps->push_back({k, h[static_cast<std::size_t>(k)]});
  return h;
}

double worstError(const std::vector<float>& y, const std::vector<double>& ref) {
  double worst = 0.0, peak = 1e-9;
  for (std::size_t i = 0; i < y.size(); ++i) worst = std::fmax(worst, std::fabs(y[i] - ref[i])), peak = std::fmax(peak, std::fabs(ref[i]));
  return worst / peak;
}

// Random call sizes up to maxBlock, alternately in place.
template <class Block>
std::vector<float> run(Block& block, const std::vector<float>& x, int maxBlock, std::uint32_t seed) {
  sig::Rng rng{seed};
  std::vector<float> y(x.size());
  for (std::size_t done = 0; done < x.size();) {
    const int n = std::min<int>(rng.range(1, maxBlock), static_cast<int>(x.size() - done));
    if (rng.next() % 2) {
      copy(&x[done], &y[done], n);
      block.process(&y[done], &y[done], n);
    } else {
      block.process(&x[done], &y[done], n);
    }
    done += static_cast<std::size_t>(n);
  }
  return y;
}

// The tail alone, called as a host would: ticks, then work() for the call.
struct TailRun {
  std::vector<float> y;
  long forced = 0;
};

TailRun runTail(TailConvolver& tail, const std::vector<float>& x, int maxBlock, std::uint32_t seed, bool doWork = true) {
  sig::Rng rng{seed};
  TailRun r;
  r.y.resize(x.size());
  for (std::size_t done = 0; done < x.size();) {
    const int n = std::min<int>(rng.range(1, maxBlock), static_cast<int>(x.size() - done));
    for (int i = 0; i < n; ++i) r.y[done + static_cast<std::size_t>(i)] = tail.tick(x[done + static_cast<std::size_t>(i)]);
    if (doWork) tail.work(n);
    done += static_cast<std::size_t>(n);
  }
  r.forced = tail.forced();
  return r;
}

// The transform run step by step in pieces cut at random places, or with one butterfly left out.
struct Stepping {
  float sign;
  std::uint32_t seed;
  bool skipOne;
};

void steppedFft(const Fft& fft, std::vector<float>& re, std::vector<float>& im, Stepping how) {
  const float sign = how.sign;
  const bool skipOne = how.skipOne;
  sig::Rng rng{how.seed};
  for (int step = 0; step <= fft.bits(); ++step) {
    const int units = fft.stepUnits(step) - (skipOne && step == 2 ? 1 : 0);
    for (int at = 0; at < units;) {
      const int to = skipOne ? units : std::min(units, at + rng.range(1, 37));
      fft.run(re.data(), im.data(), sign, step, {at, to});
      at = to;
    }
  }
}

void checkSteppedFft(int size, float sign) {
  Arena mem(gMem, sizeof gMem);
  Fft fft;
  CHECK(fft.prepare(size, mem), "size %d", size);
  const std::vector<float> re0 = sig::noise(size, 1.0f, 3u + size), im0 = sig::noise(size, 1.0f, 8u + size);
  std::vector<float> wantRe = re0, wantIm = im0;
  sign > 0 ? fft.forward(wantRe.data(), wantIm.data()) : fft.inverse(wantRe.data(), wantIm.data());
  std::vector<float> re = re0, im = im0;
  steppedFft(fft, re, im, {sign, 7u + size, false});
  CHECK(re == wantRe && im == wantIm, "size %d sign %.0f: stepped transform differs from forward/inverse", size, sign);
  if (size < 8) return;
  re = re0, im = im0;  // negative control: leave one butterfly out and the comparison must notice
  steppedFft(fft, re, im, {sign, 7u + size, true});
  CHECK(re != wantRe || im != wantIm, "size %d: a skipped butterfly changes the result", size);
}

}  // namespace

TEST(fftStepsGiveTheSameTransform) {
  for (int size = 2; size <= 4096; size *= 2)
    for (float sign : {1.0f, -1.0f}) checkSteppedFft(size, sign);
}


TEST(tailConvolverMatchesDirectConvolution) {
  static TailConvolver tail;
  double worst = 0.0;
  for (int block : {2, 8, 64, 256}) {
    for (int extra : {1, block - 1, block, block + 1, 2 * block + 3, 5 * block - 2}) {
      const int length = 2 * block + extra;
      const std::vector<float> h = dense(length, 41u + static_cast<std::uint32_t>(length));
      const std::vector<float> x = sig::noise(14 * block + 90, 0.8f, 5u + static_cast<std::uint32_t>(length));
      const std::vector<double> ref = convolveSparse(x, allTaps(h, 2 * block));
      for (int maxBlock : {1, 7, 3 * block}) {
        Arena mem(gMem, sizeof gMem);
        CHECK(tail.prepare(h.data(), length, block, mem), "block %d length %d", block, length);
        const TailRun r = runTail(tail, x, maxBlock, 9u + static_cast<std::uint32_t>(maxBlock));
        const double e = worstError(r.y, ref);
        CHECK(e < 3e-6, "block %d, length %d, calls up to %d: error %.3g of peak", block, length, maxBlock, e);
        worst = std::fmax(worst, e);
      }
    }
  }
  std::printf("     tail convolver vs direct convolution in double: worst error %.3g of peak\n", worst);
}

TEST(tailJobsFinishBeforeTheyFallDue) {
  static TailConvolver tail;
  const int block = 256, length = 2 * block + 9 * block;
  const std::vector<float> h = dense(length, 77);
  const std::vector<float> x = sig::noise(40 * block, 0.8f, 78);
  const std::vector<double> ref = convolveSparse(x, allTaps(h, 2 * block));
  for (int calls : {1, 16, 64, 255}) {
    Arena mem(gMem, sizeof gMem);
    CHECK(tail.prepare(h.data(), length, block, mem), "prepare");
    std::vector<float> y(x.size());
    for (std::size_t done = 0; done < x.size();) {
      const int n = std::min<int>(calls, static_cast<int>(x.size() - done));
      for (int i = 0; i < n; ++i) y[done + static_cast<std::size_t>(i)] = tail.tick(x[done + static_cast<std::size_t>(i)]);
      tail.work(n);
      done += static_cast<std::size_t>(n);
    }
    CHECK(tail.forced() == 0, "calls of %d: %ld jobs were still open when due", calls, tail.forced());
    CHECK(worstError(y, ref) < 3e-6, "calls of %d: exact", calls);
  }
  // Negative control: with work() never called every job is finished by force, and the output is
  // still exact. The counter is what notices.
  Arena mem(gMem, sizeof gMem);
  CHECK(tail.prepare(h.data(), length, block, mem), "prepare");
  const TailRun idle = runTail(tail, x, 64, 4, false);
  CHECK(idle.forced > 30 && worstError(idle.y, ref) < 3e-6, "no work(): %ld forced, exact", idle.forced);
}

TEST(cabinetLongPlanMatchesDenseConvolution) {
  static Cabinet cab;
  double worst = 0.0;
  for (int length : {257, 300, 700, 1024, 1025, 2049, 3000}) {
    const std::vector<float> h = dense(length, 21u + static_cast<std::uint32_t>(length));
    const std::vector<float> x = sig::noise(5000, 0.7f, 22u + static_cast<std::uint32_t>(length));
    const std::vector<double> ref = convolveSparse(x, allTaps(h));
    for (Cabinet::Plan plan : {Cabinet::Plan{16, 32}, Cabinet::Plan{16, 128}, Cabinet::Plan{64, 64}, Cabinet::Plan{8, 512}}) {
      Arena mem(gMem, sizeof gMem);
      cab.configure(h.data(), length, plan, false);
      cab.prepare(Setup{kRate, 128}, mem);
      const double e = worstError(run(cab, x, 128, 3u + static_cast<std::uint32_t>(length)), ref);
      CHECK(cab.ready() && e < 3e-6, "plan %d/%d, length %d: error %.3g of peak", plan.partition, plan.tail, length, e);
      worst = std::fmax(worst, e);
    }
  }
  std::printf("     cabinet long plans vs direct convolution in double: worst error %.3g of peak\n", worst);
}

TEST(cabinetPlanTable) {
  using E = Cabinet::Engine;
  CHECK(Cabinet::planFor(4096, 64).partition == 64 && Cabinet::planFor(4096, 64).tail == 0, "short: the caller's partition");
  CHECK(Cabinet::planFor(16384, 64).partition == 64, "at the threshold: still the caller's");
  CHECK(Cabinet::planFor(16385, 64).partition == 256 && Cabinet::planFor(32768, 64).tail == 0, "past it: partition 256");
  CHECK(Cabinet::planFor(32769, 64).tail == 4096 && Cabinet::planFor(240000, 64).tail == 4096, "tail tier of 4096 from 32769");
  CHECK(Cabinet::planFor(240001, 64).tail == 8192, "tail tier of 8192 past 240000");
  CHECK(Cabinet::planFor(240001, 256, E::Partitioned).partition == 256 && Cabinet::planFor(240001, 256, E::Partitioned).tail == 0,
        "Partitioned keeps the old plan at any length");
  CHECK(Cabinet::planFor(100, 64, E::Long).partition == 256, "Long forces the long plan");
  static Cabinet cab;
  const std::vector<float> h = dense(100, 1);
  cab.configure(h.data(), 100, Cabinet::Plan{16, 24}, false);
  CHECK(cab.plan().partition == 4 && cab.plan().tail == 0, "a tail that is not a power of two is refused like a bad partition");
}

TEST(cabinetLongPlansMatchSparseConvolution) {
  static Cabinet cab;
  struct Case {
    int length;
    Cabinet::Plan expect;
  };
  for (const Case c : {Case{20000, {256, 0}}, Case{49000, {256, 4096}}, Case{130000, {256, 4096}}, Case{241000, {256, 8192}}}) {
    std::vector<Tap> taps;
    const std::vector<float> h = sparse(c.length, c.expect.tail, 90u + static_cast<std::uint32_t>(c.length), &taps);
    const std::vector<float> x = sig::noise(70000, 0.6f, 91u + static_cast<std::uint32_t>(c.length));
    const std::vector<double> ref = convolveSparse(x, taps);
    Arena mem(gBig, sizeof gBig);
    cab.configure(h.data(), c.length, Cabinet::kDefaultPartition, false);
    cab.prepare(Setup{kRate, 64}, mem);
    const Cabinet::Plan got = cab.plan();
    CHECK(cab.ready() && got.partition == c.expect.partition && got.tail == c.expect.tail, "length %d: plan %d/%d", c.length, got.partition, got.tail);
    const double e = worstError(run(cab, x, 96, 7u + static_cast<std::uint32_t>(c.length)), ref);
    CHECK(e < 4e-6, "length %d: error %.3g of peak", c.length, e);
    CHECK(cab.far().forced() == 0, "length %d: %ld tail jobs forced", c.length, cab.far().forced());
    std::printf("     %d taps, plan %d/%d, %zu KB of arena: error %.3g of peak\n", c.length, got.partition, got.tail, mem.used() >> 10, e);
  }
}

TEST(cabinetLongPlanHasNoLatency) {
  static Cabinet cab;
  const int length = 60000;
  std::vector<float> h = dense(length, 4);
  h[0] = 0.7f;
  Arena mem(gBig, sizeof gBig);
  cab.configure(h.data(), length, Cabinet::kDefaultPartition, false);
  cab.prepare(Setup{kRate, 64}, mem);
  std::vector<float> x(length + 600, 0.0f);
  x[0] = 1.0f;
  const std::vector<float> y = run(cab, x, 64, 8);
  double worst = 0.0;
  for (int i = 0; i < length + 600; ++i) worst = std::fmax(worst, std::fabs(y[static_cast<std::size_t>(i)] - (i < length ? h[static_cast<std::size_t>(i)] : 0.0f)));
  CHECK(cab.latency() == 0 && cab.far().ready() && worst < 1e-6, "an impulse returns the whole response from sample 0 (error %.3g)", worst);
}

TEST(cabinetLongPlanWithCutFiltersMatchesThePlainPlanWithThem) {
  // The cut filters sit after the convolution, whichever plan makes it: the same response through
  // the long plan and through the plain one must come out the same.
  const int length = 70000;
  const std::vector<float> h = dense(length, 9);
  const std::vector<float> x = sig::noise(8000, 0.5f, 11);
  static Cabinet plain, longPlan;
  Arena memA(gBig, sizeof gBig);
  plain.configure(h.data(), length, Cabinet::kDefaultPartition, true, Cabinet::Engine::Partitioned);
  plain.prepare(Setup{kRate, 64}, memA);
  static unsigned char other[24 << 20];
  Arena memB(other, sizeof other);
  longPlan.configure(h.data(), length, Cabinet::kDefaultPartition, true, Cabinet::Engine::Long);
  longPlan.prepare(Setup{kRate, 64}, memB);
  CHECK(longPlan.far().ready() && !plain.far().ready(), "one plan has a tail convolver, the other has none");
  const std::vector<float> a = run(plain, x, 64, 1), b = run(longPlan, x, 64, 1);
  double worst = 0.0;
  for (std::size_t i = 0; i < a.size(); ++i) worst = std::fmax(worst, std::fabs(a[i] - b[i]));
  CHECK(worst < 1e-3, "worst difference %.3g", worst);
  // Negative control: without the filters the same pair is not the filtered output.
  static Cabinet bare;
  Arena memC(other, sizeof other);
  bare.configure(h.data(), length, Cabinet::kDefaultPartition, false, Cabinet::Engine::Long);
  bare.prepare(Setup{kRate, 64}, memC);
  const std::vector<float> c = run(bare, x, 64, 1);
  double differs = 0.0;
  for (std::size_t i = 0; i < c.size(); ++i) differs = std::fmax(differs, std::fabs(c[i] - b[i]));
  CHECK(differs > 10 * worst + 1e-4, "the filters change the output (%.3g against %.3g)", differs, worst);
}

TEST(cabinetLongPlanTakesNothingInProcess) {
  static Cabinet cab;
  const int length = 100000;
  const std::vector<float> h = dense(length, 6);
  Arena mem(gBig, sizeof gBig);
  cab.configure(h.data(), length, Cabinet::kDefaultPartition, false);
  cab.prepare(Setup{kRate, 64}, mem);
  std::vector<float> x = sig::noise(64, 0.5f, 3), out(64);
  rt::arm();
  for (int i = 0; i < 4 * length / 64; ++i) cab.process(x.data(), out.data(), 64);  // many tail blocks
  const rt::Counts c = rt::disarm();
  CHECK(cab.far().ready() && c.total() == 0, "%ld new, %ld delete, %ld malloc, %ld free, %ld lock", c.news, c.deletes, c.mallocs, c.frees, c.locks);
  rt::arm();
  std::vector<float> leak(64);
  cab.process(x.data(), leak.data(), 64);
  CHECK(rt::disarm().news >= 1, "the watch sees a vector built inside it");
}

TEST(cabinetLongPlanStaysSilentWithoutMemory) {
  static Cabinet cab;
  const int length = 70000;
  const std::vector<float> h = dense(length, 9);
  cab.configure(h.data(), length, Cabinet::kDefaultPartition, false);
  Arena measure;
  cab.prepare(Setup{kRate, 64}, measure);
  CHECK(!cab.ready() && measure.required() > static_cast<std::size_t>(length) * 2 * sizeof(float), "needs %zu bytes", measure.required());
  Arena small(gBig, measure.required() / 2);
  cab.prepare(Setup{kRate, 64}, small);
  std::vector<float> x = sig::noise(256, 0.5f, 7);
  cab.process(x.data(), x.data(), 256);
  CHECK(!cab.ready() && sig::peak(x) == 0.0f, "out of memory: silence");
  Arena exact(gBig, measure.required());
  cab.prepare(Setup{kRate, 64}, exact);
  CHECK(cab.ready(), "the measured size is enough");
}

TEST(cabinetLongPlanResetForgetsTheTail) {
  static Cabinet cab;
  const int length = 50000;
  const std::vector<float> h = dense(length, 10);
  Arena mem(gBig, sizeof gBig);
  cab.configure(h.data(), length, Cabinet::kDefaultPartition, false);
  cab.prepare(Setup{kRate, 64}, mem);
  std::vector<float> x = sig::noise(30000, 0.8f, 11), y(30000);
  cab.process(x.data(), y.data(), 30000);
  cab.reset();
  std::vector<float> silence(30000, 0.0f);
  cab.process(silence.data(), y.data(), 30000);
  CHECK(sig::peak(y) == 0.0f, "after reset(), silence in is silence out (peak %.3g)", sig::peak(y));
}
