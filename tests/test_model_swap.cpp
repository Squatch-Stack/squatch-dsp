// SPDX-License-Identifier: MIT
// ModelSwap: a live handover between two cabinets (the same mechanism carries two NAM models in
// a host). The swap is exact once done (the standby was warmed on the live
// input), has no step beyond the signals' own, keeps the level with correlated signals, and
// takes no allocation or lock on the audio thread. Negative controls: a hard switch exceeds the
// bound, and plain equal power on identical signals is +3 dB. The cabinets run without their cut
// filters (configure(..., false)): the filters are a separate stage with their own state, and not
// what the handover is judged on.
#include <squatch/dsp/blocks/cabinet.hpp>
#include <squatch/dsp/blocks/model_swap.hpp>

#include "check.hpp"
#include "glitch.hpp"
#include "rt_watch.hpp"
#include "signals.hpp"

using namespace squatch::dsp;

namespace {

constexpr double kRate = 48000.0;
alignas(64) unsigned char gMem[1 << 20], gMemB[1 << 20], gMemC[1 << 20];

std::vector<float> response(int length, std::uint32_t seed, float tilt) {
  std::vector<float> h = sig::noise(length, 0.4f, seed);
  for (int i = 0; i < length; ++i) h[static_cast<std::size_t>(i)] *= std::exp(-tilt * float(i) / float(length));
  h[0] = 0.6f;
  return h;
}

// Guitar-like test material: a few partials with a slow amplitude wobble, plus a little noise.
std::vector<float> guitar(int n) {
  std::vector<float> x = sig::sine(196.0, n, kRate, 0.30), b = sig::sine(392.0, n, kRate, 0.15), c = sig::noise(n, 0.02f, 5);
  for (int i = 0; i < n; ++i) x[static_cast<std::size_t>(i)] += b[static_cast<std::size_t>(i)] + c[static_cast<std::size_t>(i)];
  return x;
}

std::vector<double> convolve(const std::vector<float>& x, const std::vector<float>& h) {
  std::vector<double> y(x.size(), 0.0);
  for (std::size_t t = 0; t < x.size(); ++t)
    for (std::size_t k = 0; k < h.size() && k <= t; ++k) y[t] += double(h[k]) * x[t - k];
  return y;
}

float maxStep(const std::vector<float>& y, int from, int to) {
  float m = 0.0f;
  for (int i = from + 1; i < to; ++i) m = std::fmax(m, std::fabs(y[static_cast<std::size_t>(i)] - y[static_cast<std::size_t>(i - 1)]));
  return m;
}

double rms(const std::vector<float>& y, int from, int to) {
  double s = 0.0;
  for (int i = from; i < to; ++i) s += double(y[static_cast<std::size_t>(i)]) * y[static_cast<std::size_t>(i)];
  return std::sqrt(s / (to - from));
}

struct Pair {
  Cabinet a, b;
  std::vector<float> ha, hb;
  ModelSwap swap;
  int loads = 0;
  Pair(int la, int lb, float tiltB, int warm, std::uint32_t seedB = 22)
      : ha(response(la, 11, 5.0f)), hb(response(lb, seedB, tiltB)), swap(a, b, warm) {
    a.configure(ha.data(), la, Cabinet::kDefaultPartition, false);
    b.configure(hb.data(), lb, Cabinet::kDefaultPartition, false);
  }
  void prepare(int block) {
    Arena mem(gMem, sizeof gMem);
    swap.prepare(Setup{kRate, block}, mem);
  }
  // Control thread: load the standby with response `h` and publish.
  bool load(const std::vector<float>& h, int block) {
    Block* s = swap.standby();
    if (!s) return false;
    Arena mem(loads++ % 2 ? gMemC : gMemB, sizeof gMemB);  // the standby's old arena is free again every second time
    static_cast<Cabinet*>(s)->configure(h.data(), static_cast<int>(h.size()), Cabinet::kDefaultPartition, false);
    s->prepare(Setup{kRate, block}, mem);
    return swap.publish();
  }
};

// Blocks of `block`, publishing `h` at sample `at` (the control thread's part runs between blocks).
std::vector<float> render(Pair& p, const std::vector<float>& x, int block, int at, const std::vector<float>* h) {
  std::vector<float> y(x.size());
  bool done = h == nullptr;
  for (int i = 0; i < int(x.size()); i += block) {
    const int n = std::min<int>(block, int(x.size()) - i);
    if (!done && i >= at) done = p.load(*h, block);
    p.swap.process(&x[static_cast<std::size_t>(i)], &y[static_cast<std::size_t>(i)], n);
  }
  return y;
}

constexpr int kStart = 6000;

}  // namespace

