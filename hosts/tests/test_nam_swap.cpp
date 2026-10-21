// SPDX-License-Identifier: MIT
// NamAmp swapping real NAM models while it plays (make test-nam NAM_CORE=DIR; needs NAM Core's
// example_models). The swap has no step beyond the models' own, keeps the level, ends on the new
// model, and takes no allocation or lock on the audio thread. Not part of `make test`: NAM Core
// is optional and never part of the library.
//
// The method is adapted from pedal-science/namchain by Rik Hemsley (MIT), pinned to e208bb8,
// licence in THIRD_PARTY_NOTICES.md (see model_swap.hpp):
//   https://github.com/pedal-science/namchain/blob/e208bb869069b15a7331b452e03b7dc88726ffee/docs/transitions.md
// Copyright (c) 2026 Rik Hemsley
#include <atomic>
#include <cmath>
#include <string>
#include <thread>
#include <vector>

#include <squatch/dsp/blocks/bypass.hpp>

#include "../../tests/check.hpp"
#include "../../tests/glitch.hpp"
#include "../../tests/rt_watch.hpp"
#include "../../tests/signals.hpp"
#include "../jack/nam_amp.hpp"

#ifdef SQUATCH_WITH_NAM
using namespace squatch::dsp;
using squatch::host::NamAmp;

