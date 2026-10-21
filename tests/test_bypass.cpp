// SPDX-License-Identifier: MIT
// Bypass: switching a block (a Cabinet here) on and off is a crossfade, never a step; no
// allocation or lock; garbage-proof; and the glitch oracle (tests/glitch.hpp) finds no click, drop,
// burst or missed landing in either direction. The click test has a negative control: the immediate switch
// the rig used to do must exceed the bound, or the bound is not measuring anything.
#include <squatch/dsp/blocks/bypass.hpp>
#include <squatch/dsp/blocks/cabinet.hpp>
#include <squatch/dsp/blocks/limiter.hpp>

#include "check.hpp"
#include "glitch.hpp"
#include "rt_watch.hpp"
#include "signals.hpp"

using namespace squatch::dsp;

namespace {

alignas(64) unsigned char gMem[1 << 20];
// The largest sample-to-sample step a switch may add. An immediate switch of the test cabinet
// steps by about 1.1; a fade measures about 0.05 (the cabinet starting from silence).
constexpr float kClick = 0.08f;


// A cabinet whose output differs a lot from its input, behind a Bypass.
struct Rig {
  Cabinet cab;
  Bypass bypass{cab, true};
  std::vector<float> ir;
  Rig() {
    ir = sig::noise(480, 0.5f, 3);
    for (int i = 0; i < 480; ++i) ir[i] *= std::exp(-6.0f * float(i) / 480.0f);
    ir[0] = 0.3f;
    cab.configure(ir.data(), 480, Cabinet::kDefaultPartition, false);  // no cut filters: their 20 Hz state is not under test
  }
};

// The input through the bypass in blocks of `block`, toggling Engaged at the given samples.
std::vector<float> render(Bypass& b, const std::vector<float>& x, int block, const std::vector<std::pair<int, float>>& toggles) {
  Arena mem(gMem, sizeof gMem);
  b.prepare(Setup{48000.0, block}, mem);
  std::vector<float> y(x.size());
  std::size_t next = 0;
  for (int at = 0; at < int(x.size()); at += block) {
    const int n = std::min<int>(block, int(x.size()) - at);
    while (next < toggles.size() && toggles[next].first < at + n) b.set(Bypass::Engaged, toggles[next++].second);
    b.process(&x[at], &y[at], n);
  }
  return y;
}

// The largest sample-to-sample step from `from` on.
float maxStep(const std::vector<float>& y, int from) {
  float m = 0.0f;
  for (std::size_t i = from + 1; i < y.size(); ++i) m = std::fmax(m, std::fabs(y[i] - y[i - 1]));
  return m;
}

}  // namespace

TEST(bypassSwitchingACabinetIsClickFree) {
  const std::vector<float> x = sig::sine(220.0, 24000, 48000.0, 0.5);
  Rig on, off;  // references: always on, always bypassed
  const std::vector<float> wet = render(on.bypass, x, 64, {});
  const std::vector<float> dry = render(off.bypass, x, 64, {{0, 0.0f}});
  const float natural = std::fmax(maxStep(wet, 2000), maxStep(dry, 0));  // the steady signals' own steps
  Rig r;
  const std::vector<float> y = render(r.bypass, x, 64, {{5000, 0.0f}, {12000, 1.0f}, {18000, 0.0f}});
  const float step = maxStep(y, 2000);
  CHECK(step <= kClick, "faded step %.4f exceeds %.2f (steady signals step %.4f)", step, kClick, natural);
  CHECK(!r.bypass.fading() && !r.bypass.engaged(), "ended bypassed");
  for (std::size_t i = 19000; i < y.size(); ++i) CHECK(std::fabs(y[i] - x[i]) < 1e-6f, "settles to the dry input at %zu", i);
  // Negative control: the hard switch (the old behaviour) jumps from wet to dry in one sample.
  float hard = 0.0f;
  for (int i = 5000; i < 5002; ++i) hard = std::fmax(hard, std::fabs(wet[i] - dry[i]));
  hard = std::fmax(hard, std::fabs(wet[5000] - dry[5001]));
  CHECK(hard > 5.0f * kClick, "an immediate switch steps %.4f, which the bound must catch", hard);
}

TEST(bypassSurvivesRapidSwitchingAtAnyBlockSize) {
  const std::vector<float> x = sig::sine(110.0, 20000, 48000.0, 0.5);
  const int blocks[] = {1, 7, 64, 128};
  for (int block : blocks) {
    Rig r;
    std::vector<std::pair<int, float>> t;
    for (int i = 0; i < 40; ++i) t.push_back({3000 + i * 350, i % 2 ? 1.0f : 0.0f});  // faster than a fade
    const std::vector<float> y = render(r.bypass, x, block, t);
    CHECK(maxStep(y, 2000) <= kClick, "block %d: step %.4f", block, maxStep(y, 2000));
    bool finite = true;
    for (float v : y) finite = finite && v == v && std::fabs(v) < 2.0f;
    CHECK(finite, "block %d: finite and bounded", block);
  }
}