TEST(modelSwapEndsExactlyOnTheNewResponse) {
  for (int block : {1, 37, 64, 128}) {
    Pair p(900, 1400, 4.0f, 1500);
    p.prepare(block);
    const std::vector<float> x = guitar(16000);
    const std::vector<float> y = render(p, x, block, kStart, &p.hb);
    const std::vector<double> a = convolve(x, p.ha), b = convolve(x, p.hb);
    double before = 0.0, after = 0.0;
    for (int i = 0; i < kStart; ++i) before = std::fmax(before, std::fabs(y[static_cast<std::size_t>(i)] - a[static_cast<std::size_t>(i)]));
    for (int i = kStart + 1500 + 1100 + block; i < int(x.size()); ++i) after = std::fmax(after, std::fabs(y[static_cast<std::size_t>(i)] - b[static_cast<std::size_t>(i)]));
    CHECK(before < 2e-6 && after < 2e-6, "block %d: error %.3g before, %.3g after", block, before, after);
    CHECK(p.swap.idle() && p.swap.swaps() == 1 && p.swap.playing() == 1, "one swap done, the second block plays");
  }
}

TEST(modelSwapHasNoStepBeyondTheSignalsOwn) {
  const std::vector<float> x = guitar(16000);
  Pair p(900, 1400, 4.0f, 1500);
  p.prepare(64);
  const std::vector<double> ad = convolve(x, p.ha), bd = convolve(x, p.hb);
  std::vector<float> a(x.size()), b(x.size());
  for (std::size_t i = 0; i < x.size(); ++i) a[i] = float(ad[i]), b[i] = float(bd[i]);
  const float natural = std::fmax(maxStep(a, 2000, 16000), maxStep(b, 2000, 16000));
  const std::vector<float> y = render(p, x, 64, kStart, &p.hb);
  const float step = maxStep(y, 2000, 16000);
  CHECK(step <= natural * 1.5f, "swap step %.4f against the signals' own %.4f", step, natural);
  // Negative control: a hard switch at the same place (A's output, then B's) steps by far more.
  float cutStep = 0.0f;
  for (int at = 3000; at < 3400; at += 7) cutStep = std::fmax(cutStep, std::fabs(float(bd[static_cast<std::size_t>(at)] - ad[static_cast<std::size_t>(at - 1)])));
  CHECK(cutStep > natural * 1.5f, "a hard switch steps %.4f, which the bound %.4f must catch", cutStep, natural * 1.5f);
}

TEST(modelSwapKeepsTheLevelWithCorrelatedSignals) {
  // The same response in both: the swap must be inaudible, so the level through the fade stays put.
  const std::vector<float> x = guitar(16000);
  Pair p(900, 900, 5.0f, 1500, 11);
  p.hb = p.ha;
  p.prepare(64);
  const std::vector<float> y = render(p, x, 64, kStart, &p.hb);
  const std::vector<double> ref = convolve(x, p.ha);
  double worst = 0.0;
  for (std::size_t i = 0; i < y.size(); ++i) worst = std::fmax(worst, std::fabs(y[i] - ref[i]));
  CHECK(worst < 2e-6, "identical models: the output is the response throughout (error %.3g)", worst);
  // Negative control: equal power with no correlation correction adds 3 dB at the midpoint.
  Crossfade naive;
  Arena mem(gMem, sizeof gMem);
  naive.prepare(960, mem);
  std::vector<float> a(960), out(960);
  for (std::size_t i = 0; i < 960; ++i) a[i] = float(ref[8000 + i]);
  naive.start(960, 0.0);
  naive.mix(a.data(), a.data(), out.data(), 960);
  float mid = 0.0f;
  for (int i = 400; i < 560; ++i) mid = std::fmax(mid, std::fabs(out[static_cast<std::size_t>(i)]) / std::fmax(1e-6f, std::fabs(a[static_cast<std::size_t>(i)])));
  CHECK(mid > 1.3f, "equal power on identical signals swells by %.2fx, which the exact check above catches", mid);
}

