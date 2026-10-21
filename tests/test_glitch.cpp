// SPDX-License-Identifier: MIT
// The glitch oracle (tests/glitch.hpp) tested on its own: each rule passes a clean fade and fails
// the defect it exists for, so a test that uses the oracle is judging with something that can fail.
#include "check.hpp"
#include "glitch.hpp"
#include "signals.hpp"

namespace {

constexpr double kRate = 48000.0;
constexpr int kN = 9600;
constexpr std::size_t kAt = 4000, kLen = 240;

std::vector<float> tone(double hz, double amp) { return sig::sine(hz, kN, kRate, amp); }

// before -> after over [kAt, kAt + kLen) with `w` the weight of `after` at each step.
std::vector<float> blend(const std::vector<float>& a, const std::vector<float>& b, double (*w)(double)) {
  std::vector<float> y(a.size());
  for (std::size_t i = 0; i < y.size(); ++i) {
    const double t = i < kAt ? 0.0 : i >= kAt + kLen ? 1.0 : double(i - kAt) / double(kLen);
    y[i] = float((1.0 - w(t)) * a[i] + w(t) * b[i]);
  }
  return y;
}

double smooth(double t) { return 0.5 - 0.5 * std::cos(3.14159265358979323846 * t); }
double hard(double t) { return t < 0.5 ? 0.0 : 1.0; }

const std::vector<glitch::Edit> kEdit = {{kAt, kAt + kLen}};

}  // namespace

TEST(glitchOraclePassesACleanFade) {
  const std::vector<float> a = tone(220.0, 0.5), b = tone(330.0, 0.4);
  const glitch::Report r = glitch::analyse(blend(a, b, smooth), a, b, kEdit);
  CHECK(r.ok && r.error.empty() && r.continuityEvaluated, "a raised-cosine fade is clean: %s", r.findings.empty() ? "" : r.findings[0].c_str());
}

TEST(glitchOracleCatchesAHardSwitch) {
  const std::vector<float> a = tone(220.0, 0.5), b = tone(330.0, 0.4);
  const glitch::Report r = glitch::analyse(blend(a, b, hard), a, b, kEdit);
  CHECK(!r.ok && r.failed("continuity"), "the step is a continuity finding");
}

TEST(glitchOracleCatchesADropout) {
  const std::vector<float> a = tone(220.0, 0.5), b = tone(330.0, 0.4);
  std::vector<float> y = blend(a, b, smooth);
  for (std::size_t i = kAt + 40; i < kAt + 140; ++i) y[i] = 0.0f;  // 2 ms of black in the middle of the fade
  const glitch::Report r = glitch::analyse(y, a, b, kEdit);
  CHECK(r.failed("dropout") && r.maxDropoutMs > 1.9, "dropout %.2f ms", r.maxDropoutMs);
}

TEST(glitchOracleCatchesAHighFrequencyBurst) {
  const std::vector<float> a = tone(220.0, 0.5), b = tone(330.0, 0.4);
  std::vector<float> y = blend(a, b, smooth);
  const std::vector<float> buzz = sig::sine(9000.0, kN, kRate, 0.05);  // continuous in value, energy above 5 kHz
  for (std::size_t i = kAt + 60; i < kAt + 180; ++i) y[i] += buzz[i] * float(std::sin(3.14159265358979 * double(i - kAt - 60) / 120.0));
  const glitch::Report r = glitch::analyse(y, a, b, kEdit);
  CHECK(r.failed("hf") && r.maxHfExcessDb > 3.0, "hf excess %.1f dB", r.maxHfExcessDb);
}

TEST(glitchOracleCatchesASlideThatNeverArrives) {
  const std::vector<float> a = tone(220.0, 0.5), b = tone(330.0, 0.4);
  std::vector<float> y(a.size());
  for (std::size_t i = 0; i < y.size(); ++i) y[i] = float(0.5 * a[i] + 0.5 * b[i]);  // smooth, steady, and neither chain
  const glitch::Report r = glitch::analyse(y, a, b, kEdit);
  CHECK(r.failed("convergence") && r.worstConvergenceDb > -60.0, "convergence %.1f dBFS", r.worstConvergenceDb);
}

TEST(glitchOracleRefusesSpansItCannotJudge) {
  const std::vector<float> a = tone(220.0, 0.5), b = tone(330.0, 0.4), y = blend(a, b, smooth);
  CHECK(!glitch::analyse(y, a, b, {{50, 290}}).ok && !glitch::analyse(y, a, b, {{50, 290}}).error.empty(), "an edit at the edge: an error, not a pass");
  CHECK(!glitch::analyse(y, a, b, {{kAt, kAt + 10}, {kAt + 5, kAt + 20}}).ok, "overlapping edits");
  CHECK(!glitch::analyse(y, std::vector<float>(10), b, kEdit).ok, "mismatched lengths");
  const std::vector<float> z(kN, 0.0f);
  const glitch::Report silent = glitch::analyse(z, z, z, kEdit);
  CHECK(!silent.continuityEvaluated, "silence is unmeasurable, and says so");
}