TEST(bypassInPlaceMatchesSeparateBuffers) {
  const std::vector<float> x = sig::sine(110.0, 20000, 48000.0, 0.5);
  std::vector<float> in = x, separate(x.size(), 0.0f);
  Rig a, b;
  static unsigned char m1[1 << 20], m2[1 << 20];
  Arena arenaA(m1, sizeof m1), arenaB(m2, sizeof m2);
  a.bypass.prepare(Setup{48000.0, 64}, arenaA);
  b.bypass.prepare(Setup{48000.0, 64}, arenaB);
  for (int at = 0; at + 64 <= int(x.size()); at += 64) {
    if (at == 4096) a.bypass.set(Bypass::Engaged, 0.0f), b.bypass.set(Bypass::Engaged, 0.0f);
    a.bypass.process(&x[at], &separate[at], 64);
    b.bypass.process(&in[at], &in[at], 64);
  }
  int differ = 0;
  for (std::size_t i = 0; i + 64 <= x.size(); ++i) differ += separate[i] != in[i];
  CHECK(differ == 0, "%d samples differ", differ);
}

TEST(bypassNeverAllocatesOrLocksAndIgnoresGarbage) {
  Rig r;
  Arena mem(gMem, sizeof gMem);
  r.bypass.prepare(Setup{48000.0, 128}, mem);
  static float in[128], out[128];
  sig::Rng rng{21};
  rt::Counts sum;
  for (int call = 0; call < 3000; ++call) {
    const int n = call % 4 == 0 ? 1 : rng.range(1, 128);
    for (int i = 0; i < n; ++i) in[i] = rng.uniform();
    const float v = call % 7 == 0 ? NAN : (call % 5 == 0 ? 1e9f : float(call % 2));
    rt::arm();
    if (call % 3 == 0) r.bypass.set(Bypass::Engaged, v);
    r.bypass.process(in, out, n);
    const rt::Counts c = rt::disarm();
    sum.news += c.news, sum.deletes += c.deletes, sum.mallocs += c.mallocs, sum.frees += c.frees, sum.locks += c.locks;
    for (int i = 0; i < n; ++i) CHECK(out[i] == out[i], "finite output");
  }
  CHECK(sum.total() == 0, "%ld new, %ld delete, %ld malloc, %ld free, %ld lock", sum.news, sum.deletes, sum.mallocs, sum.frees, sum.locks);
}

TEST(bypassStaysSilentWhenTheArenaIsTooSmall) {
  Rig r;
  alignas(64) unsigned char tiny[64];
  Arena mem(tiny, sizeof tiny);
  r.bypass.prepare(Setup{48000.0, 128}, mem);
  float in[16], out[16];
  for (float& v : in) v = 1.0f;
  r.bypass.process(in, out, 16);
  for (float v : out) CHECK(v == 0.0f, "silent, as the Block contract says");
}

TEST(bypassPassesTheGlitchOracle) {
  // Broadband: the oracle's high-frequency rule needs material with high frequencies in it.
  std::vector<float> x = sig::sine(220.0, 24000, 48000.0, 0.4);
  const std::vector<float> hiss = sig::noise(24000, 0.1f, 8);
  for (std::size_t i = 0; i < x.size(); ++i) x[i] += hiss[i];
  glitch::Options opt;
  opt.guardMs = 15.0;  // the cabinet restarts from silence when engaged: its 480 taps must settle first
  const int fade = 240;
  for (int block : {1, 64, 128}) {
    Rig on, off, down, up;
    const std::vector<float> wet = render(on.bypass, x, block, {});
    const std::vector<float> dry = render(off.bypass, x, block, {{0, 0.0f}});
    const std::vector<float> off1 = render(down.bypass, x, block, {{6000, 0.0f}});
    const glitch::Report d = glitch::analyse(off1, wet, dry, {{6000, 6000 + fade}}, opt);
    CHECK(d.ok && d.error.empty(), "block %d, engaged -> bypassed: %s%s", block, d.error.c_str(), d.findings.empty() ? "" : d.findings[0].c_str());
    const std::vector<float> on1 = render(up.bypass, x, block, {{0, 0.0f}, {6000, 1.0f}});
    const glitch::Report u = glitch::analyse(on1, dry, wet, {{6000, 6000 + fade}}, opt);
    CHECK(u.ok && u.error.empty(), "block %d, bypassed -> engaged: %s%s", block, u.error.c_str(), u.findings.empty() ? "" : u.findings[0].c_str());
  }
  // The controls use a pure tone, where a step is far above the signal's own curvature.
  const std::vector<float> tone = sig::sine(220.0, 24000, 48000.0, 0.5);
  Rig on, off;
  const std::vector<float> wet = render(on.bypass, tone, 64, {});
  const std::vector<float> dry = render(off.bypass, tone, 64, {{0, 0.0f}});
  std::vector<float> hard = wet, stuck = wet;
  for (std::size_t i = 6000; i < hard.size(); ++i) hard[i] = dry[i];
  for (std::size_t i = 6000; i < stuck.size(); ++i) stuck[i] = 0.5f * (wet[i] + dry[i]);
  const glitch::Report h = glitch::analyse(hard, wet, dry, {{6000, 6000 + fade}}, opt);
  CHECK(!h.ok && h.failed("continuity"), "an immediate switch is a continuity finding");
  CHECK(glitch::analyse(stuck, wet, dry, {{6000, 6000 + fade}}, opt).failed("convergence"), "a fade that never lands is a convergence finding");
}

