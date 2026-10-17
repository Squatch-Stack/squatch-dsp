// SPDX-License-Identifier: MIT
// The limiter is ear safety, so it is tested hard: no output sample ever exceeds the ceiling, on
// any signal, at any ceiling, in any block size; neither does the waveform between samples (true
// peak, measured at 16x with a 96-tap reference interpolator) at 44.1, 48 and 96 kHz; and it
// matches its double-precision definition.
#include <cstdint>
#include <cstring>
#include <squatch/dsp/blocks/limiter.hpp>

#include "check.hpp"
#include "golden.hpp"
#include "reference.hpp"
#include "signals.hpp"

using namespace squatch::dsp;

#if __has_include("golden/limiter.inc")
static const double kGolden[golden::kSegments] =
#include "golden/limiter.inc"
    ;
static const double* const kGoldenRender = kGolden;
#else
static const double* const kGoldenRender = nullptr;
#endif

namespace {

#if defined(__clang__) && defined(__aarch64__)
// Taken from the limiter before the look-ahead option existed (Apple clang, arm64, the Makefile's test flags).
constexpr std::uint64_t kDefaultTortureHash = 0xcb148b4795168880ull;
#endif
constexpr double kRate = 48000.0;
alignas(64) unsigned char gMem[1 << 16];

struct Rig {
  Limiter lim;
  explicit Rig(float ceilingDb, float releaseMs = 50.0f, double rate = kRate, int lookahead = 0) {
    Arena mem(gMem, sizeof gMem);
    lim.setLookaheadSamples(lookahead);
    lim.set(Limiter::Ceiling, ceilingDb);
    lim.set(Limiter::Release, releaseMs);
    lim.prepare(Setup{rate, 256}, mem);
  }

  /// Runs x through in random block sizes (1 and 256 included).
  std::vector<float> run(const std::vector<float>& x, std::uint32_t seed = 1) {
    std::vector<float> y(x.size());
    sig::Rng rng{seed};
    for (std::size_t i = 0; i < x.size();) {
      const int n = std::min<int>(static_cast<int>(x.size() - i), rng.range(0, 3) == 0 ? 1 : rng.range(1, 256));
      lim.process(&x[i], &y[i], n);
      i += static_cast<std::size_t>(n);
    }
    return y;
  }
};

std::vector<float> concat(std::initializer_list<std::vector<float>> parts) {
  std::vector<float> x;
  for (const auto& p : parts) x.insert(x.end(), p.begin(), p.end());
  return x;
}

// Signals chosen to break a limiter: full-scale and over-scale squares (including fs/2), inter-
// sample overs, noise far over the ceiling, lone spikes, bursts out of silence, and non-audio.
std::vector<std::vector<float>> torture() {
  const int n = 12000;
  std::vector<std::vector<float>> s;
  for (int period : {2, 3, 4, 7, 16, 100, 441}) s.push_back(sig::square(period, n, 1.0f));
  s.push_back(sig::square(50, n, 10.0f));
  s.push_back(sig::sine(kRate / 4, n, kRate, 1.0, sig::kPi / 4));  // samples at 0.707, true peak 1
  s.push_back(sig::sine(kRate / 4, n, kRate, 4.0, sig::kPi / 4));
  s.push_back(sig::sine(0.45 * kRate, n, kRate, 2.0, 0.3));
  s.push_back(sig::noise(n, 4.0f, 11));
  std::vector<float> spikes(n, 0.0f);
  for (int i = 5; i < n; i += 997) spikes[i] = i % 2 ? 8.0f : -8.0f;
  s.push_back(spikes);
  s.push_back(concat({std::vector<float>(3000, 0.0f), sig::noise(n - 3000, 1.0f, 5)}));
  std::vector<float> junk = sig::noise(n, 0.5f, 9);
  for (int i = 100; i < n; i += 500) junk[i] = i % 3 == 0 ? NAN : (i % 3 == 1 ? INFINITY : -1e30f);
  s.push_back(junk);
  return s;
}

}  // namespace

TEST(limiterNeverExceedsCeiling) {
  const auto signals = torture();
  for (float ceilingDb : {0.0f, -1.0f, -6.0f, -20.0f}) {
    const float ceiling = dbToGain(ceilingDb);
    for (std::size_t k = 0; k < signals.size(); ++k) {
      Rig r(ceilingDb, 20.0f + 100.0f * float(k % 3));
      const std::vector<float> y = r.run(signals[k], static_cast<std::uint32_t>(k + 1));
      bool finite = true;
      for (float v : y) finite = finite && std::isfinite(v);
      CHECK(finite, "signal %zu: non-finite output", k);
      CHECK(sig::peak(y) <= ceiling, "signal %zu at %g dB: peak %.9g > ceiling %.9g", k, ceilingDb, sig::peak(y),
            ceiling);
    }
  }
}

