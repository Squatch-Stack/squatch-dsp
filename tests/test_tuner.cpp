// SPDX-License-Identifier: MIT
// The tuner behind the Block interface. Its accuracy is tested in blocks/tuner (make test runs
// that suite too); here: the reading arrives through the snapshot, audio passes or mutes, and a
// golden render pins what the tuner makes of a fixed input, hop by hop.
#include <squatch/dsp/blocks/tuner.hpp>

#include "check.hpp"
#include "glitch.hpp"
#include "golden.hpp"
#include "signals.hpp"

using namespace squatch::dsp;

#if __has_include("golden/tuner.inc")
static const double kGolden[golden::kSegments] =
#include "golden/tuner.inc"
    ;
static const double* const kGoldenReading = kGolden;
#else
static const double* const kGoldenReading = nullptr;
#endif

namespace {

constexpr double kRate = 48000.0;

/// The tuner's own hop, and so the block size here: one reading published per hop.
constexpr int kHop = 64;

TunerBlock& prepared() {
  static TunerBlock t;  // ~128 kB: static, as a host keeps it
  Arena none;
  t.set(TunerBlock::Mute, 0.0f);
  t.prepare(Setup{kRate, kHop}, none);
  return t;
}

// The fixed, documented input of the golden render below: five cases at 48 kHz, 65536 samples in
// all, which is exactly 64 fingerprint segments of 16 hops each, so no segment straddles two
// cases and a segment's index says which case it belongs to. Three are exact equal-tempered
// pitches at A4 = 440, one is 25 cents flat of A2, and one is the same A2 under hiss.
struct Case {
  const char* name;
  double hz;      // the tone's exact frequency
  int samples;    // always a multiple of 1024, so the cases line up with the segment grid
  float amp;      // the tone's amplitude
  float hiss;     // hiss mixed in on top of it, 0 for the clean cases
  unsigned seed;  // sig::noise's seed, so the noisy case is the same signal on every run
};

const Case kCases[] = {
    {"A2 exactly", 110.0000, 12288, 0.5f, 0.0f, 0},
    {"E4 exactly", 329.6276, 12288, 0.5f, 0.0f, 0},
    {"E2 exactly", 82.4069, 8192, 0.5f, 0.0f, 0},
    {"A2 -25 cents", 108.4229, 16384, 0.5f, 0.0f, 0},  // 110 * 2^(-25/1200), to the sample shown
    {"A2 under hiss", 110.0000, 16384, 0.15f, 0.05f, 7},
};

/// One case: the tone, with hiss mixed in where the case asks for it.
std::vector<float> render(const Case& c) {
  std::vector<float> x = sig::sine(c.hz, c.samples, kRate, c.amp);
  if (c.hiss <= 0.0f) return x;
  const std::vector<float> h = sig::noise(c.samples, c.hiss, c.seed);
  for (int i = 0; i < c.samples; ++i) x[i] += h[i];
  return x;
}

/// What the tuner made of the fixed input: the fundamental it measured at every hop (zero until it
/// locks on), and the reading it settled on at the end of each case.
struct Measured {
  std::vector<float> hz;
  std::vector<squatch::tuner::TunerReading> settled;
};

/// Runs the five fixed cases back to back through one prepared tuner, a hop at a time, exactly as
/// a host would: so the note handovers between the cases are part of what the golden pins.
Measured measure(TunerBlock& t) {
  Measured m;
  std::vector<float> out;
  for (const Case& c : kCases) {
    const std::vector<float> x = render(c);
    out.assign(x.size(), 0.0f);
    for (std::size_t i = 0; i < x.size(); i += kHop) {
      t.process(&x[i], &out[i], kHop);
      CHECK(t.readings().update(), "%s: a reading published every hop", c.name);
      m.hz.push_back(t.readings().front().reading.hz);
    }
    m.settled.push_back(t.readings().front().reading);
  }
  return m;
}

/// Signed offset of `hz` from `ref`, in cents: the definition's own formula, written out here so
/// the expectations below do not lean on the code under test.
float centsFrom(double hz, double ref) { return static_cast<float>(1200.0 * std::log2(hz / ref)); }

}  // namespace

TEST(tunerBlockPublishesReadings) {
  TunerBlock& t = prepared();
  const double hz = 110.0 * std::exp2(13.0 / 1200.0);  // A2, 13 cents sharp
  const std::vector<float> x = sig::sine(hz, 28800, kRate, 0.3);
  std::vector<float> y(x.size());
  for (size_t i = 0; i < x.size(); i += 64) t.process(&x[i], &y[i], 64);
  CHECK(t.readings().update(), "a snapshot was published");
  const TunerSnapshot& s = t.readings().front();
  CHECK(s.samples == x.size(), "snapshot after %llu samples", static_cast<unsigned long long>(s.samples));
  CHECK(s.reading.valid && s.reading.midi == 45 && std::fabs(s.reading.cents - 13.0f) < 0.01f, "A2 %+.3f c (midi %d)",
        s.reading.cents, s.reading.midi);
  CHECK(!t.readings().update(), "nothing newer until the next process()");
  CHECK(y == x, "unmuted, the input passes through bit for bit");
}