// A block with latency: the Limiter's lookahead. The dry path is delayed to match, whatever the state,
// and a block switched on runs unheard for its latency before it fades in.
namespace {

constexpr int kAt = 24000;

Bypass limited(Limiter& lim, bool engaged) {
  lim.set(Limiter::Ceiling, -12.0f);  // well under the signal, so the limiter is working
  return Bypass(lim, engaged);
}

std::vector<float> loudWithHiss() {
  std::vector<float> x = sig::sine(220.0, 48000, 48000.0, 0.8);
  const std::vector<float> hiss = sig::noise(48000, 0.02f, 8);
  for (std::size_t i = 0; i < x.size(); ++i) x[i] += hiss[i];
  return x;
}

// A limiter that starts afresh needs its release time (120 ms by default, and its gain smoothing) to find the gain the running one has.
glitch::Options limiterOptions() {
  glitch::Options opt;
  opt.guardMs = 400.0;
  return opt;
}

// One switch at kAt, in blocks of `block`: judged against the always-on and always-off renders.
void checkSwitch(const std::vector<float>& x, const std::vector<float>& wet, const std::vector<float>& dry, int block, bool toEngaged) {
  Limiter lim;
  Bypass b = limited(lim, !toEngaged);
  const std::vector<float> y = render(b, x, block, {{kAt, toEngaged ? 1.0f : 0.0f}});
  const glitch::Edit edit{std::size_t(kAt), std::size_t(kAt) + 240 + 2 * std::size_t(b.latency()) + 128};
  const glitch::Report r = toEngaged ? glitch::analyse(y, dry, wet, {edit}, limiterOptions()) : glitch::analyse(y, wet, dry, {edit}, limiterOptions());
  CHECK(r.ok && r.error.empty(), "block %d, %s: %s%s", block, toEngaged ? "bypassed -> engaged" : "engaged -> bypassed", r.error.c_str(),
        r.findings.empty() ? "" : r.findings[0].c_str());
}

}  // namespace

TEST(bypassAlignsTheDryPathOfABlockWithLatency) {
  const std::vector<float> x = loudWithHiss();
  Limiter l0, l1;
  Bypass on = limited(l0, true), off = limited(l1, false);
  const std::vector<float> wet = render(on, x, 64, {});
  const std::vector<float> dry = render(off, x, 64, {});
  const int lat = on.latency();
  CHECK(lat > 0 && off.latency() == lat, "latency %d is the same on and off", lat);
  int misplaced = 0;
  for (int i = 0; i < int(x.size()); ++i) misplaced += dry[i] != (i < lat ? 0.0f : x[i - lat]);
  CHECK(misplaced == 0, "bypassed is the input delayed by %d (%d samples differ)", lat, misplaced);
  for (int block : {1, 64, 128}) {
    checkSwitch(x, wet, dry, block, false);
    checkSwitch(x, wet, dry, block, true);
  }
  // Negative control: the same fade without the compensation (the dry path early by `lat`) is not
  // the previous chain before the edit, which the oracle's convergence rule must see.
  std::vector<float> naive = wet;
  for (std::size_t i = 0; i < naive.size(); ++i) {
    const double t = i < kAt ? 0.0 : i >= kAt + 240 ? 1.0 : double(i - kAt) / 240.0;
    naive[i] = float((1.0 - t) * x[i] + t * dry[i]);
  }
  const glitch::Report n = glitch::analyse(naive, wet, dry, {{std::size_t(kAt), std::size_t(kAt) + 368}}, limiterOptions());
  CHECK(!n.ok && (n.failed("convergence") || n.failed("continuity")), "an uncompensated dry path is found by the oracle");
}

