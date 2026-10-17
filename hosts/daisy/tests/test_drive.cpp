// SPDX-License-Identifier: MIT
// Host tests for the drive block the Seed3 chain runs: it reproduces the reference figures recorded
// for the same Faust model built as a desktop plugin (96 kHz, 0.2 V peak at 796 Hz, defaults), takes no memory from the arena, and never allocates or locks
// in process() or set().
#include <algorithm>
#include <cmath>
#include <vector>

#include "../../../tests/check.hpp"
#include "../../../tests/rt_watch.hpp"
#include "../../../tests/signals.hpp"
#include "../faust/drive_block.hpp"

using namespace squatch;
using squatch::board::DriveStage;

namespace {

constexpr int kRate = 96000;

struct Measured {
  double peakGainDb, fundamentalGainDb, thdPercent;
};

// The reference method: three seconds, the last one analysed (796 whole cycles), DC removed,
// harmonics 2 to 10 from a coherent, unwindowed DFT.
Measured measure(DriveStage& t, double amp) {
  const int n = 3 * kRate;
  const std::vector<float> x = sig::sine(796.0, n, kRate, amp);
  std::vector<float> y(static_cast<std::size_t>(n));
  for (int i = 0; i < n; i += 256) t.process(&x[i], &y[i], std::min(256, n - i));
  double mean = 0.0, peak = 0.0;
  for (int i = 2 * kRate; i < n; ++i) mean += y[i];
  mean /= kRate;
  for (int i = 2 * kRate; i < n; ++i) peak = std::fmax(peak, std::fabs(y[i] - mean));
  double h[11] = {};
  for (int k = 1; k <= 10; ++k) {
    double re = 0.0, im = 0.0;
    for (int i = 2 * kRate; i < n; ++i) {
      const double ph = 2.0 * sig::kPi * 796.0 * k * (i - 2 * kRate) / kRate;
      re += (y[i] - mean) * std::cos(ph);
      im += (y[i] - mean) * std::sin(ph);
    }
    h[k] = 2.0 * std::sqrt(re * re + im * im) / kRate;
  }
  double harmonics = 0.0;
  for (int k = 2; k <= 10; ++k) harmonics += h[k] * h[k];
  return {sig::db(peak / amp), sig::db(h[1] / amp), 100.0 * std::sqrt(harmonics) / h[1]};
}

}  // namespace

TEST(daisyDriveMatchesTheReferenceMeasurement) {
  static DriveStage t;
  alignas(16) static unsigned char mem[64];
  dsp::Arena arena(mem, sizeof mem);
  t.prepare(dsp::Setup{kRate, 256}, arena);
  CHECK(t.ready(), "all three sliders bound");
  CHECK(arena.used() == 0, "the Faust state lives in the block, not the arena: %zu", arena.used());
  const Measured m = measure(t, 0.2);
  // The plugin build's recorded figures: 3.463506, 4.529812, 21.234344.
  CHECK(std::fabs(m.peakGainDb - 3.463506) < 1e-3, "peak gain %.6f dB", m.peakGainDb);
  CHECK(std::fabs(m.fundamentalGainDb - 4.529812) < 1e-3, "fundamental gain %.6f dB", m.fundamentalGainDb);
  CHECK(std::fabs(m.thdPercent - 21.234344) < 1e-2, "THD %.6f%%", m.thdPercent);
}

TEST(daisyDriveControlsMoveTheSound) {
  static DriveStage t;
  dsp::Arena none;
  t.prepare(dsp::Setup{kRate, 256}, none);
  const Measured full = measure(t, 0.02);
  t.set(DriveStage::Drive, 0.0f);
  t.reset();
  const Measured low = measure(t, 0.02);
  CHECK(full.fundamentalGainDb > low.fundamentalGainDb + 6.0, "drive raises the gain: %.2f vs %.2f dB",
        full.fundamentalGainDb, low.fundamentalGainDb);
  t.set(DriveStage::Level, 0.25f);
  t.reset();
  const Measured quiet = measure(t, 0.02);
  CHECK(std::fabs(quiet.fundamentalGainDb - (low.fundamentalGainDb - 6.0206)) < 0.05, "level halves: %.3f dB",
        quiet.fundamentalGainDb - low.fundamentalGainDb);
  t.set(DriveStage::Level, NAN);  // the descriptor's default (0.5) rather than NaN in the audio
  t.set(99, 1.0f);           // an unknown id is ignored
  t.reset();
  CHECK(std::fabs(measure(t, 0.02).fundamentalGainDb - low.fundamentalGainDb) < 0.05, "NaN level -> default");
}

TEST(daisyDriveNeverAllocatesOrLocks) {
  static DriveStage t;
  dsp::Arena none;
  t.prepare(dsp::Setup{48000.0, 64}, none);
  static float buf[64];
  sig::Rng rng{5};
  long total = 0;
  for (int call = 0; call < 2000; ++call) {
    const int n = call % 3 == 0 ? 1 : rng.range(1, 64);
    for (int i = 0; i < n; ++i) buf[i] = rng.uniform();
    rt::arm();
    if (call % 4 == 0) t.set(static_cast<dsp::ParamId>(call % 3), rng.uniform());
    t.process(buf, buf, n);
    total += rt::disarm().total();
    for (int i = 0; i < n; ++i) CHECK(std::isfinite(buf[i]), "finite output");
  }
  CHECK(total == 0, "%ld allocations or locks in process()/set()", total);
  CHECK(rt::mallocMeasured(), "malloc was watched (the sanitizer allocator is linked)");
}

// The negative control for the test above, in this binary: the watch does see an allocation.
TEST(daisyRtWatchSeesAnAllocation) {
  static int* volatile keep = nullptr;
  rt::arm();
  keep = new int(1);
  delete keep;
  const rt::Counts c = rt::disarm();
  CHECK(c.news >= 1 && c.deletes >= 1, "news %ld deletes %ld", c.news, c.deletes);
}