// The hard clamp guarantees the sample peak; this shows the gain path does the work itself: its
// double-precision definition, with no clamp, stays under the ceiling on the same signals.
TEST(limiterDefinitionHoldsWithoutClamp) {
  const auto signals = torture();
  for (std::size_t k = 0; k < signals.size() - 1; ++k) {  // the last one is the non-audio case
    const ref::Limiter def{std::pow(10.0, -1.0 / 20.0), 50.0, kRate};
    double peak = 0.0;
    for (double v : def.run(signals[k])) peak = std::max(peak, std::fabs(v));
    CHECK(peak <= def.ceiling * (1.0 + 1e-12), "signal %zu: definition peaks %.12g", k, peak);
  }
}

namespace {

std::vector<float> onset(double hz, int n, double rate, double amp, double phase) {
  return concat({std::vector<float>(3000, 0.0f), sig::sine(hz, n - 3000, rate, amp, phase)});
}

// Noise band-limited to 20 kHz (eight cookbook lowpasses).
std::vector<float> bandLimitedNoise(int n, float amp, std::uint32_t seed, double rate) {
  const std::vector<float> x = sig::noise(n, amp, seed);
  std::vector<double> d(x.begin(), x.end());
  const double w0 = 2.0 * sig::kPi * 20000.0 / rate, a = std::sin(w0) / (2.0 * 0.7071), c = std::cos(w0);
  for (int i = 0; i < 8; ++i) d = ref::filter({(1 - c) / 2, 1 - c, (1 - c) / 2, 1 + a, -2 * c, 1 - a}, d);
  return std::vector<float>(d.begin(), d.end());
}

// Everything that makes inter-sample peaks: tones to 20 kHz at several phases, squares, hard
// onsets, band-limited noise, white noise with full power up to fs/2, a sweep and gated noise.
std::vector<std::vector<float>> interSampleSet(double rate, int n) {
  std::vector<std::vector<float>> s = {sig::sine(rate / 4, n, rate, 2.0, sig::kPi / 4)};
  for (double hz : {8000.0, 11025.0, 15000.0, 18000.0, 19000.0, 20000.0})
    for (double phase : {0.3, 1.1, 2.0}) s.push_back(sig::sine(hz, n, rate, 2.0, phase));
  for (int period : {2, 3, 5, 7, 16, 33, 100, 441}) s.push_back(sig::square(period, n, 1.5f));
  for (double hz : {1000.0, 5000.0, 15000.0, 19000.0, 20000.0}) s.push_back(onset(hz, n, rate, 3.0, 0.7));
  for (std::uint32_t seed : {3u, 4u}) s.push_back(bandLimitedNoise(n, 8.0f, seed, rate));
  for (std::uint32_t seed : {5u, 6u, 7u}) s.push_back(sig::noise(n, 2.0f + 6.0f * float(seed - 5), seed));
  std::vector<float> sweep(static_cast<std::size_t>(n)), gated = sig::noise(n, 1.0f, 77);
  for (int i = 0; i < n; ++i) sweep[i] = float(2.0 * std::sin(2 * sig::kPi * (20.0 * i / rate + 40000.0 * i * i / (rate * rate))));
  for (int i = 0; i < n; ++i) gated[i] *= (i / 2400) % 2 ? 4.0f : 0.2f;
  s.push_back(sweep);
  s.push_back(gated);
  return s;
}

}  // namespace

TEST(limiterHoldsTruePeak) {
  for (double rate : {44100.0, 48000.0, 96000.0}) {
    const auto signals = interSampleSet(rate, 12000);
    double worst = -100.0;
    std::size_t worstAt = 0;
    for (std::size_t k = 0; k < signals.size(); ++k) {
      Rig r(-1.0f, 50.0f, rate);
      const double over = sig::db(ref::truePeak(r.run(signals[k]))) + 1.0;
      if (over > worst) worst = over, worstAt = k;
    }
    CHECK(worst <= 0.0, "%.0f Hz: signal %zu true peak %+.4f dB over the ceiling", rate, worstAt, worst);
    std::printf("     %.1f kHz: worst true peak vs ceiling %+.4f dB (signal %zu of %zu)\n", rate / 1000, worst, worstAt,
                signals.size());
  }
  const std::vector<float> over = sig::sine(kRate / 4, 4800, kRate, 1.0, sig::kPi / 4);
  CHECK(sig::db(ref::truePeak(over) / sig::peak(over)) > 2.9, "the fs/4 signal has a 3 dB inter-sample over");
}