// Material like a guitar's: a bass note with a pick attack in it. The attack is far rougher than the
// note around it, so a reference taken from the roughest point of the span (rule 1) is blind to a
// step in the smooth part, which is where a player's switch lands.
namespace {

std::vector<float> pluck(double gain) {
  std::vector<float> x = tone(110.0, 0.3 * gain);
  const std::vector<float> ring = sig::sine(3100.0, kN, kRate, 0.6 * gain);
  for (std::size_t i = 1400; i < 2200; ++i) x[i] += ring[i] * float(std::exp(-double(i - 1400) / 90.0));
  return x;
}

// before -> after over `len` frames from kAt, raised cosine.
std::vector<float> fadeOver(const std::vector<float>& a, const std::vector<float>& b, std::size_t len) {
  std::vector<float> y(a.size());
  for (std::size_t i = 0; i < y.size(); ++i) {
    const double t = i < kAt ? 0.0 : i >= kAt + len ? 1.0 : double(i - kAt) / double(len);
    y[i] = float((1.0 - smooth(t)) * a[i] + smooth(t) * b[i]);
  }
  return y;
}

}  // namespace

TEST(glitchOracleLocalRuleCatchesAStepNextToALoudAttack) {
  const std::vector<float> a = pluck(1.0), b = pluck(0.4);
  const glitch::Report step = glitch::analyse(fadeOver(a, b, 1), a, b, kEdit);
  // The negative control: the old global reference does not see this step. If it ever does, this test no longer shows what the local rule is for.
  CHECK(!step.failed("continuity"), "rule 1 is blind to the step (second difference %.4g, limit %.4g)", step.maxEditD2, 2.0 * step.referenceD2);
  CHECK(step.failed("local") && step.localRatio > 20.0, "the local rule catches it: %.1f times the limit", step.localRatio);
  CHECK(step.localFrame >= kAt - 2 && step.localFrame <= kAt + 3, "and finds it at the step, frame %zu", step.localFrame);
}

TEST(glitchOracleLocalRulePassesAnyFadeThatIsNotAClick) {
  const std::vector<float> a = pluck(1.0), b = pluck(0.4);
  for (std::size_t len : {std::size_t(240), std::size_t(480), std::size_t(48)}) {  // 5 ms, 10 ms, and 1 ms
    const glitch::Report r = glitch::analyse(fadeOver(a, b, len), a, b, {{kAt, kAt + len}});
    CHECK(r.ok && r.localRatio < 1.0, "a %zu-frame fade passes: local %.3g %s", len, r.localRatio, r.findings.empty() ? "" : r.findings[0].c_str());
  }
  const glitch::Report quick = glitch::analyse(fadeOver(a, b, 4), a, b, {{kAt, kAt + 4}});  // 0.08 ms: a click in all but name
  CHECK(quick.failed("local"), "a 0.08 ms fade is too fast: local %.3g", quick.localRatio);
}

TEST(glitchOracleLocalRuleLeavesRoughOraclesAlone) {
  // A switch inside the attack itself is masked by it: the same step lands in the oracles' own roughness and is not a finding.
  const std::vector<float> a = pluck(1.0), b = pluck(0.9);
  const glitch::Report r = glitch::analyse(fadeOver(a, b, 1), a, b, {{kAt, kAt + 1}});
  const std::vector<float> c = pluck(1.0), d = pluck(0.4);
  const glitch::Report loud = glitch::analyse(fadeOver(c, d, 1), c, d, kEdit);
  CHECK(r.localRatio < loud.localRatio, "a step of a tenth of the difference rates lower: %.2f against %.2f", r.localRatio, loud.localRatio);
}

TEST(glitchOracleHfIgnoresNoiseFloorDifferences) {
  const std::vector<float> a = tone(220.0, 0.5), b = tone(330.0, 0.4);
  std::vector<float> y = blend(a, b, smooth);
  const std::vector<float> hiss = sig::sine(9000.0, kN, kRate, 2e-5);  // -94 dBFS, over 90 dB below the signal
  for (std::size_t i = kAt; i < kAt + 240; ++i) y[i] += hiss[i];
  const glitch::Report quiet = glitch::analyse(y, a, b, kEdit);
  CHECK(!quiet.failed("hf"), "a noise-floor difference is not a transition (%.1f dB)", quiet.maxHfExcessDb);
  glitch::Options strict;
  strict.hfFloorDb = -200.0;  // negative control: with no floor the same signal does fail
  CHECK(glitch::analyse(y, a, b, kEdit, strict).failed("hf"), "without the floor it is a finding, so the floor is what passes it");
}