TEST(tunerBlockMutesWithoutClicks) {
  TunerBlock& t = prepared();
  // 100 ms of the note, then the mute: the oracles are the note itself and silence.
  std::vector<float> x = sig::sine(196.0, 9600, kRate, 0.4);
  const std::vector<float> hiss = sig::noise(9600, 0.1f, 4);  // the oracle's high-frequency rule needs some
  for (std::size_t i = 0; i < x.size(); ++i) x[i] += hiss[i];
  const std::vector<float> silence(x.size(), 0.0f);
  std::vector<float> y(x.size());
  t.process(x.data(), y.data(), 4800);
  t.set(TunerBlock::Mute, 1.0f);
  t.process(x.data() + 4800, y.data() + 4800, 4800);
  glitch::Options opt;
    opt.dropoutMaxMs = 1e9;  // the muted half is meant to be black where the note still sounds in the oracle
  const glitch::Report r = glitch::analyse(y, x, silence, {{4800, 4800 + 240}}, opt);
  CHECK(r.ok && r.error.empty(), "a 5 ms ramp, not a click: %s%s", r.error.c_str(), r.findings.empty() ? "" : r.findings[0].c_str());
  CHECK(peakAbs(&y[4800 + 240], 4800 - 240) == 0.0f, "silent once the ramp is done");
  CHECK(t.readings().update() && t.readings().front().samples >= 9600, "still tuning while muted");
  // Negative control: the same mute without the ramp is found.
  const std::vector<float> tone = sig::sine(196.0, 9600, kRate, 0.5);  // pure, where a step towers over the signal's own curvature
  std::vector<float> cut = tone;
  for (std::size_t i = 4800; i < cut.size(); ++i) cut[i] = 0.0f;
  CHECK(glitch::analyse(cut, tone, silence, {{4800, 4800 + 240}}, opt).failed("continuity"), "an unramped mute is a continuity finding");
}

TEST(tunerGolden) {
  // A tuner's output is its reading, not a sound: unmuted, process() passes the audio through bit
  // for bit, so a golden render of the audio alone would pin nothing about the tuner. This one
  // fingerprints the fundamental measured at each of the 1024 hops of the input above: what a
  // display shows, and what any change to the decimator, the coarse note finder, the strobe bank
  // or the stiffness fit moves.
  //
  // tests/golden/tuner.inc was recorded once from this input and is only ever compared here, to
  // 1e-5 relative - the house tolerance, because FMA contraction and libm differ between the Mac,
  // the Pi and the M7. Nothing in this test writes it; that is `make golden`, and only after a
  // deliberate change of sound.
  TunerBlock& t = prepared();
  const Measured m = measure(t);
  CHECK(m.hz.size() == 1024, "%llu hops measured, one per 64 samples",
        static_cast<unsigned long long>(m.hz.size()));
  CHECK(m.hz[0] == 0.0f, "nothing measured before the coarse window has filled");

  // What each case must read, from the definition rather than from the golden: a stored render
  // says the output did not change, these say it was right.
  const float wantHz[] = {110.0f, 329.6276f, 82.4069f, 110.0f, 110.0f};
  const int wantMidi[] = {45, 64, 40, 45, 45};
  const float wantCents[] = {0.0f, 0.0f, 0.0f, -25.0f, 0.0f};
  const float centsTol[] = {0.01f, 0.01f, 0.01f, 0.05f, 0.5f};  // the noisy case jitters, then settles
  for (std::size_t i = 0; i < m.settled.size() && i < 5; ++i) {
    const squatch::tuner::TunerReading& r = m.settled[i];
    std::printf("     %-14s %10.4f Hz  midi %d  %+8.3f cents  uncertainty %.3f\n", kCases[i].name, r.hz, r.midi,
                r.cents, r.uncertainty);
    CHECK(r.valid, "%s: a reading, not withheld", kCases[i].name);
    CHECK(r.midi == wantMidi[i], "%s: midi %d, want %d", kCases[i].name, r.midi, wantMidi[i]);
    CHECK(std::fabs(centsFrom(r.hz, wantHz[i]) - wantCents[i]) <= centsTol[i], "%s: %+.3f cents from %.4f Hz, want %+.2f",
          kCases[i].name, centsFrom(r.hz, wantHz[i]), wantHz[i], wantCents[i]);
  }

  // The hiss case is only worth pinning if the hiss really does move the reading: for its first
  // 128 hops the fit chases the noise by several cents, and only after that does it settle.
  const std::size_t hissFrom = 768;  // 192 + 192 + 128 + 256 hops: the start of the last case
  float wildest = 0.0f;
  for (std::size_t h = hissFrom; h < hissFrom + 128; ++h) wildest = std::fmax(wildest, std::fabs(centsFrom(m.hz[h], 110.0)));
  CHECK(wildest > 5.0f, "the hiss moves the reading (worst %+.2f cents in its first 128 hops)", wildest);
  std::printf("     tuner hiss: worst %+.2f cents while the fit settles\n", wildest);

  golden::check("tuner", m.hz, kGoldenReading, 1e-5);
}