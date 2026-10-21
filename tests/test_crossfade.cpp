// SPDX-License-Identifier: MIT
// Crossfade. The cases are ported from pedal-science/namchain by Rik Hemsley (MIT), pinned to e208bb8:
//   https://github.com/pedal-science/namchain/blob/e208bb869069b15a7331b452e03b7dc88726ffee/tests/test_crossfade.cpp
// Copyright (c) 2026 Rik Hemsley; see THIRD_PARTY_NOTICES.md. Ported: equal-power weights and endpoints, a fade that closes mid-block,
// no energy added for the same audio on both sides, no dip for unrelated signals, constant
// amplitude at rho 1, rho measured while the fade runs, rho clamped. His cases that run a whole
// namchain plan through a glitch detector have no counterpart here (no plans, no detector);
// tests/test_bypass.cpp judges the same thing with a step bound and a hard-switch control.
#include <squatch/dsp/crossfade.hpp>

#include "check.hpp"
#include "rt_watch.hpp"
#include "signals.hpp"

using namespace squatch::dsp;

namespace {

constexpr int kFade = 240;  // 5 ms at 48 kHz
alignas(64) unsigned char gMem[1 << 20];

double energy(const float* x, int n) {
  double e = 0.0;
  for (int i = 0; i < n; ++i) e += double(x[i]) * x[i];
  return e;
}

// How far the fade's energy sits above the louder of its two inputs, in dB.
double excessDb(const std::vector<float>& y, const std::vector<float>& a, const std::vector<float>& b, int n) {
  const double ref = std::fmax(energy(a.data(), n), energy(b.data(), n));
  return 10.0 * std::log10(energy(y.data(), n) / ref);
}

}  // namespace

TEST(crossfadeWeightsAreEqualPowerAndReachBothEndpoints) {
  Arena mem(gMem, sizeof gMem);
  Crossfade fade;
  fade.prepare(1024, mem);
  CHECK(fade.ready() && fade.length() == 0 && !fade.active(), "idle before start");
  std::vector<float> ones(kFade, 1.0f), zeros(kFade, 0.0f), from(kFade), to(kFade);
  fade.start(kFade, 0.0);  // rho 0: the shape the equal-power identity belongs to
  CHECK(fade.active() && fade.length() == kFade, "length %d", fade.length());
  fade.mix(ones.data(), zeros.data(), from.data(), kFade);
  fade.start(kFade, 0.0);
  fade.mix(zeros.data(), ones.data(), to.data(), kFade);
  CHECK(std::fabs(from.front() - 1.0f) < 1e-6f && std::fabs(from.back()) < 1e-6f, "old ends %g %g", from.front(), from.back());
  CHECK(std::fabs(to.front()) < 1e-6f && std::fabs(to.back() - 1.0f) < 1e-6f, "new ends %g %g", to.front(), to.back());
  for (int i = 0; i < kFade; ++i) {
    CHECK(std::fabs(from[i] * from[i] + to[i] * to[i] - 1.0f) < 1e-4f, "power at %d", i);
    CHECK(to[i] >= 0.0f && to[i] <= 1.0f, "weight %g at %d", to[i], i);
  }
  CHECK(!fade.active(), "closed");
}

TEST(crossfadeClosingMidBlockLeavesTheNewSignal) {
  Arena mem(gMem, sizeof gMem);
  Crossfade fade;
  fade.prepare(1024, mem);
  std::vector<float> a(64, 1.0f), b(64, 0.5f), out(64, -1.0f);
  fade.start(10);
  fade.mix(a.data(), b.data(), out.data(), 64);
  CHECK(!fade.active(), "closed");
  for (int i = 10; i < 64; ++i) CHECK(out[i] == 0.5f, "tail %d is %g", i, out[i]);
}

TEST(crossfadeSameAudioAddsNoEnergyButEqualPowerAloneWould) {
  Arena mem(gMem, sizeof gMem);
  Crossfade fade;
  fade.prepare(kFade, mem);
  const std::vector<float> x = sig::sine(220.0, kFade, 48000.0, 0.4);
  std::vector<float> y(kFade);
  fade.start(kFade);  // measures rho = 1
  fade.mix(x.data(), x.data(), y.data(), kFade);
  const double excess = excessDb(y, x, x, kFade);
  CHECK(excess < 0.1, "excess %.3f dB", excess);
  // Negative control: with rho forced to 0 the same fade adds energy, which is the thump
  // the correlation law exists to remove. If this stops failing the test above proves nothing.
  fade.start(kFade, 0.0);
  fade.mix(x.data(), x.data(), y.data(), kFade);
  const double raw = excessDb(y, x, x, kFade);
  CHECK(raw > 1.5, "equal power alone adds %.3f dB", raw);
}