TEST(glitchOracleDropoutLetsAGateClose) {
  // A gate that closes leaves the chain after the edit silent. The chain before it was audible, and that
  // is not a reason to call the silence a dropout.
  const std::vector<float> a = tone(220.0, 0.5), b(kN, 0.0f);
  const glitch::Report closed = glitch::analyse(blend(a, b, smooth), a, b, kEdit);
  CHECK(!closed.failed("dropout") && closed.ok, "fading to a closed gate is clean: %s", closed.findings.empty() ? "" : closed.findings[0].c_str());
  // Negative control: silence BEFORE the edit, where the chain is still audible, is a dropout.
  std::vector<float> y = blend(a, b, smooth);
  for (std::size_t i = 1000; i < 1100; ++i) y[i] = 0.0f;
  CHECK(glitch::analyse(y, a, b, kEdit).failed("dropout"), "a hole in the audible chain before the edit is still found");
  // And silence after the edit where the chain after it is audible (a gate that opens but the output stays shut).
  const std::vector<float> opened = blend(b, a, smooth);
  std::vector<float> stuck = opened;
  for (std::size_t i = kAt + 400; i < kAt + 700; ++i) stuck[i] = 0.0f;
  CHECK(glitch::analyse(stuck, b, a, kEdit).failed("dropout"), "a gate that never opens is found");
}

// Rule 6. Correlated chains (the same signal at two gains, as a cabinet switched on or off is to a first approximation) crossfaded with
// an equal-power law add in phase and swell by up to 3 dB; a linear fade keeps the level between the two.
namespace {
std::vector<float> lawFade(const std::vector<float>& a, const std::vector<float>& b, bool equalPower) {
  std::vector<float> y(a.size());
  for (std::size_t i = 0; i < y.size(); ++i) {
    const double t = i < kAt ? 0.0 : i >= kAt + kLen ? 1.0 : double(i - kAt) / double(kLen);
    const double wa = equalPower ? std::cos(t * 1.5707963267948966) : 1.0 - t, wb = equalPower ? std::sin(t * 1.5707963267948966) : t;
    y[i] = float(wa * a[i] + wb * b[i]);
  }
  return y;
}
}  // namespace

TEST(glitchOracleFindsALevelSwellDuringAFade) {
  const std::vector<float> a = tone(220.0, 0.5), b = tone(220.0, 0.5);
  const glitch::Report swell = glitch::analyse(lawFade(a, b, true), a, b, kEdit);
  CHECK(swell.failed("level") && swell.maxSwellDb > 1.8, "an equal-power fade on correlated material swells %.1f dB and is found", swell.maxSwellDb);
  const glitch::Report linear = glitch::analyse(lawFade(a, b, false), a, b, kEdit);
  CHECK(linear.ok && !linear.failed("level") && linear.maxSwellDb < 0.5, "a linear fade on the same material does not swell (%.2f dB)", linear.maxSwellDb);
  const std::vector<float> c = tone(330.0, 0.4);  // uncorrelated: the linear fade dips, which is measured, not judged
  const glitch::Report dip = glitch::analyse(lawFade(a, c, false), a, c, kEdit);
  CHECK(dip.maxDipDb > 1.5 && !dip.failed("level"), "the dip of a linear fade on uncorrelated tones is reported (%.1f dB)", dip.maxDipDb);
}

TEST(glitchOracleDoesNotPassASpanItCouldNotJudge) {
  const std::vector<float> z(kN, 0.0f);
  const glitch::Report silent = glitch::analyse(z, z, z, kEdit);
  CHECK(!silent.ok && !silent.judged() && !silent.error.empty(), "a span on which no rule ran is not ok");
  const std::vector<float> a = tone(220.0, 0.5), b = tone(330.0, 0.4);
  const glitch::Report r = glitch::analyse(blend(a, b, smooth), a, b, kEdit);
  CHECK(r.ok && r.judged() && r.hfEvaluated == false, "tones with nothing above 5 kHz leave the hf rule unapplied, and say so, while others judged it");
}

TEST(glitchOracleContinuityReferenceIsLocalToEachEdit) {
  // A loud pick attack far from the edit must not loosen rule 1 for it.
  const std::vector<float> a = pluck(1.0), b = pluck(0.4);
  glitch::Options near;
  near.continuityReachMs = 20.0;  // the attack is 40 ms away
  const glitch::Report step = glitch::analyse(fadeOver(a, b, 1), a, b, kEdit, near);
  glitch::Options wide;
  wide.continuityReachMs = 1000.0;  // the old behaviour, near enough: the attack is in reach
  const glitch::Report old = glitch::analyse(fadeOver(a, b, 1), a, b, kEdit, wide);
  CHECK(!old.failed("continuity"), "with the attack in reach rule 1 is blind to the step");
  CHECK(step.failed("continuity"), "with the reference taken near the edit, rule 1 finds it");
}