TEST(limiterLatencyAndTransparency) {
  Rig r(-1.0f);
  const int lat = r.lim.latency();
  CHECK(lat == 72 + 16, "latency %d samples at 48 kHz (1.5 ms lookahead + half the interpolator)", lat);
  std::vector<float> x = sig::sine(110.0, 4000, kRate, 0.3);  // true peak under the ceiling: bit-exact
  const std::vector<float> mid = sig::sine(1000.0, 4000, kRate, 0.25, 0.4);
  for (std::size_t i = 0; i < x.size(); ++i) x[i] += mid[i];
  x[1000] = 0.8f;
  const std::vector<float> y = r.run(x);
  bool exact = true;
  for (std::size_t i = 0; i + lat < x.size(); ++i) exact = exact && y[i + lat] == x[i];
  CHECK(exact, "below the ceiling the output is the input, delayed by the latency");
}

TEST(limiterSettlesAtCeiling) {
  Rig r(-6.0f);
  const std::vector<float> y = r.run(sig::sine(440.0, 48000, kRate, 2.0));  // +6 dBFS into -6
  const float tail = peakAbs(&y[24000], 24000);
  CHECK(gainToDb(tail) <= -6.0f && gainToDb(tail) > -6.1f, "a steady tone sits just under the ceiling: %.3f dB",
        gainToDb(tail));
}

TEST(limiterReleases) {
  Rig r(-6.0f, 100.0f);
  const std::vector<float> x = concat({sig::sine(440.0, 4800, kRate, 4.0), sig::sine(440.0, 48000, kRate, 0.1)});
  const std::vector<float> y = r.run(x);
  const double after100ms = sig::amplitudeAt(y, 440.0, kRate, 4800 + 88 + 4800, 4800 + 88 + 5280) / 0.1;
  const double after1s = sig::amplitudeAt(y, 440.0, kRate, 48000, 52800) / 0.1;
  CHECK(after100ms > 0.5 && after100ms < 0.8, "one time constant in: gain %.3f", after100ms);
  CHECK(after1s > 0.999, "recovered after 1 s: gain %.5f", after1s);
}

TEST(limiterCeilingCutAppliesAtOnce) {
  Rig r(0.0f);
  const std::vector<float> x = sig::sine(440.0, 9600, kRate, 2.0);
  std::vector<float> y(x.size());
  r.lim.process(x.data(), y.data(), 4800);
  r.lim.set(Limiter::Ceiling, -12.0f);
  r.lim.process(&x[4800], &y[4800], 4800);
  CHECK(peakAbs(&y[4800], 4800) <= dbToGain(-12.0f), "the next sample already obeys the new ceiling");
}

TEST(limiterArenaTooSmallIsSilent) {
  Limiter lim;
  Arena measure;
  lim.prepare(Setup{kRate, 64}, measure);
  CHECK(measure.failed() && measure.required() > 0, "a measuring arena reports %zu bytes", measure.required());
  alignas(16) static unsigned char small[64];
  Arena tiny(small, sizeof small);
  lim.prepare(Setup{kRate, 64}, tiny);
  float x[64], y[64];
  for (float& v : x) v = 0.5f;
  lim.process(x, y, 64);
  CHECK(tiny.failed() && !lim.ready() && peakAbs(y, 64) == 0.0f, "out of memory: reported, and silent");
  static unsigned char exact[4096];
  Arena enough(exact, measure.required());
  lim.prepare(Setup{kRate, 64}, enough);
  CHECK(!enough.failed() && lim.ready(), "the measured size is enough");
}

TEST(limiterGolden) {
  const std::vector<float> x = concat({sig::noise(8192, 0.3f, 21), sig::sine(110.0, 16384, kRate, 3.0),
                                       sig::square(37, 8192, 1.5f), sig::noise(32768, 0.2f, 22)});
  Rig r(-3.0f, 80.0f);
  const std::vector<float> y = r.run(x, 77);
  const double e = sig::esr(y, ref::Limiter{std::pow(10.0, -3.0 / 20.0), 80.0, kRate}.run(x));
  CHECK(e < 1e-9, "ESR against the double-precision definition %.3g", e);
  std::printf("     limiter ESR vs reference: %.3g\n", e);
  golden::check("limiter", y, kGoldenRender, 1e-5);
}