TEST(crossfadeUnrelatedSignalsDoNotDip) {
  Arena mem(gMem, sizeof gMem);
  Crossfade fade;
  fade.prepare(kFade, mem);
  const std::vector<float> a = sig::sine(220.0, kFade, 48000.0, 0.4);
  const std::vector<float> b = sig::noise(kFade, 0.4f, 5);
  std::vector<float> y(kFade);
  fade.start(kFade);
  fade.mix(a.data(), b.data(), y.data(), kFade);
  const double dip = 10.0 * std::log10(energy(y.data(), kFade) / (0.5 * (energy(a.data(), kFade) + energy(b.data(), kFade))));
  CHECK(dip > -1.0, "level %.3f dB against the inputs' mean power", dip);
  CHECK(fade.correlation() < 0.5, "rho %g", fade.correlation());
}

TEST(crossfadeIsConstantAmplitudeAtRhoOne) {
  Arena mem(gMem, sizeof gMem);
  Crossfade fade;
  fade.prepare(1024, mem);
  std::vector<float> ones(kFade, 1.0f), out(kFade);
  fade.start(kFade, 1.0);
  CHECK(fade.correlation() == 1.0, "rho %g", fade.correlation());
  fade.mix(ones.data(), ones.data(), out.data(), kFade);
  for (int i = 0; i < kFade; ++i) CHECK(std::fabs(out[i] - 1.0f) < 1e-4f, "sample %d is %g", i, out[i]);
}

TEST(crossfadeMeasuresRhoWhileItRuns) {
  Arena mem(gMem, sizeof gMem);
  Crossfade fade;
  fade.prepare(1024, mem);
  const std::vector<float> x = sig::sine(220.0, 64, 48000.0, 0.4), other = sig::noise(64, 0.4f, 9);
  std::vector<float> out(64);
  fade.start(240);
  CHECK(fade.correlation() == 0.0, "the safe law until a block arrives");
  fade.mix(x.data(), x.data(), out.data(), 64);
  CHECK(fade.correlation() > 0.99, "same audio: %g", fade.correlation());
  fade.start(240);
  fade.mix(x.data(), other.data(), out.data(), 64);
  CHECK(fade.correlation() < 0.5, "unrelated: %g", fade.correlation());
}

TEST(crossfadeCorrelationIsClampedSoAFadeNeverAmplifies) {
  Arena mem(gMem, sizeof gMem);
  Crossfade fade;
  fade.prepare(1024, mem);
  std::vector<float> a = sig::sine(220.0, 64, 48000.0, 0.4), b(64), out(64);
  for (int i = 0; i < 64; ++i) b[i] = -a[i];  // rho would be -1
  fade.start(240);
  fade.mix(a.data(), b.data(), out.data(), 64);
  CHECK(fade.correlation() >= 0.0 && fade.correlation() <= 1.0, "rho %g", fade.correlation());
  fade.start(240, NAN);  // a NaN rho is not a license to emit NaN
  fade.mix(a.data(), a.data(), out.data(), 64);
  for (float v : out) CHECK(v == v && std::fabs(v) <= 0.81f, "sample %g", v);
  fade.start(240, 7.0);
  CHECK(fade.correlation() == 1.0, "clamped high: %g", fade.correlation());
}

TEST(crossfadeLengthIsClampedAndUnpreparedHandsOverAtOnce) {
  Arena mem(gMem, sizeof gMem);
  Crossfade fade;
  fade.prepare(100, mem);
  fade.start(100000);
  CHECK(fade.length() == 100, "long start clamped, got %d", fade.length());
  fade.start(0);
  CHECK(fade.length() == 1, "short start clamped, got %d", fade.length());
  std::vector<float> a(8, 1.0f), b(8, 0.25f), out(8);
  fade.finish();
  fade.mix(a.data(), b.data(), out.data(), 8);
  for (float v : out) CHECK(v == 0.25f, "settled signal is the new one");

  Crossfade bare;  // never prepared
  bare.start(50);
  CHECK(!bare.ready() && !bare.active(), "unprepared stays idle");
  bare.mix(a.data(), b.data(), out.data(), 8);
  for (float v : out) CHECK(v == 0.25f, "unprepared hands over at once");

  alignas(64) unsigned char tiny[16];
  Arena small(tiny, sizeof tiny);
  Crossfade starved;
  starved.prepare(4096, small);
  CHECK(!starved.ready() && small.failed(), "an arena that is too small is refused at prepare time");
}