TEST(modelSwapLevelThroughTheFadeWithRelatedModels) {
  // Two different but strongly related responses: the RMS through the fade stays within 1 dB of both.
  const std::vector<float> x = guitar(16000);
  Pair p(600, 600, 5.0f, 1500, 11);
  p.hb = p.ha;
  for (float& v : p.hb) v *= 0.8f;
  p.prepare(64);
  const std::vector<float> y = render(p, x, 64, kStart, &p.hb);
  const double inA = rms(y, 3000, 6000), inB = rms(y, 11000, 16000);
  double lo = 1e9, hi = 0.0;
  for (int i = kStart + 1500; i + 480 < kStart + 1500 + 960 + 128; i += 160) lo = std::fmin(lo, rms(y, i, i + 480)), hi = std::fmax(hi, rms(y, i, i + 480));
  const double top = std::fmax(inA, inB) * 1.12, bottom = std::fmin(inA, inB) * 0.89;
  CHECK(hi <= top && lo >= bottom, "fade level %.4f..%.4f within %.4f..%.4f (A %.4f, B %.4f)", lo, hi, bottom, top, inA, inB);
}

TEST(modelSwapHandoffRules) {
  Pair p(300, 400, 3.0f, 100);
  CHECK(p.swap.standby() == nullptr, "not prepared: no standby");
  p.prepare(64);
  Block* s = p.swap.standby();
  CHECK(s == &p.b && p.swap.standby() == nullptr, "one standby at a time");
  p.swap.cancel();
  CHECK(p.swap.idle() && !p.swap.publish(), "cancelled: back to idle, nothing to publish");
  CHECK(p.load(p.hb, 64) && !p.swap.publish() && p.swap.standby() == nullptr, "published: no second publish, no standby while swapping");
  const std::vector<float> x = guitar(4000);
  render(p, x, 64, 0, nullptr);
  CHECK(p.swap.idle() && p.swap.playing() == 1, "done after the fade");
  CHECK(p.swap.standby() == &p.a, "the old block is now the standby");
  p.swap.cancel();
  CHECK(p.load(p.ha, 64), "and swaps back");
  render(p, x, 64, 0, nullptr);
  CHECK(p.swap.playing() == 0 && p.swap.swaps() == 2, "A plays again after two swaps");
}

TEST(modelSwapTakesNothingOnTheAudioThread) {
  Pair p(900, 1400, 4.0f, 1500);
  p.prepare(128);
  const std::vector<float> x = guitar(20000);
  std::vector<float> y(x.size());
  Arena mem(gMemB, sizeof gMemB);
  Block* s = p.swap.standby();
  p.b.configure(p.hb.data(), 1400, Cabinet::kDefaultPartition, false);
  s->prepare(Setup{kRate, 128}, mem);
  CHECK(p.swap.publish(), "published");
  rt::arm();
  for (int i = 0; i + 128 <= int(x.size()); i += 128) {
    if (i % 1024 == 0) p.swap.set(0, 0.0f);
    p.swap.process(&x[static_cast<std::size_t>(i)], &y[static_cast<std::size_t>(i)], 128);
  }
  const rt::Counts c = rt::disarm();
  CHECK(p.swap.swaps() == 1 && c.total() == 0, "%ld new, %ld delete, %ld malloc, %ld free, %ld lock", c.news, c.deletes, c.mallocs, c.frees, c.locks);
  rt::arm();
  std::vector<float> leak(128);
  p.swap.process(x.data(), leak.data(), 128);
  CHECK(rt::disarm().news >= 1, "the watch sees a vector built inside it");
}

