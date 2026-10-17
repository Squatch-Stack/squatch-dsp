// SPDX-License-Identifier: MIT
// Host tests for the Seed3 firmware's measurement code: cycle statistics, the two-bank meter,
// the timed chain and the click detector. The same headers run on the board.
#include <cmath>
#include <string_view>
#include <vector>

#include "../../../tests/check.hpp"
#include "../../../tests/signals.hpp"
#include "../common/click_detector.hpp"
#include "../common/dfu_command.hpp"
#include "../common/gen_signal.hpp"
#include "../common/meter.hpp"
#include "../common/timed_chain.hpp"

using namespace squatch;
using namespace squatch::board;

namespace {

// %u in a CHECK message: uint32_t is unsigned long on some targets.
unsigned u(std::uint32_t v) { return static_cast<unsigned>(v); }

// A clock the tests move by hand.
struct FakeClock {
  static std::uint32_t t;
  static std::uint32_t now() { return t; }
};
std::uint32_t FakeClock::t = 0;

// A block that costs a known number of fake cycles and adds a constant.
class Costly final : public dsp::Block {
 public:
  Costly(std::uint32_t cycles, float add) : cycles_(cycles), add_(add) {}
  void prepare(const dsp::Setup&, dsp::Arena&) override {}
  void process(const float* in, float* out, int n) override {
    for (int i = 0; i < n; ++i) out[i] = in[i] + add_;
    FakeClock::t += cycles_;
  }
  void reset() override {}
  void set(dsp::ParamId, float) override {}
  float get(dsp::ParamId) const override { return NAN; }

 private:
  std::uint32_t cycles_;
  float add_;
};

}  // namespace

TEST(daisyStatsMeanP99Worst) {
  CycleStats s;
  s.configure(12800);  // bins of 100 cycles
  for (int i = 0; i < 990; ++i) s.record(1000);
  for (int i = 0; i < 9; ++i) s.record(5000);
  s.record(40000);  // over 2x the budget: last bin, but worst stays exact
  const CycleSummary r = s.summarize();
  CHECK(r.count == 1000, "count %u", u(r.count));
  CHECK(r.mean == (990u * 1000 + 9 * 5000 + 40000) / 1000, "mean %u", u(r.mean));
  CHECK(r.p99 >= 1000 && r.p99 <= 1100, "p99 %u: the 990th of 1000 is 1000 cycles, to one bin", u(r.p99));
  CHECK(r.worst == 40000, "worst %u", u(r.worst));
  s.clear();
  CHECK(s.summarize().count == 0 && s.summarize().worst == 0, "clear empties it");
}

TEST(daisyStatsPercentFormatting) {
  char buf[80];
  CHECK(basisPoints(3200, 320000) == 100, "1%%");
  formatPercent(buf, sizeof buf, 3200, 320000);
  CHECK(std::string_view(buf) == "1.00", "got %s", buf);
  CycleSummary s{10, 320000, 16000, 16320, 32000};
  formatSummary(buf, sizeof buf, "gate", s);
  CHECK(std::string_view(buf) == "gate 5.00/5.10/10.00", "got %s", buf);
}

TEST(daisyMeterFlipsWindowsAndCountsLateAndOverruns) {
  Meter<3> m;
  m.configure(1000, 4);  // budget 1000 cycles, 4 blocks per window
  Window<3> w;
  CHECK(!m.take(w), "nothing before the first window");
  const std::uint32_t starts[] = {0, 1000, 2000, 3600, 4600};  // the 4th block starts 1600 late
  const std::uint32_t works[] = {100, 1200, 100, 100, 100};    // the 2nd overruns
  for (int i = 0; i < 4; ++i) m.endBlock(starts[i], works[i], 0.25f, 0.5f);
  CHECK(m.take(w), "a window after 4 blocks");
  CHECK(w.seq == 1 && w.blocks == 4, "seq %u blocks %u", u(w.seq), u(w.blocks));
  CHECK(w.late == 1, "late %u", u(w.late));
  CHECK(w.overruns == 1, "overruns %u", u(w.overruns));
  CHECK(w.slot[Meter<3>::kPeriod].count == 3 && w.slot[Meter<3>::kPeriod].worst == 1600, "periods");
  CHECK(w.inPeak == 0.25f && w.outPeak == 0.5f, "peaks");
  CHECK(!m.take(w), "taken once");
  m.endBlock(starts[4], works[4], 0.0f, 0.0f);
  CHECK(!m.take(w), "the next window is not finished");
}