namespace {

constexpr double kRate = 48000.0;
constexpr int kBlock = 64;
alignas(64) unsigned char gMem[1 << 24];

std::string model(const char* name) { return std::string(NAM_MODELS_DIR) + "/" + name; }

// A low note with one overtone: slow enough that the models' own steps stay small, so a click shows.
std::vector<float> guitar(int n) {
  std::vector<float> x = sig::sine(110.0, n, kRate, 0.20), b = sig::sine(220.0, n, kRate, 0.05);
  for (int i = 0; i < n; ++i) x[static_cast<std::size_t>(i)] += b[static_cast<std::size_t>(i)];
  return x;
}

struct Run {
  std::vector<float> y;
  rt::Counts counts;
  int swapDoneAt = -1;
};

// Plays x; at `swapAt` loads `second` (on this thread, between blocks, as a control thread would).
Run play(NamAmp& amp, const std::vector<float>& x, int swapAt, const char* second) {
  Run r;
  r.y.resize(x.size());
  for (int i = 0; i + kBlock <= int(x.size()); i += kBlock) {
    if (second && i == swapAt) {
      const std::string err = amp.load(model(second).c_str(), true);
      CHECK(err.empty(), "load: %s", err.c_str());
    }
    rt::arm();
    amp.process(&x[static_cast<std::size_t>(i)], &r.y[static_cast<std::size_t>(i)], kBlock);
    const rt::Counts c = rt::disarm();
    r.counts.news += c.news, r.counts.deletes += c.deletes, r.counts.mallocs += c.mallocs, r.counts.frees += c.frees, r.counts.locks += c.locks;
    if (second && i > swapAt && r.swapDoneAt < 0 && !amp.swapping()) r.swapDoneAt = i;
  }
  return r;
}

NamAmp& fresh(NamAmp& amp, const char* first) {
  Arena mem(gMem, sizeof gMem);
  CHECK(amp.load(model(first).c_str(), true).empty(), "first model");
  amp.prepare(Setup{kRate, kBlock}, mem);
  return amp;
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

}  // namespace

TEST(namSwapIsClickFreeAndKeepsTheLevel) {
  const std::vector<float> x = guitar(48000 * 2);
  static NamAmp a, b, s;
  const Run ra = play(fresh(a, "wavenet_a1_standard.nam"), x, -1, nullptr);
  const Run rb = play(fresh(b, "A2.nam"), x, -1, nullptr);
  const int at = 48000 / 2;
  const Run rs = play(fresh(s, "wavenet_a1_standard.nam"), x, at, "A2.nam");
  CHECK(rs.swapDoneAt > at, "the swap finished (at sample %d)", rs.swapDoneAt);
  const float natural = std::fmax(maxStep(ra.y, 20000, 96000), maxStep(rb.y, 20000, 96000));
  const float step = maxStep(rs.y, 20000, 96000);
  CHECK(step <= natural * 1.5f, "swap step %.4f against the models' own %.4f", step, natural);
  const double la = rms(ra.y, 20000, at), lb = rms(rb.y, 60000, 96000);
  double lo = 1e9, hi = 0.0;
  for (int i = at; i + 960 < rs.swapDoneAt + 960; i += 480) lo = std::fmin(lo, rms(rs.y, i, i + 960)), hi = std::fmax(hi, rms(rs.y, i, i + 960));
  std::printf("     A %.4f, B %.4f, through the swap %.4f..%.4f, step %.4f (natural %.4f)\n", la, lb, lo, hi, step, natural);
  CHECK(hi <= std::fmax(la, lb) * 1.15 && lo >= std::fmin(la, lb) * 0.85, "level stays between the two models (no +3 dB thump)");
  double after = 0.0;
  for (int i = rs.swapDoneAt + 4800; i < 96000; ++i) after = std::fmax(after, std::fabs(rs.y[static_cast<std::size_t>(i)] - rb.y[static_cast<std::size_t>(i)]));
  CHECK(after < 1e-3, "ends on the new model (difference %.3g)", after);
  CHECK(rs.counts.total() == 0, "audio thread: %ld new, %ld delete, %ld malloc, %ld free, %ld lock", rs.counts.news, rs.counts.deletes,
        rs.counts.mallocs, rs.counts.frees, rs.counts.locks);
  // Negative control: a hard cut from model A's output to model B's steps by far more than the bound.
  float cut = 0.0f;
  for (int i = 30000; i < 30400; i += 7) cut = std::fmax(cut, std::fabs(rb.y[static_cast<std::size_t>(i)] - ra.y[static_cast<std::size_t>(i - 1)]));
  CHECK(cut > natural * 1.5f, "a hard cut steps %.4f against the bound %.4f", cut, natural * 1.5f);
}

// A NAM model answers silence with a constant; the 5 Hz DC blocker on every voice is what keeps it from the output.
TEST(theDcBlockerRemovesTheConstantAModelAnswersSilenceWith) {
  const std::vector<float> silence(48000, 0.0f);
  static NamAmp on, off;
  off.setDcBlock(false);
  const Run ron = play(fresh(on, "A2.nam"), silence, -1, nullptr);
  const Run roff = play(fresh(off, "A2.nam"), silence, -1, nullptr);
  const double with = rms(ron.y, 24000, 48000), without = rms(roff.y, 24000, 48000);
  std::printf("     silence in: blocker on %.2g, off %.2g (rms)\n", with, without);
  CHECK(without > 1e-4, "negative control: without the blocker the model's constant comes through (%.3g)", without);
  CHECK(with < without * 0.05, "with the blocker it does not (%.3g against %.3g)", with, without);
}

TEST(namLoadWhileSwappingIsRefusedAndBadFilesDoNotSwap) {
  static NamAmp amp;
  fresh(amp, "wavenet_a1_standard.nam");
  CHECK(!amp.load("/nonexistent.nam", true).empty() && !amp.swapping(), "a missing file: error, no swap, standby given back");
  CHECK(amp.load(model("A2.nam").c_str(), true).empty() && amp.swapping(), "loaded: swapping");
  CHECK(!amp.load(model("lstm.nam").c_str(), true).empty(), "a second load during the swap is refused");
  const std::vector<float> x = guitar(48000);
  play(amp, x, -1, nullptr);
  CHECK(!amp.swapping() && amp.load(model("lstm.nam").c_str(), true).empty(), "after the fade a new swap is accepted");
}

// The handoff with real models across two threads: the control thread loads model after model
// (file read, network build, NAM's Reset and prewarm: all allocation) while the audio thread plays.
// ASan under `make test-nam`, ThreadSanitizer under `make test-nam-tsan` (NAM Core built with it too).
TEST(namSwapUnderAControlThreadStress) {
  static NamAmp amp;
  fresh(amp, "wavenet_a1_standard.nam");
  const char* models[] = {"A2.nam", "lstm.nam", "wavenet.nam", "wavenet_a1_standard.nam"};
  constexpr int kLoads = 14;
  std::atomic<bool> done{false};
  std::atomic<int> accepted{0}, refused{0}, nonFinite{0};
  std::thread control([&] {
    for (int k = 0; k < kLoads; ++k) {
      std::string err;
      while ((err = amp.load(model(models[k % 4]).c_str(), true)) == "a model swap is already under way") std::this_thread::yield();
      (err.empty() ? accepted : refused).fetch_add(1);
    }
    while (amp.swapping()) std::this_thread::yield();
    done.store(true);
  });
  const std::vector<float> x = guitar(kBlock * 64);
  std::vector<float> y(x.size());
  float peak = 0.0f;
  while (!done.load()) {
    for (int i = 0; i + kBlock <= int(x.size()); i += kBlock) {
      amp.process(&x[static_cast<std::size_t>(i)], &y[static_cast<std::size_t>(i)], kBlock);
      for (int j = 0; j < kBlock; ++j) {
        const float v = y[static_cast<std::size_t>(i + j)];
        if (!std::isfinite(v)) nonFinite.fetch_add(1);
        peak = std::fmax(peak, std::fabs(v));
      }
    }
  }
  control.join();
  CHECK(refused.load() == 0 && accepted.load() == kLoads, "%d of %d loads accepted (%d refused)", accepted.load(), kLoads, refused.load());
  CHECK(nonFinite.load() == 0, "%d non-finite samples", nonFinite.load());
  CHECK(peak < 4.0f, "the output stays sane through %d swaps: peak %.3f", kLoads, peak);
  CHECK(!amp.swapping(), "idle at the end");
}

// A switched-off amp that is not run keeps the state it last had (NamAmp's voices have no reset: NAM's own allocates), so on switching
// back on its receptive field holds old input, not the note now playing: the wet side starts wrong and the fade lands on a signal that is
// not the amp's. Kept warm (Bypass's keepWarm) it has heard everything. The control leaves it cold.
std::vector<float> render(Bypass& b, const std::vector<float>& x, int at, bool toOn) {
  Arena mem(gMem, sizeof gMem);
  b.prepare(Setup{kRate, kBlock}, mem);
  std::vector<float> y(x.size());
  for (int i = 0; i + kBlock <= int(x.size()); i += kBlock) {
    if (i == at) b.set(Bypass::Engaged, toOn ? 1.0f : 0.0f);
    b.process(&x[static_cast<std::size_t>(i)], &y[static_cast<std::size_t>(i)], kBlock);
  }
  return y;
}

glitch::Report switchAmpOn(bool keepWarm, double* swell) {
  std::vector<float> x = guitar(48000);
  for (std::size_t i = 0; i < x.size(); ++i) x[i] *= 2.5f;  // a hard strum: the model's nonlinear range
  NamAmp a, b, c;
  CHECK(a.load(model("wavenet_a1_standard.nam").c_str(), true).empty(), "model A1");
  CHECK(b.load(model("wavenet_a1_standard.nam").c_str(), true).empty(), "model A1");
  CHECK(c.load(model("wavenet_a1_standard.nam").c_str(), true).empty(), "model A1");
  Bypass on(a, true, keepWarm), off(b, false, keepWarm), sw(c, false, keepWarm);
  const int at = 24000;
  const std::vector<float> wet = render(on, x, -1, true), dry = render(off, x, -1, false), y = render(sw, x, at, true);
  glitch::Options opt;
  opt.guardMs = 150.0;
  opt.globalContinuity = false;
  const glitch::Report r = glitch::analyse(y, dry, wet, {{std::size_t(at), std::size_t(at) + 480}}, opt);
  if (swell) *swell = r.maxSwellDb;
  std::printf("     amp on, %s: local %.2f of its limit, swell %+.2f dB, worst convergence %.0f dB%s%s\n", keepWarm ? "kept warm" : "cold     ", r.localRatio,
              r.maxSwellDb, r.worstConvergenceDb, r.findings.empty() ? "" : ", ", r.findings.empty() ? "" : r.findings[0].c_str());
  return r;
}

TEST(aKeptWarmAmpSwitchesOnCleanly) {
  const glitch::Report r = switchAmpOn(true, nullptr);
  CHECK(r.ok && r.error.empty(), "%s%s", r.error.c_str(), r.findings.empty() ? "" : r.findings[0].c_str());
}

TEST(aColdAmpSwitchesOnWithAFinding) {
  const glitch::Report r = switchAmpOn(false, nullptr);
  CHECK(!r.ok && r.failed("hf"), "a cold amp is found by the oracle as a burst of high frequencies");
}

#else
TEST(namSwapNeedsNamCore) {}
#endif
