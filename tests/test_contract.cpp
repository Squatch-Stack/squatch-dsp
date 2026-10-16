// SPDX-License-Identifier: MIT
// The block contract at the top of include/squatch/dsp/block.hpp, checked for every block: one
// parameter descriptor per ParamId in id order, in place the same as out of place, reset() back to
// a fresh state, and the latency a block reports is the latency it adds. tests/test_rt.cpp watches
// process() for allocations and locks; this is the rest of the contract, which nothing else covers.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <squatch/dsp/blocks/cabinet.hpp>
#include <squatch/dsp/blocks/eq.hpp>
#include <squatch/dsp/blocks/gate.hpp>
#include <squatch/dsp/blocks/limiter.hpp>
#include <squatch/dsp/blocks/tuner.hpp>

#include "check.hpp"
#include "signals.hpp"

using namespace squatch::dsp;

namespace {

constexpr int kMaxBlock = 128;
alignas(64) unsigned char gMem[1 << 20];

struct Named {
  const char* name;
  Block& block;
};

/// A cabinet-like response, so the cabinet has real state to carry from one call to the next.
std::vector<float> response(int length) {
  std::vector<float> h = sig::noise(length, 0.5f, 5);
  for (int i = 0; i < length; ++i) h[i] *= static_cast<float>(std::exp(-6.0 * i / length));
  h[0] = 0.8f;
  return h;
}

void descriptors(const char* name, Span<const ParamDesc> ps, int count) {
  CHECK(static_cast<int>(ps.size()) == count, "%s: %zu descriptors, want %d", name, ps.size(), count);
  for (std::size_t i = 0; i < ps.size(); ++i) {
    const ParamDesc& d = ps[i];
    CHECK(d.id == i, "%s: descriptor %zu has id %u, want %zu (one per ParamId, in id order)", name, i,
          unsigned(d.id), i);
    CHECK(d.min <= d.def && d.def <= d.max, "%s: %s default %g outside [%g, %g]", name, d.name, d.def, d.min, d.max);
    CHECK(d.clamp(NAN) == d.def, "%s: %s takes NaN as the default", name, d.name);
    CHECK(d.clamp(d.min - 1e6f) == d.min && d.clamp(d.max + 1e6f) == d.max, "%s: %s clamps to its range", name,
          d.name);
  }
}

/// The block over x in random call sizes, in place or not. reset() first, so two calls with the same
/// seed start from the same state.
std::vector<float> render(Block& b, const std::vector<float>& x, bool inPlace, std::uint32_t seed) {
  b.reset();
  sig::Rng rng{seed};
  std::vector<float> y(x.size());
  for (std::size_t done = 0; done < x.size();) {
    const int n = std::min<int>(rng.range(1, kMaxBlock), static_cast<int>(x.size() - done));
    if (inPlace) {
      copy(&x[done], &y[done], n);
      b.process(&y[done], &y[done], n);
    } else {
      b.process(&x[done], &y[done], n);
    }
    done += static_cast<std::size_t>(n);
  }
  return y;
}

double worstDifference(const std::vector<float>& a, const std::vector<float>& b) {
  double worst = 0.0;
  for (std::size_t i = 0; i < a.size(); ++i) worst = std::fmax(worst, std::fabs(double(a[i]) - double(b[i])));
  return worst;
}

/// Every block, prepared as a host would, with parameters that make it do something.
struct Rig {
  Gate gate;
  Eq eq;
  Limiter limiter;
  Cabinet cab;
  TunerBlock tuner;
  std::vector<float> ir = response(2000);