TEST(daisyMeterStretchesWhenTheReaderIsSlow) {
  Meter<2> m;
  m.configure(1000, 2);
  for (std::uint32_t i = 0; i < 2; ++i) m.endBlock(i * 1000, 10, 0.0f, 0.0f);  // window 1: ready
  for (std::uint32_t i = 2; i < 6; ++i) m.endBlock(i * 1000, 10, 0.0f, 0.0f);  // windows 2 and 3 merge
  Window<2> w;
  CHECK(m.take(w) && w.blocks == 2, "the first window, as it was");
  for (std::uint32_t i = 6; i < 8; ++i) m.endBlock(i * 1000, 10, 0.0f, 0.0f);
  CHECK(m.take(w), "the stretched window arrives");
  CHECK(w.blocks == 6 && w.stretched >= 1, "blocks %u stretched %u", u(w.blocks), u(w.stretched));
  CHECK(w.slot[0].count == 6, "every block kept: %u", u(w.slot[0].count));
}

TEST(daisyTimedChainTimesEachStage) {
  static Costly a(300, 1.0f), b(700, 2.0f);
  TimedChain<FakeClock, 4> chain;
  chain.add(a, "a");
  chain.add(b, "b");
  alignas(16) static unsigned char mem[1024];
  dsp::Arena arena(mem, sizeof mem);
  chain.prepare(dsp::Setup{48000.0, 32}, arena);
  CycleStats stats[2];
  stats[0].configure(10000);
  stats[1].configure(10000);
  float in[32], out[32];
  for (float& v : in) v = 0.5f;
  FakeClock::t = 4294967000u;  // wraps during the call: the differences still hold
  chain.process(in, out, 32, stats);
  CHECK(out[31] == 3.5f, "in -> a -> b: %f", out[31]);
  CHECK(stats[0].summarize().worst == 300 && stats[1].summarize().worst == 700, "stage cycles");
  chain.process(out, out, 32, stats);  // in place
  CHECK(out[0] == 6.5f, "in place: %f", out[0]);
  CHECK(std::string_view(chain.name(1)) == "b" && chain.size() == 2, "names");
}

namespace {

// A loop's response to a one-sample click at `pre`: a Hann-windowed sinc centred `lag` samples
// later (a linear-phase converter chain), scaled and optionally inverted, plus noise.
std::vector<float> loopback(int n, int pre, double lag, float gain, float noise) {
  std::vector<float> x = sig::noise(n, noise, 7);
  for (int k = -24; k <= 24; ++k) {
    const int i = pre + static_cast<int>(std::floor(lag)) + k;
    const double t = i - pre - lag;
    const double sinc = t == 0.0 ? 1.0 : std::sin(sig::kPi * 0.9 * t) / (sig::kPi * 0.9 * t) * 0.9;
    const double hann = 0.5 + 0.5 * std::cos(sig::kPi * t / 25.0);
    if (i >= 0 && i < n) x[i] += static_cast<float>(gain * sinc * hann);
  }
  return x;
}

}  // namespace