// A block whose output depends on the input before the switch (a cabinet's tail, a NAM model's receptive field). Switched on from reset(),
// its wet side has not heard the note that is playing, and what it makes of the abrupt start rings on top of the level the chain should
// have: a low note through a resonant cabinet swelled 2 to 4 dB over the louder oracle on real recordings. A kept-warm block has heard it.
namespace {

struct Resonant {
  Cabinet cab;
  std::vector<float> ir;
  Resonant() {  // 110 Hz ring, 50 ms time constant, 100 ms long: a speaker's low resonance
    ir.resize(4800);
    for (int i = 0; i < 4800; ++i) ir[i] = 0.3f * std::exp(-float(i) / 2400.0f) * std::sin(6.2831853f * 110.0f * float(i) / 48000.0f);
    ir[0] = 0.3f;
    cab.configure(ir.data(), int(ir.size()), Cabinet::kDefaultPartition, false);
  }
};

glitch::Report switchOn(bool keepWarm, int block, std::size_t at) {
  std::vector<float> x = sig::sine(82.0, 48000, 48000.0, 0.4);
  const std::vector<float> hiss = sig::noise(48000, 0.01f, 5);
  for (std::size_t i = 0; i < x.size(); ++i) x[i] += hiss[i];
  Resonant a, b, c;
  Bypass on(a.cab, true, keepWarm), off(b.cab, false, keepWarm), sw(c.cab, false, keepWarm);
  const std::vector<float> wet = render(on, x, block, {}), dry = render(off, x, block, {});
  const std::vector<float> y = render(sw, x, block, {{int(at), 1.0f}});
  glitch::Options opt;
  opt.guardMs = 150.0;
  opt.globalContinuity = false;  // rule 5 judges the steps, as in the rig test
  return glitch::analyse(y, dry, wet, {{at, at + 480}}, opt);
}

}  // namespace

TEST(aKeptWarmBlockSwitchesOnWithoutASwell) {
  for (int block : {1, 64, 128})
    for (std::size_t at : {20000u, 20100u, 20217u, 20400u}) {
      const glitch::Report r = switchOn(true, block, at);
      CHECK(r.ok && r.error.empty(), "block %d at %zu: %s%s", block, at, r.error.c_str(), r.findings.empty() ? "" : r.findings[0].c_str());
      CHECK(r.maxSwellDb < 0.5, "block %d at %zu: swell %.2f dB", block, at, r.maxSwellDb);
    }
}

// Negative control: the same switch with the block left cold (the Bypass of before) swells past the level rule at some instant.
TEST(aColdBlockSwellsWhenSwitchedOn) {
  double worst = 0.0;
  bool found = false;
  for (std::size_t at : {20000u, 20100u, 20217u, 20400u}) {
    const glitch::Report r = switchOn(false, 64, at);
    worst = std::max(worst, r.maxSwellDb);
    found = found || r.failed("level");
  }
  CHECK(found, "a cold resonant cabinet swells: worst %.2f dB (the rule allows 1.5)", worst);
  double warm = 0.0;
  for (std::size_t at : {20000u, 20100u, 20217u, 20400u}) warm = std::max(warm, switchOn(true, 64, at).maxSwellDb);
  std::printf("     swell over the louder oracle: cold %.2f dB, kept warm %.2f dB\n", worst, warm);
}

// What keeping warm costs when off, and nothing it should not: the block runs, the output is the dry input exactly.
TEST(aKeptWarmBlockIsHeardByNobodyWhileOff) {
  std::vector<float> x = sig::sine(220.0, 4800, 48000.0, 0.4);
  Resonant r;
  Bypass off(r.cab, false, true);
  Arena mem(gMem, sizeof gMem);
  off.prepare(Setup{48000.0, 64}, mem);
  std::vector<float> y(x.size());
  rt::arm();
  for (std::size_t at = 0; at + 64 <= x.size(); at += 64) off.process(&x[at], &y[at], 64);
  const rt::Counts c = rt::disarm();
  int differ = 0;
  for (std::size_t i = 0; i + 64 <= x.size(); ++i) differ += y[i] != x[i];
  CHECK(differ == 0, "%d samples differ from the input", differ);
  CHECK(c.news + c.deletes + c.mallocs + c.frees + c.locks == 0, "running warm allocates or locks");
}
