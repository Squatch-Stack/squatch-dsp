// SPDX-License-Identifier: MIT
// Tests for the quality suite itself: the offline reference against synthetic truth, the display
// model against the hold rules of hold.js / TunerDisplay.swift, the truth series, the MIDI reader
// and the synth extensions. No framework, as the tuner's own tests. `make tuner-quality-test`.
#include <cstdio>
#include <random>

#include "display_path.hpp"
#include "io.hpp"
#include "regression_cases.hpp"
#include "smf.hpp"
#include "synth_cases.hpp"


using namespace quality;

static int gFailures = 0;
#define CHECK(cond, ...)                                          \
  do {                                                            \
    if (!(cond)) {                                                \
      ++gFailures;                                                \
      std::printf("FAIL %s:%d: %s  ", __FILE__, __LINE__, #cond); \
      std::printf(__VA_ARGS__);                                   \
      std::printf("\n");                                          \
    }                                                             \
  } while (0)

// The public-set loaders' tests use CHECK, so they come after it.
#include "public_cases_test.hpp"  // NOLINT(build/include_order)

static std::vector<float> tone(bench::ToneSpec s, unsigned seed) {
  std::mt19937 rng(seed);
  return bench::synthesize(s, 48000.0, rng);
}

// The reference recovers f0 of a stiff, decaying string: clean, noisy, with hum, on every string.
static void testReferenceRecoversTruth() {
  double worst = 0.0;
  unsigned seed = 1;
  for (const OpenString& o : openStrings())
    for (double snr : {60.0, 30.0})
      for (double hum : {-200.0, -30.0}) {
        bench::ToneSpec s = basePluck(o.midi, o.B, o.t60, 7.3);
        s.glideCents = 0.0;
        s.snrDb = snr;
        s.humDb = hum;
        s.humHarmonics = 5;
        const std::vector<float> x = tone(s, seed++);
        const NoteRef r = referenceNote(x, 48000.0, s.preroll + 0.5, s.preroll + 1.7, {midiHz(o.midi), 60.0, 8});
        const double err = std::fabs(cents(r.fit.f0, s.f0));
        worst = std::max(worst, err);
        CHECK(r.fit.ok && err < 0.1 && err < 4.0 * r.fit.sigmaCents + 0.01, "%s snr %.0f hum %.0f: error %.4f c, sigma %.4f c", o.name, snr, hum, err,
              r.fit.sigmaCents);
        if (snr > 50.0) CHECK(std::fabs(r.fit.B - o.B) < 0.25 * o.B, "%s: B %.3g, true %.3g", o.name, r.fit.B, o.B);
      }
  std::printf("reference, stationary strings: worst error %.4f c\n", worst);
}

// On a gliding pluck the note reference is the window's average; the contour follows the glide.
static void testContourFollowsGlide() {
  bench::ToneSpec s = basePluck(40, 2e-4, 4.0, -3.0);
  s.glideCents = 10.0;
  const std::vector<float> x = tone(s, 99);
  const std::vector<ContourPoint> c = contour(x, 48000.0, s.preroll, s.preroll + s.seconds, s.f0);
  double worst = 0.0;
  for (const ContourPoint& p : c) {
    // A causal 0.6 s window reads the average pitch of the window, which it lags by ~0.3 s.
    const double mid = p.t - s.preroll - 0.3;
    worst = std::max(worst, std::fabs(cents(p.hz, bench::truthAt(s, mid))));
  }
  CHECK(!c.empty() && worst < 0.5, "contour vs glide truth at the window centre: worst %.3f c", worst);
  std::printf("reference contour on a 10 c glide: worst %.3f c against the truth at its window centre\n", worst);
}

// Audio positions for the display-model tests: 48 kHz, from ms.
static constexpr double kRate = 48000.0;
static std::uint64_t at(double ms) { return static_cast<std::uint64_t>(std::llround(ms * kRate / 1000.0)); }

static squatch::host::DisplayReading reading(bool valid, int midi, float cents) {
  squatch::host::DisplayReading d;
  d.valid = valid;
  d.midi = midi;
  d.note = squatch::host::noteName(midi);
  d.cents = cents;
  d.steadyHz = 82.4f;
  d.hz = 82.4f;
  return d;
}

// hold.js: after release, the median of the last 0.5 s of confident readings, for 1.5 s.
static void testHoldAfterRelease() {
  DisplayModel m;
  double t = 0.0;
  for (int i = 0; i < 30; ++i, t += 33.3) m.update(reading(true, 40, i % 2 ? 0.4f : 0.2f), 0.1f, at(t), kRate);
  Frame f = m.update(reading(false, 0, 0.0f), 0.0f, at(t), kRate);
  CHECK(!f.live && f.noteShown && std::fabs(f.shown - 0.3f) < 1e-6f, "held %.2f, want the median 0.3", f.shown);
  f = m.update(reading(false, 0, 0.0f), 0.0f, at(t + 1600.0), kRate);
  CHECK(!f.noteShown, "the note must clear 1.5 s after release");
}

// HOLD_MS counts on the audio clock from the last valid reading (hold.js): at 30 reads a second,
// 1600 samples apart at 48 kHz, the note stays on 44 invalid reads, and the 45th, exactly 1500 ms
// of audio after it, is gone. Every display counts it the same way.
static void testHoldExpiryOnAudioTime() {
  DisplayModel m;
  int k = 0;
  for (; k < 30; ++k) m.update(reading(true, 40, 0.2f), 0.1f, 1600u * static_cast<unsigned>(k), kRate);
  int held = 0;
  for (int i = 0; i < 60; ++i, ++k) held += m.update(reading(false, 0, 0.0f), 0.0f, 1600u * static_cast<unsigned>(k), kRate).noteShown;
  CHECK(held == 44, "the let-go note stays on %d invalid reads, want 44", held);
  squatch::host::TunerFrames frames;  // the same rule in the rows the S3 probe and squatch-tuner-csv write
  frames.reset(48000, 440.0f);
  squatch::tuner::TunerReading r;
  r.valid = true;
  r.hz = r.steadyHz = 82.41f;
  r.uncertainty = 0.1f;
  int rowsHeld = 0;
  for (int i = 0; i < 90; ++i) {
    if (i == 30) r.valid = false;
    frames.advance(frames.untilFrame());
    const squatch::host::FrameRow row = frames.take(r);
    rowsHeld += !row.d.valid && row.shown != squatch::host::FrameRow::Shown::None;
  }
  CHECK(rowsHeld == 44, "TunerFrames holds the let-go note on %d rows, want 44", rowsHeld);
}

// Too short a note (< 250 ms of confident readings) holds the name with "--.-".
static void testHoldTooShort() {
  DisplayModel m;
  for (int i = 0; i < 5; ++i) m.update(reading(true, 45, 1.0f), 0.1f, at(33.3 * i), kRate);
  const Frame f = m.update(reading(false, 0, 0.0f), 0.0f, at(200.0), kRate);
  CHECK(f.noteShown && std::isnan(f.shown), "a 133 ms note must hold --.-, got %.2f", f.shown);
}

// A poll that finds no newer snapshot sees the same audio position: the same reading, not added again.
static void testStaleReadingsNotHeld() {
  DisplayModel m;
  for (int i = 0; i < 20; ++i) m.update(reading(true, 40, 0.0f), 0.1f, at(33.3 * std::min(i, 2)), kRate);
  const Frame f = m.update(reading(false, 0, 0.0f), 0.0f, at(700.0), kRate);
  CHECK(std::isnan(f.shown), "three fresh readings span 67 ms: the hold must show --.-");
}

// A steady tone reads the same through 16- and 64-frame blocks at 48 kHz (both divide 1600).
static void testBlockSizesAgree() {
  bench::ToneSpec s = basePluck(45, 1.5e-4, 3.5, 2.0);
  const std::vector<float> x = tone(s, 5);
  const std::vector<Frame> a = runDisplayPath(x, {48000.0, 16, 30.0, 440.0f}), b = runDisplayPath(x, {48000.0, 64, 30.0, 440.0f});
  CHECK(a.size() == b.size() && framesCsv(a, "") == framesCsv(b, ""), "16 and 64 frame blocks differ (%zu vs %zu frames)", a.size(), b.size());
}

// Reading at given positions (another platform's frame times) at the Mac's own grid gives the
// Mac's own frames: what makes a device's log comparable line by line.
static void testReplayAtGrid() {
  bench::ToneSpec s = basePluck(52, 1e-4, 3.0, -7.0);
  const std::vector<float> x = tone(s, 11);
  const std::vector<Frame> a = runDisplayPath(x, {48000.0, 16, 30.0, 440.0f});
  std::vector<size_t> at;
  for (const Frame& f : a) at.push_back(static_cast<size_t>(std::llround(f.tMs * 48.0)));
  const std::vector<Frame> b = runDisplayPathAt(x, {48000.0, 64, 30.0, 440.0f}, at);
  CHECK(framesCsv(a, "") == framesCsv(b, ""), "replay at the Mac's grid differs from the Mac's run");
}

// Truth: steady only once a 4 c glide has died down; the note flips when the truth crosses 50 c.
static void testTruthSeries() {
  bench::ToneSpec s = basePluck(40, 2e-4, 4.0, 49.0);
  const Case c = synthCase("t", "t", "E2", s);
  std::vector<double> t;
  for (double ms = 0; ms < 3800; ms += 100) t.push_back(ms);
  const std::vector<TruthRow> r = truthSeries(c, t);
  CHECK(r[2].segment == -1 && r[4].segment == 0, "segments: %d %d", r[2].segment, r[4].segment);
  CHECK(r[5].midi == 41 && r[37].midi == 40, "gliding sharp from +49 c crosses into F2: %d then %d", r[5].midi, r[37].midi);
  CHECK(!r[5].steady && r[37].steady, "steady only after the glide");
}

// ---------------------------------------------------------------- regressions (regression_cases.hpp) ----

struct RegRun {
  int live = 0, wrong = 0, belowE2 = 0;
  double firstCorrect = -1.0, maxStep = 0.0, glideErr = -1.0;
  std::vector<double> err;  // |error| of the live, right frames over 0.5-1.7 s
};

// One regression case through the display path: wrong notes after 100 ms, notes below E2, the
// first live frame on the right note, the largest step between consecutive live frames on one
// note from 0.5 s, and the median |error| over 0.5-1.7 s.
// One live frame `t` s after the onset; `prev` is the live frame before it, if any.
static void scoreRegressionFrame(const Frame& f, const Frame* prev, double t, double truth, RegRun& r) {
  const int midi = static_cast<int>(std::lround(69.0 + 12.0 * std::log2(truth / 440.0)));
  const bool right = f.midi == midi;
  ++r.live;
  r.belowE2 += f.midi < 40;
  r.wrong += t >= 0.1 && !right;
  if (right && r.firstCorrect < 0.0) r.firstCorrect = t;
  if (right && t >= 0.5 && t <= 1.7) r.err.push_back(std::fabs(cents(f.hz, truth)));
  if (prev && prev->midi == f.midi && t >= 0.5) r.maxStep = std::max(r.maxStep, static_cast<double>(std::fabs(f.shown - prev->shown)));
}

static RegRun runRegression(Case c) {
  c.load(c);
  const std::vector<Frame> fr = runDisplayPath(c.x, {c.fs, 64, 30.0, 440.0f});
  const Segment& g = c.segments[0];
  RegRun r;
  const Frame* prev = nullptr;
  for (const Frame& f : fr) {
    const double t = f.tMs / 1000.0 - g.onset;
    if (f.live) scoreRegressionFrame(f, prev, t, g.exact(t), r);
    prev = f.live ? &f : nullptr;
  }
  std::sort(r.err.begin(), r.err.end());
  if (!r.err.empty()) r.glideErr = r.err[r.err.size() / 2];
  return r;
}

// Each failure found on real guitars, as a synthetic case made to fail the way the real one did.
static void testRegressions() {
  const RegRun hum = runRegression(humTailCase());
  CHECK(hum.belowE2 == 0 && hum.live > 20, "hum-tail: %d of %d live frames showed the 56 Hz hum as A1..C2", hum.belowE2, hum.live);
  const RegRun quiet = runRegression(quietTailCase());
  CHECK(quiet.maxStep <= 1.5 && quiet.live > 60, "quiet-tail: the number leapt %.1f c (live %d)", quiet.maxStep, quiet.live);
  const RegRun fading = runRegression(fadingFundamentalCase());
  CHECK(fading.wrong == 0 && fading.live > 100, "fading-fundamental: %d of %d frames an octave up with no pluck", fading.wrong, fading.live);
  const RegRun glide = runRegression(slowGlideCase());
  CHECK(glide.glideErr >= 0.0 && glide.glideErr <= 1.75 && glide.maxStep <= 1.0, "slow-glide: median error %.2f c over 0.5-1.7 s, step %.1f c",
        glide.glideErr, glide.maxStep);
  const RegRun together = runRegression(stringsTogetherCase());
  CHECK(together.wrong == 0 && together.firstCorrect >= 0.0 && together.firstCorrect <= 3.0,
        "strings-together: %d wrong frames, the E first shown at %.2f s", together.wrong, together.firstCorrect);
  std::printf("regressions: hum-tail %d below E2; quiet-tail step %.1f c; fading-fundamental %d wrong; slow-glide %.2f c, step %.1f c; "
              "strings-together %d wrong, E at %.2f s\n", hum.belowE2, quiet.maxStep, fading.wrong, glide.glideErr, glide.maxStep, together.wrong,
              together.firstCorrect);
}

// Every display rounds as the page's formatCents: Math.round of 10 c, so halves go up, and -0.0 is 0.0.
static void testRoundingIsThePages() {
  using squatch::host::roundTenth;
  CHECK(roundTenth(-4.25) == -4.2f && roundTenth(4.25) == 4.3f && roundTenth(-4.75) == -4.7f, "halves go up: %.1f %.1f %.1f",
        static_cast<double>(roundTenth(-4.25)), static_cast<double>(roundTenth(4.25)), static_cast<double>(roundTenth(-4.75)));
  CHECK(roundTenth(-0.25) == -0.2f && roundTenth(-0.04f) == 0.0f && !std::signbit(roundTenth(-0.04f)), "-0.25 shows -0.2, -0.04 shows 0.0");
  CHECK(roundTenth(-0.05) == 0.0f, "a double -0.05 is 10 x -0.05 = -0.5 before the half is added (no fused multiply-add)");
  CHECK(roundTenth(static_cast<float>(-4.2503)) == -4.3f && roundTenth(-4.2497) == -4.2f, "full precision decides, not three decimals");
  DisplayModel m;
  double t = 0.0;
  for (int i = 0; i < 16; ++i, t += 33.3) m.update(reading(true, 40, i % 2 ? -4.0f : -4.5f), 0.1f, at(t), kRate);
  const Frame held = m.update(reading(false, 0, 0.0f), 0.0f, at(t), kRate);
  CHECK(std::fabs(held.shown - (-4.2f)) < 1e-6f, "a held median of -4.25 shows -4.2, got %.1f", static_cast<double>(held.shown));
}

static void testMidiReader() {
  const unsigned char smf[] = {'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, 0, 100, 'M', 'T', 'r', 'k', 0, 0, 0, 19,
                               0, 0xFF, 0x51, 3, 0x07, 0xA1, 0x20,  // 500000 us per quarter
                               0, 0x90, 40, 100, 100, 0x80, 40, 0, 0, 0xFF, 0x2F, 0};
  const std::string path = "build/test_quality.mid";
  FILE* f = std::fopen(path.c_str(), "wb");
  if (f) {
    std::fwrite(smf, 1, sizeof smf, f);
    std::fclose(f);
  }
  const std::vector<MidiNote> n = readMidi(path);
  CHECK(n.size() == 1 && n[0].pitch == 40 && std::fabs(n[0].end - 0.5) < 1e-9, "one E2 lasting 0.5 s (got %zu notes)", n.size());
}

int main() {
  testReferenceRecoversTruth();
  testContourFollowsGlide();
  testHoldAfterRelease();
  testHoldTooShort();
  testHoldExpiryOnAudioTime();
  testStaleReadingsNotHeld();
  testBlockSizesAgree();
  testReplayAtGrid();
  testTruthSeries();
  testMidiReader();
  testRegressions();
  testRoundingIsThePages();
  testPublicCases();
  std::printf(gFailures ? "%d failure(s)\n" : "all tests passed\n", gFailures);
  return gFailures ? 1 : 0;
}
