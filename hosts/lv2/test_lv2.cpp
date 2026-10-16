// SPDX-License-Identifier: MIT
// Smoke host for the LV2 bundles: loads each plugin's binary the way a host does (dlopen, then
// lv2_descriptor), with no LV2 library, and checks it end to end. The tuner's checks are here;
// every other plugin's are in its own test_<name>_lv2.cpp, which the Makefile compiles in when
// it is present and which registers itself with addPluginTest().
//   Tuner: a plucked E2 reads as E2 at 82.41 Hz within a few cents; audio passes; Mute mutes;
//   the generated port table agrees with the block's own parameter descriptor.
//
//   test_lv2 BUILD_DIR EXT        (make -C hosts/lv2 test)
#include <squatch/dsp/blocks/tuner.hpp>

#include "squatch-tuner_ports.hpp"
#include "test_lv2.hpp"

using namespace lv2test;

namespace {

std::vector<float> pluckedE2(std::size_t n) {
  std::vector<float> x(n);
  for (std::size_t i = 0; i < n; ++i) {
    const double t = static_cast<double>(i) / kRate, env = 0.4 * std::exp(-t / 3.0);
    for (int k = 1; k <= 4; ++k) x[i] += static_cast<float>(env / k * std::sin(2 * kPi * 82.4069 * k * t));
  }
  return x;
}

void tunerPortsMatchBlock() {
  namespace tu = squatch::lv2::tuner;
  const squatch::dsp::ParamDesc& mute = squatch::dsp::TunerBlock::params()[squatch::dsp::TunerBlock::Mute];
  check(mute.min == tu::kPorts[tu::kMute].min && mute.max == tu::kPorts[tu::kMute].max && mute.def == tu::kPorts[tu::kMute].def,
        "tuner: Mute port = TunerBlock::Mute", mute.def);
}

void tuner(const Loaded& l) {
  tunerPortsMatchBlock();
  namespace tu = squatch::lv2::tuner;
  const std::vector<float> def = defaults(tu::kPorts, tu::kPortCount);
  Instance in(l.d, def);
  check(in.h != nullptr, "tuner: instantiate", 0);
  if (!in.h) return;
  in.connect(tu::kAudioIn, tu::kAudioOut);
  l.d->activate(in.h);
  const std::size_t n = static_cast<std::size_t>(2.0 * kRate);
  std::vector<float> x = pluckedE2(n), y(n);
  in.run(x.data(), y.data(), n / 2, 128);
  in.run(x.data() + n / 2, y.data() + n / 2, n / 2, 1000);  // past the chunk
  const float* c = in.controls.data();
  check(c[tu::kValid] == 1.0f, "tuner: a note is read", c[tu::kValid]);
  check(c[tu::kNote] == 4.0f && c[tu::kOctave] == 2.0f, "tuner: named E2", c[tu::kNote]);
  check(std::fabs(c[tu::kPitch] - 40.0f) < 0.03f, "tuner: pitch 40.00 (E2) within 3 cents", c[tu::kPitch]);
  check(std::fabs(c[tu::kCents]) < 3.0f, "tuner: cents within 3", c[tu::kCents]);
  check(std::fabs(c[tu::kFreq] - 82.407f) < 0.15f, "tuner: frequency 82.41 Hz", c[tu::kFreq]);
  check(c[tu::kLevel] > -40.0f && c[tu::kLevel] < 0.0f, "tuner: level in dB", c[tu::kLevel]);
  check(std::memcmp(x.data(), y.data(), n * sizeof(float)) == 0, "tuner: audio passes untouched", 0);
  in.controls[tu::kMute] = 1.0f;
  in.run(x.data(), y.data(), 4800, 64);
  check(y[4799] == 0.0f, "tuner: Mute mutes", y[4799]);
}

const bool kRegistered = addPluginTest({"squatch-tuner", squatch::lv2::tuner::kUri, tuner});

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) return std::fputs("usage: test_lv2 BUILD_DIR EXT\n", stderr), 64;
  std::vector<PluginTest> tests = pluginTests();
  std::sort(tests.begin(), tests.end(), [](const PluginTest& a, const PluginTest& b) { return std::strcmp(a.bundle, b.bundle) < 0; });
  for (const PluginTest& t : tests) {
    const Loaded l = load(argv[1], t.bundle, argv[2]);
    check(l.d && std::strcmp(l.d->URI, t.uri) == 0, t.bundle, 0);
    if (l.d) t.run(l);
  }
  std::printf("lv2 smoke: %zu plugin(s), %d checks, %d failure(s)\n", tests.size(), gChecks, gFailures);
  return gFailures == 0 && !tests.empty() ? 0 : 1;
}