TEST(crossfadeRenderPathNeverAllocatesOrLocks) {
  Arena mem(gMem, sizeof gMem);
  Crossfade fade;
  fade.prepare(4096, mem);
  std::vector<float> a(64, 1.0f), b(64, 0.5f), out(64);
  rt::arm();
  for (int i = 0; i < 1000; ++i) {
    fade.start(i % 3 ? 240 : 7, i % 2 ? Crossfade::kMeasureCorrelation : 0.5);
    for (int blocks = 0; blocks < 5; ++blocks) fade.mix(a.data(), b.data(), out.data(), 64);
  }
  const rt::Counts c = rt::disarm();
  CHECK(c.total() == 0, "%ld new, %ld delete, %ld malloc, %ld free, %ld lock", c.news, c.deletes, c.mallocs, c.frees, c.locks);
  // Negative control: the watch does see an allocation made in the same place.
  rt::arm();
  std::vector<float> leak(64);
  fade.mix(a.data(), b.data(), leak.data(), 64);
  const rt::Counts seen = rt::disarm();
  CHECK(seen.news >= 1, "the watch sees a vector built inside it (%ld)", seen.news);
}

namespace {
// One fade of two partly related signals, fed in blocks of `block`. `firstBlockRho` re-creates the
// original estimator (correlation of the first block only) as the negative control.
std::vector<float> renderFade(int block, bool firstBlockRho, double* rhoEnd) {
  Arena mem(gMem, sizeof gMem);
  Crossfade fade;
  fade.prepare(1024, mem);
  const int n = 480;
  std::vector<float> a = sig::sine(220.0, n, 48000.0, 0.4), b = sig::sine(220.0, n, 48000.0, 0.3);
  const std::vector<float> na = sig::noise(n, 0.25f, 3), nb = sig::noise(n, 0.25f, 4);
  for (int i = 0; i < n; ++i) a[i] += na[i], b[i] += nb[i];
  double rho = Crossfade::kMeasureCorrelation;
  if (firstBlockRho) {
    double ab = 0, aa = 0, bb = 0;
    for (int i = 0; i < block; ++i) ab += double(a[i]) * b[i], aa += double(a[i]) * a[i], bb += double(b[i]) * b[i];
    rho = std::fmin(1.0, std::fmax(0.0, ab / std::sqrt(aa * bb)));
  }
  fade.start(kFade, rho);
  std::vector<float> y(n);
  for (int at = 0; at < n; at += block) {
    const int m = std::min(block, n - at);
    fade.mix(a.data() + at, b.data() + at, y.data() + at, m);
  }
  *rhoEnd = fade.correlation();
  return y;
}

double maxDiff(const std::vector<float>& x, const std::vector<float>& y) {
  double d = 0.0;
  for (std::size_t i = 0; i < x.size(); ++i) d = std::fmax(d, std::fabs(double(x[i]) - y[i]));
  return d;
}
}  // namespace

TEST(crossfadeResultDoesNotDependOnBlockSize) {
  double r0 = 0, r = 0;
  const std::vector<float> ref = renderFade(64, false, &r0);
  CHECK(r0 > 0.05 && r0 < 0.95, "the signals are partly related (rho %g), so the weights matter", r0);
  for (int block : {1, 7, 32, 64, 128}) {
    const std::vector<float> y = renderFade(block, false, &r);
    CHECK(maxDiff(y, ref) < 1e-6, "block %d differs from block 64 by %g", block, maxDiff(y, ref));
    CHECK(std::fabs(r - r0) < 1e-6, "block %d ends at rho %g, block 64 at %g", block, r, r0);
  }
  // Negative control: the first-block estimator gives each block size its own weights, and this
  // comparison catches it. If this stops failing, the comparison above proves nothing.
  const std::vector<float> old1 = renderFade(1, true, &r), old128 = renderFade(128, true, &r);
  CHECK(maxDiff(old1, old128) > 0.01, "first-block estimates differ by %g", maxDiff(old1, old128));
}