TEST(daisyClickFoundToAFractionOfASample) {
  const double lags[] = {64.0, 78.4, 131.25};
  for (double lag : lags) {
    const std::vector<float> x = loopback(2304, 256, lag, 0.4f, 1e-4f);
    const ClickResult r = analyzeClick(x.data(), 2304, 256);
    CHECK(r.found, "lag %.2f found", lag);
    CHECK(std::fabs(r.lag - lag) < 0.03, "lag %.2f measured %.3f", lag, r.lag);
    CHECK(r.onset <= r.lag && r.onset >= r.lag - 3, "onset %d before the peak %.2f", r.onset, r.lag);
    CHECK(r.peak > 0.0f, "upright");
  }
  const std::vector<float> inv = loopback(2304, 256, 80.0, -0.3f, 1e-4f);
  CHECK(analyzeClick(inv.data(), 2304, 256).peak < 0.0f, "an inverting loop is reported");
}

TEST(daisyClickNotFoundInNoiseOrSilence) {
  const std::vector<float> noise = sig::noise(2304, 0.01f, 3);
  CHECK(!analyzeClick(noise.data(), 2304, 256).found, "noise alone is not a click");
  std::vector<float> silence(2304, 0.0f);
  CHECK(!analyzeClick(silence.data(), 2304, 256).found, "silence is not a click");
  CHECK(!analyzeClick(silence.data(), 100, 256).found, "a capture shorter than pre is refused");
}

TEST(daisyMedian) {
  const float odd[] = {5.0f, 1.0f, 3.0f};
  const float even[] = {4.0f, 1.0f, 3.0f, 2.0f};
  CHECK(median(odd, 3) == 3.0f && median(even, 4) == 2.5f && median(odd, 0) == 0.0f, "median");
}

TEST(daisyDfuCommandMatchesOnlyAWholeLine) {
  auto fires = [](std::initializer_list<const char*> chunks) {
    DfuMatcher m;
    int n = 0;
    for (const char* c : chunks) n += m.feed(reinterpret_cast<const std::uint8_t*>(c), std::string_view(c).size()) ? 1 : 0;
    return n;
  };
  CHECK(fires({"dfu\n"}) == 1, "dfu LF");
  CHECK(fires({"dfu\r"}) == 1, "dfu CR");
  CHECK(fires({"dfu\r\n"}) == 1, "dfu CRLF fires once");
  CHECK(fires({"d", "f", "u", "\n"}) == 1, "one byte at a time");
  CHECK(fires({"xxdfu\n"}) == 1, "after noise");
  CHECK(fires({"ddfu\n"}) == 1, "a stray d first");
  CHECK(fires({"dfu\n", "dfu\n"}) == 2, "twice");
  CHECK(fires({"dfu"}) == 0, "no newline yet");
  CHECK(fires({"dfux\n"}) == 0, "dfux is not dfu");
  CHECK(fires({"df\nu\n"}) == 0, "a split line");
  CHECK(fires({"# banner: DFU mode\n"}) == 0, "case matters");
  CHECK(fires({"dfux", "dfu\n"}) == 1, "recovers after a miss");
}

TEST(daisyHotSignalSegments) {
  HotSignal g(0.5f, 48000);
  float peak[HotSignal::kSegments] = {};
  for (int seg = 0; seg < HotSignal::kSegments; ++seg) {
    for (int i = 0; i < 96000; ++i) peak[seg] = std::fmax(peak[seg], std::fabs(g.next()));
  }
  CHECK(std::fabs(peak[0] - 0.5f) < 0.001f, "low note peaks at amp: %.4f", peak[0]);
  CHECK(std::fabs(peak[1] - 0.5f) < 0.01f, "bursts peak at amp: %.4f", peak[1]);
  CHECK(std::fabs(peak[2] - 0.35355f) < 0.001f, "fs/4 samples are 0.707 of amp: %.4f", peak[2]);
  CHECK(peak[3] > 0.49f && peak[3] <= 0.5f, "noise fills the range: %.4f", peak[3]);
  CHECK(peak[4] == 0.5f, "square");
  CHECK(peak[5] == 0.0f, "silence");
  CHECK(g.segment() == 0, "twelve seconds later it starts again");
}