namespace {

// FNV-1a over the bit patterns of a rendering.
std::uint64_t hashBits(const std::vector<float>& y, std::uint64_t h = 1469598103934665603ull) {
  for (float v : y) {
    std::uint32_t b;
    std::memcpy(&b, &v, sizeof b);
    for (int k = 0; k < 4; ++k) h = (h ^ ((b >> (8 * k)) & 0xffu)) * 1099511628211ull;
  }
  return h;
}

std::uint64_t hashOfTorture(int lookahead, bool set) {
  const auto signals = torture();
  std::uint64_t h = 1469598103934665603ull;
  for (std::size_t k = 0; k < signals.size(); ++k) {
    Limiter lim;
    if (set) lim.setLookaheadSamples(lookahead);
    Arena mem(gMem, sizeof gMem);
    lim.set(Limiter::Ceiling, -1.0f);
    lim.set(Limiter::Release, 20.0f + 100.0f * float(k % 3));
    lim.prepare(Setup{kRate, 256}, mem);
    std::vector<float> y(signals[k].size());
    lim.process(signals[k].data(), y.data(), static_cast<int>(y.size()));
    h = hashBits(y, h);
  }
  return h;
}

}  // namespace

// The default look-ahead (1.5 ms, 72 samples at 48 kHz) stays exactly what it was.
TEST(limiterDefaultIsBitIdentical) {
  const std::uint64_t h = hashOfTorture(0, false);
  CHECK(hashOfTorture(0, true) == h, "setLookaheadSamples(0) is the default");
  CHECK(hashOfTorture(72, true) == h, "72 samples is exactly the default 1.5 ms at 48 kHz");
  CHECK(hashOfTorture(71, true) != h, "71 samples really is different (the test can tell)");
#if defined(__clang__) && defined(__aarch64__)
  CHECK(h == kDefaultTortureHash, "default rendering hash %016llx, recorded before the option: %016llx",
        static_cast<unsigned long long>(h), static_cast<unsigned long long>(kDefaultTortureHash));
#endif
}

// Short look-ahead (setLookaheadSamples): the latency is n + 16, the ceiling holds at every length, and the price is
// distortion on hot low notes and more work for the final clamp. The table is printed so a change shows up in the log.
namespace {

// THD of a steady, hot sine through a limiter at -1 dB: harmonics 2..10 against the fundamental over the last 0.5 s.
double hotThd(double hz, int lookahead) {
  Rig hot(-1.0f, 120.0f, kRate, lookahead);
  const std::vector<float> y = hot.run(sig::sine(hz, 48000, kRate, 2.0));
  const double f = sig::amplitudeAt(y, hz, kRate, 24000, 48000);
  double h2 = 0.0;
  for (int k = 2; k <= 10; ++k) {
    const double a = sig::amplitudeAt(y, hz * k, kRate, 24000, 48000);
    h2 += a * a;
  }
  return 100.0 * std::sqrt(h2) / f;
}

}  // namespace

// Short look-ahead (setLookaheadSamples): the latency is n + 16, the ceiling holds at every length, and the price is
// distortion on hot low notes and more work for the final clamp. The table is printed so a change shows up in the log.
TEST(limiterShortLookahead) {
  std::printf("     lookahead latency  THD@82Hz THD@110Hz THD@440Hz (+7 dB over)  clampHits(torture)  worst true peak vs ceiling\n");
  for (int n : {0, 32, 16}) {
    const int expectLookahead = n == 0 ? 72 : n;
    {
      Rig r(-1.0f, 120.0f, kRate, n);
      CHECK(r.lim.latency() == expectLookahead + 16, "lookahead %d: latency %d", n, r.lim.latency());
    }
    const double t82 = hotThd(82.41, n), t110 = hotThd(110.0, n), t440 = hotThd(440.0, n);
    // The ceiling, on everything that breaks a limiter, and how often the clamp had to step in.
    const double ceiling = dbToGain(-1.0f);
    std::uint64_t hits = 0;
    const auto signals = torture();
    for (std::size_t k = 0; k < signals.size(); ++k) {
      Rig r(-1.0f, 20.0f + 100.0f * float(k % 3), kRate, n);
      CHECK(sig::peak(r.run(signals[k], static_cast<std::uint32_t>(k + 1))) <= ceiling, "lookahead %d signal %zu: sample peak over the ceiling", n, k);
      hits += r.lim.clampHits();
    }
    double worst = -100.0;
    const auto inter = interSampleSet(kRate, 12000);
    for (const auto& x : inter) {
      Rig r(-1.0f, 50.0f, kRate, n);
      worst = std::max(worst, sig::db(ref::truePeak(r.run(x))) + 1.0);
    }
    std::printf("     %9d %7d  %7.3f%% %8.3f%% %8.3f%% %22llu  %+.4f dB\n", expectLookahead, expectLookahead + 16, t82, t110, t440,
                static_cast<unsigned long long>(hits), worst);
    CHECK(worst <= 0.0, "lookahead %d: a true peak %+.4f dB over the ceiling", n, worst);
  }
}