  Rig();
};

std::vector<Named> every(Rig& r) {
  return {{"gate", r.gate}, {"eq", r.eq}, {"limiter", r.limiter}, {"cabinet", r.cab}, {"tuner", r.tuner}};
}

Rig::Rig() {
  Arena mem(gMem, sizeof gMem);
  limiter.set(Limiter::Ceiling, -3.0f);
  gate.set(Gate::Threshold, -40.0f);
  eq.set(Eq::MidGain, 6.0f);
  cab.configure(ir.data(), 2000);
  tuner.set(TunerBlock::Mute, 0.0f);
  for (const Named& b : every(*this)) b.block.prepare(Setup{48000.0, kMaxBlock}, mem);
}

/// The limiter at one rate. A signal under the ceiling passes through bit for bit, so the first
/// sample that comes out is the delay it reports.
void limiterLatencyAt(double rate) {
  Limiter lim;
  Arena mem(gMem, sizeof gMem);
  lim.set(Limiter::Ceiling, 0.0f);
  lim.prepare(Setup{rate, kMaxBlock}, mem);
  const int n = static_cast<int>(rate) + 2 * lim.latency() + 16;
  std::vector<float> x(static_cast<std::size_t>(n), 0.2f), y(static_cast<std::size_t>(n), 0.0f);
  for (std::size_t i = 0; i < x.size(); i += kMaxBlock)  // no call longer than maxBlock
    lim.process(&x[i], &y[i], std::min(kMaxBlock, static_cast<int>(x.size() - i)));
  int first = 0;
  while (first < n && y[first] == 0.0f) ++first;
  bool steady = true;
  for (int i = first; i < n; ++i) steady = steady && y[i] == 0.2f;
  CHECK(first == lim.latency() && steady, "%.0f Hz: reports %d samples, the signal starts at %d", rate,
        lim.latency(), first);
}

}  // namespace

TEST(blockDescriptorsAreOnePerIdInOrder) {
  descriptors("gate", Gate::params(), Gate::kCount);
  descriptors("eq", Eq::params(), Eq::kCount);
  descriptors("limiter", Limiter::params(), Limiter::kCount);
  descriptors("cabinet", Cabinet::params(), Cabinet::kCount);
  descriptors("tuner", TunerBlock::params(), TunerBlock::kCount);
}

TEST(blockInPlaceEqualsOutOfPlace) {
  static Rig rig;
  const std::vector<float> x = sig::noise(4000, 0.8f, 3);
  for (const Named& b : every(rig)) {
    const double worst = worstDifference(render(b.block, x, true, 5), render(b.block, x, false, 5));
    CHECK(worst == 0.0, "%s: in place differs from out of place by %.3g", b.name, worst);
  }
}

TEST(blockResetReturnsAFreshBlock) {
  static Rig rig;
  const std::vector<float> x = sig::noise(4000, 0.8f, 3);
  for (const Named& b : every(rig)) {
    render(b.block, x, false, 5);  // leave state behind
    const double worst = worstDifference(render(b.block, x, false, 5), render(b.block, x, false, 5));
    CHECK(worst == 0.0, "%s: reset() left state behind (%.3g)", b.name, worst);
  }
}

TEST(blockReportsTheLatencyItAdds) {
  // Only the limiter delays anything; the rest must report none.
  for (double rate : {32000.0, 44100.0, 48000.0, 96000.0, 192000.0}) limiterLatencyAt(rate);
  Gate gate;
  Eq eq;
  Cabinet cab;
  TunerBlock tuner;
  cab.configure(nullptr, 0);
  CHECK(gate.latency() == 0 && eq.latency() == 0 && cab.latency() == 0 && tuner.latency() == 0,
        "the blocks that add no latency say so");
}

TEST(blocksWithoutMemoryAreSilent) {
  Limiter lim;
  Cabinet cab;
  std::vector<float> x(256, 0.5f), y(256, 7.0f);
  lim.process(x.data(), y.data(), 256);
  CHECK(!lim.ready() && peakAbs(y.data(), 256) == 0.0f, "a limiter prepared against no memory is silent");
  std::fill(y.begin(), y.end(), 7.0f);
  cab.process(x.data(), y.data(), 256);
  CHECK(!cab.ready() && peakAbs(y.data(), 256) == 0.0f, "a cabinet prepared against no memory is silent");
}