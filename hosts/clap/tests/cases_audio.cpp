// SPDX-License-Identifier: MIT
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <string>
#include <thread>

#include "cases.hpp"
#include "wav.hpp"

using host::Audio;
using host::Events;
using host::Host;
using host::Plugin;

namespace cases {
namespace {

constexpr int kFrames = 1024;

struct Rng {
  std::uint32_t s = 1;
  std::uint32_t next() { return s = s * 1664525u + 1013904223u; }
  float uniform() { return static_cast<float>(next() >> 8) / 8388608.0f - 1.0f; }  // [-1, 1)
  int range(int lo, int hi) { return lo + static_cast<int>(next() % static_cast<std::uint32_t>(hi - lo + 1)); }
};

void fill(Audio& a, Rng& rng, float amp) {
  for (auto& ch : a.in)
    for (float& x : ch) x = amp * rng.uniform();
}

// Random values for every automatable parameter, at random times inside the buffer.
void randomEvents(const Plugin& pl, Events& ev, Rng& rng, uint32_t frames) {
  ev.clear();
  uint32_t t = 0;
  for (uint32_t k = 0; k < pl.count() && frames > 0; ++k) {
    const clap_param_info_t i = pl.info(k);
    if (!(i.flags & CLAP_PARAM_IS_AUTOMATABLE) || rng.range(0, 2) != 0) continue;
    t = std::min(frames - 1, t + static_cast<uint32_t>(rng.range(0, 64)));
    const double v = i.min_value + (i.max_value - i.min_value) * 0.5 * (rng.uniform() + 1.0);
    ev.add(t, i.id, (i.flags & CLAP_PARAM_IS_STEPPED) ? std::round(v) : v);
  }
}

bool call(const Plugin& pl, Audio& a, Events& ev, uint32_t frames) {
  a.frames(frames);
  a.process.in_events = &ev.in;
  a.process.out_events = &ev.out;
  if (processWatched(pl.p, &a.process) != CLAP_PROCESS_CONTINUE) return false;
  for (uint32_t c = 0; c < a.outBuf.channel_count; ++c)
    if (!clean(a.outPtr[c], frames)) return false;
  return true;
}

// The shapes hosts actually send: 1 sample, the maximum, anything between, more than the maximum
// (split by the plugin), in place and not, and channel counts that disagree with the
// configuration (a mono buffer on a stereo configuration, and the reverse; no input at all).
struct Shape {
  int inCh, outCh;
  bool inPlace;
  uint32_t frames;
};

Shape shapeOf(int config, int n, Rng& rng) {
  const int cfgIn = config == 2 ? 2 : 1, cfgOut = config == 0 ? 1 : 2;
  const bool odd = n % 7 == 3;  // the channel counts the configuration did not promise
  Shape s{odd ? 3 - cfgIn : cfgIn, odd ? 3 - cfgOut : cfgOut, false, 1};
  s.inPlace = n % 2 == 0 && s.inCh == s.outCh;
  if (n % 11 == 5) s.inCh = 0;  // no input buffer at all
  s.frames = n % 3 == 0 ? 1 : n % 3 == 1 ? 512 : static_cast<uint32_t>(rng.range(2, 511));
  if (n == 40) s.frames = 1000;  // over the activated maximum
  return s;
}

int oneCall(const Plugin& pl, int config, int n, Rng& rng) {
  const Shape shape = shapeOf(config, n, rng);
  Audio a(kFrames, shape.inCh, shape.outCh, shape.inPlace);
  fill(a, rng, n % 5 == 0 ? 0.001f : 0.9f);
  Events ev;
  randomEvents(pl, ev, rng, shape.frames);
  return call(pl, a, ev, shape.frames) ? 0 : 1;
}

// Where a small impulse (-26 dBFS, so the clippers stay near linear: trail_od's peak moves from 32 to 39 samples at
// 0.25) peaks in the output, in samples. This is the group delay of a linear-phase block (a FIR's centre tap) or the
// delay of a pure delay; -1 when the call fails or nothing sounds.
int impulsePeak(const Plugin& pl) {
  Audio a(kFrames, 2, 2, false);
  a.in[0][0] = a.in[1][0] = 0.05f;
  Events ev;
  if (!call(pl, a, ev, 512)) return -1;
  int at = -1;
  float best = 1e-9f;
  for (int i = 0; i < 512; ++i)
    if (std::fabs(a.out[0][i]) > best) best = std::fabs(a.out[0][i]), at = i;
  return at;
}

// Is a reported latency the delay the output really has? A block that reports a latency has a pure or linear-phase
// delay (a FIR's centre tap, a look-ahead), and its impulse peaks there to within a sample. A block that reports 0
// still lags by the phase of its analog-model filters (claw and crunch peak 6 samples after the impulse; their
// oversampling is minimum-phase IIR, which has nothing to compensate), but a hidden FIR or look-ahead would put the
// peak well past kAnalogLag, so a 0 report is trusted only up to there.
constexpr int kAnalogLag = 8;
bool latencyMatches(int peak, uint32_t reported) {
  if (peak < 0) return false;
  if (reported == 0) return peak <= kAnalogLag;
  return std::abs(peak - static_cast<int>(reported)) <= 1;
}

// The check is only worth having if it can fail: every kind of wrong report must be refused, for this plugin's real
// measurement. Returns how many wrong reports were wrongly accepted.
int lyingReportsAccepted(int peak, uint32_t reported) {
  int accepted = 0;
  accepted += latencyMatches(peak, reported + 3);   // too long
  accepted += latencyMatches(peak, reported + 40);  // far too long
  if (reported >= 4) accepted += latencyMatches(peak, reported - 3);  // too short
  if (peak > kAnalogLag) accepted += latencyMatches(peak, 0);         // a FIR that reports nothing
  return accepted;
}

}  // namespace

void blockSizesAndLayouts(const Subject& s) {
  for (int config = 0; config < 3; ++config) {
    Host h;
    Plugin pl(s.entry, h);
    pl.configs->select(pl.p, static_cast<clap_id>(config));
    HCHECK(pl.p->activate(pl.p, 48000, 1, 512) && pl.p->start_processing(pl.p), "%s: activates", s.key);
    Rng rng{static_cast<std::uint32_t>(config + 7)};
    int bad = 0;
    for (int n = 0; n < 300; ++n) bad += oneCall(pl, config, n, rng);
    HCHECK(bad == 0, "%s config %d: %d calls failed or gave non-finite or subnormal output", s.key, config, bad);
    pl.p->stop_processing(pl.p);
    pl.p->deactivate(pl.p);
  }
}

namespace {

// Samples of y that are not the input `latency` samples earlier (history holds all input so far).
int misaligned(const Audio& y, const std::vector<float>& history, std::size_t at, uint32_t latency) {
  int bad = 0;
  for (std::size_t i = 0; i < 256; ++i) bad += y.out[0][i] != history[at + i - latency];
  return bad;
}

int different(const Audio& x, const Audio& y) {
  int bad = 0;
  for (std::size_t i = 0; i < 256; ++i) bad += y.out[0][i] != x.out[0][i] || y.out[1][i] != x.out[1][i];
  return bad;
}

}  // namespace

// Bypassed, the output is the input delayed by the latency, bit for bit; and because the block
// kept processing, once the bypass is released the output is exactly that of a plugin that was
// never bypassed.
void bypassKeepsProcessingAligned(const Subject& s) {
  Host h;
  Plugin a(s.entry, h), b(s.entry, h);
  for (Plugin* pl : {&a, &b}) pl->p->activate(pl->p, 48000, 1, 256), pl->p->start_processing(pl->p);
  const uint32_t latency = b.latency->get(b.p);
  Rng rng{3};
  int wrong = 0, differs = 0;
  std::vector<float> history;
  for (int n = 0; n < 120; ++n) {
    Audio x(256, 2, 2, false), y(256, 2, 2, false);
    fill(x, rng, 0.5f);
    y.in[0] = x.in[0], y.in[1] = x.in[1];
    Events none, toggle;
    if (n == 1 || n == 60) toggle.add(0, b.bypassId(), n == 1 ? 1.0 : 0.0);  // on, then off again
    call(a, x, none, 256), call(b, y, toggle, 256);
    history.insert(history.end(), x.in[0].begin(), x.in[0].end());
    if (n >= 5 && n < 60) wrong += misaligned(y, history, static_cast<std::size_t>(n) * 256, latency);
    if (n >= 65) differs += different(x, y);
  }
  const int misaligned = wrong;
  HCHECK(misaligned == 0, "%s: bypassed output is the input %u samples late (%d samples differ)", s.key, latency, misaligned);
  HCHECK(differs == 0, "%s: released, it matches a never-bypassed plugin (%d samples differ)", s.key, differs);
}

// Latency is what activate() said and stays so; a new sample rate means a new activation, where
// a changed latency is reported through the host's latency extension. The reported latency is
// the real one: a small impulse peaks that many samples later (latencyMatches).
void latencyAndSampleRate(const Subject& s) {
  Host h;
  Plugin pl(s.entry, h);
  pl.p->activate(pl.p, 44100, 1, 512);
  pl.p->start_processing(pl.p);
  const uint32_t l44 = pl.latency->get(pl.p);
  const std::string defaults = pl.save(64);
  const int peak44 = impulsePeak(pl);
  HCHECK(latencyMatches(peak44, l44), "%s: reports %u samples, an impulse peaks %d samples late", s.key, l44, peak44);
  HCHECK(lyingReportsAccepted(peak44, l44) == 0, "%s: a wrong latency report would pass at 44.1 kHz (peak %d, reported %u)", s.key, peak44, l44);
  Rng rng{9};
  for (int n = 0; n < 200; ++n) oneCall(pl, 2, n, rng);
  HCHECK(pl.latency->get(pl.p) == l44 && h.restarts == 0, "%s: latency stable through 200 calls", s.key);
  pl.p->deactivate(pl.p);  // no stop_processing first (clap#515)
  HCHECK(pl.p->activate(pl.p, 96000, 1, 512), "%s: reactivates at 96 kHz after deactivate without stop", s.key);
  pl.p->start_processing(pl.p);
  pl.load(defaults, 64);  // the random calls may have muted the tuner or engaged the bypass
  const uint32_t l96 = pl.latency->get(pl.p);
  const int peak96 = impulsePeak(pl);
  HCHECK(latencyMatches(peak96, l96), "%s: reports %u samples at 96 kHz, an impulse peaks %d samples late", s.key, l96, peak96);
  HCHECK(lyingReportsAccepted(peak96, l96) == 0, "%s: a wrong latency report would pass at 96 kHz (peak %d, reported %u)", s.key,
         peak96, l96);
  HCHECK((l96 == l44) == (h.latencyChanges == 0), "%s: %u -> %u samples, %d reports", s.key, l44, l96, h.latencyChanges.load());
  HCHECK(pl.tail->get(pl.p) >= l96, "%s: tail %u covers the latency", s.key, pl.tail->get(pl.p));
  pl.p->stop_processing(pl.p);
  pl.p->deactivate(pl.p);
}

}  // namespace cases