TEST(modelSwapStaysSilentWithoutMemoryAndResetCompletes) {
  Pair p(300, 400, 3.0f, 100);
  alignas(64) unsigned char tiny[64];
  Arena mem(tiny, sizeof tiny);
  p.swap.prepare(Setup{kRate, 64}, mem);
  float in[16], out[16];
  for (float& v : in) v = 1.0f;
  p.swap.process(in, out, 16);
  for (float v : out) CHECK(v == 0.0f, "silent");
  CHECK(p.swap.standby() == nullptr, "no standby without memory");
  Pair q(300, 400, 3.0f, 5000);
  q.prepare(64);
  CHECK(q.load(q.hb, 64), "published");
  const std::vector<float> x = guitar(256);
  std::vector<float> y(256);
  q.swap.process(x.data(), y.data(), 64);
  q.swap.reset();
  CHECK(q.swap.idle() && q.swap.playing() == 1, "reset during a swap completes it");
}

TEST(modelSwapGivesTheStandbyTheCurrentParameters) {
  Pair p(900, 1400, 4.0f, 1500);
  p.prepare(64);
  p.swap.set(Cabinet::Level, -6.0f);
  const std::vector<float> x = guitar(16000);
  const std::vector<float> y = render(p, x, 64, kStart, &p.hb);
  const std::vector<double> b = convolve(x, p.hb);
  double worst = 0.0;
  for (int i = 14000; i < 16000; ++i) worst = std::fmax(worst, std::fabs(y[static_cast<std::size_t>(i)] - 0.501187 * b[static_cast<std::size_t>(i)]));
  CHECK(worst < 1e-4, "the new block runs at -6 dB too (error %.3g)", worst);
}

TEST(modelSwapPassesTheGlitchOracle) {
  std::vector<float> x = guitar(24000);
  for (int block : {16, 64, 128}) {
    Pair p(900, 1400, 4.0f, 1500);
    p.prepare(block);
    const std::vector<float> y = render(p, x, block, kStart, &p.hb);
    const std::vector<double> ad = convolve(x, p.ha), bd = convolve(x, p.hb);
    std::vector<float> a(x.size()), b(x.size());
    for (std::size_t i = 0; i < x.size(); ++i) a[i] = float(ad[i]), b[i] = float(bd[i]);
    // The fade starts at the first block boundary after the warm-up and lasts kFadeMs.
    const std::size_t start = kStart + 1500, end = start + 960 + static_cast<std::size_t>(block);
    const glitch::Report r = glitch::analyse(y, a, b, {{start, end}});
    CHECK(r.ok && r.error.empty(), "block %d: %s%s", block, r.error.c_str(), r.findings.empty() ? "" : r.findings[0].c_str());
  }
  // Negative controls: the output cut from A to B at the same place (pure tone: a step towers
  // over its curvature), and a swap that lands on the wrong response.
  Pair q(900, 1400, 4.0f, 1500);
  const std::vector<float> tone = sig::sine(196.0, 24000, kRate, 0.5);
  const std::vector<double> ad = convolve(tone, q.ha), bd = convolve(tone, q.hb);
  std::vector<float> a(tone.size()), b(tone.size()), cut(tone.size()), wrong(tone.size());
  for (std::size_t i = 0; i < tone.size(); ++i) a[i] = float(ad[i]), b[i] = float(bd[i]), cut[i] = i < kStart + 1500 + 480 ? a[i] : b[i], wrong[i] = i < kStart + 1500 + 480 ? a[i] : 0.5f * (a[i] + b[i]);
  const std::size_t start = kStart + 1500, end = start + 960;
  CHECK(glitch::analyse(cut, a, b, {{start, end}}).failed("continuity"), "a cut is a continuity finding");
  CHECK(glitch::analyse(wrong, a, b, {{start, end}}).failed("convergence"), "landing on the wrong response is a convergence finding");
}
