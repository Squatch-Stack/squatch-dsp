// SPDX-License-Identifier: MIT
// The live rig's host code that runs without JACK: the WAV reader (every supported encoding,
// written here and read back), resampling, response shaping, and the command line.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../common/ir_file.hpp"
#include "../jack/rig_options.hpp"

namespace {

int gChecks = 0, gFailures = 0;

void check(bool ok, const char* what) {
  ++gChecks;
  if (!ok) ++gFailures, std::printf("FAIL %s\n", what);
}

void put(std::vector<unsigned char>& b, std::uint32_t v, int bytes) {
  for (int i = 0; i < bytes; ++i) b.push_back(static_cast<unsigned char>(v >> (8 * i)));
}

struct Encoding {
  int tag, bits;
  bool extensible;
};

std::uint32_t encode(float x, const Encoding& e) {
  if (e.tag == 3) {
    std::uint32_t u;
    std::memcpy(&u, &x, sizeof u);
    return u;
  }
  const double full = std::ldexp(1.0, e.bits - 1);
  return static_cast<std::uint32_t>(static_cast<std::int64_t>(std::lround(x * full)));
}

// A WAV file of two channels: x on the first, -x on the second, with a chunk to skip.
std::string writeWav(const std::vector<float>& x, int rate, const Encoding& e) {
  const int bytes = e.bits / 8, channels = 2;
  std::vector<unsigned char> b;
  const auto fourcc = [&](const char* id) { b.insert(b.end(), id, id + 4); };
  fourcc("RIFF"), put(b, 0, 4), fourcc("WAVE");
  fourcc("LIST"), put(b, 3, 4), put(b, 0, 3), b.push_back(0);  // odd size: padded
  fourcc("fmt "), put(b, e.extensible ? 40 : 16, 4), put(b, e.extensible ? 0xFFFE : e.tag, 2), put(b, channels, 2);
  put(b, rate, 4), put(b, rate * channels * bytes, 4), put(b, channels * bytes, 2), put(b, e.bits, 2);
  if (e.extensible) put(b, 22, 2), put(b, e.bits, 2), put(b, 3, 4), put(b, e.tag, 2), b.insert(b.end(), 14, 0);
  fourcc("data"), put(b, static_cast<std::uint32_t>(x.size() * channels * bytes), 4);
  for (float v : x) put(b, encode(v, e), bytes), put(b, encode(-v, e), bytes);
  const char* dir = std::getenv("TMPDIR");
  const std::string path = std::string(dir && *dir ? dir : "/tmp") + "/squatch-test-" + std::to_string(e.tag) + "-" +
                           std::to_string(e.bits) + (e.extensible ? "x" : "") + ".wav";
  FILE* f = std::fopen(path.c_str(), "wb");
  std::fwrite(b.data(), 1, b.size(), f);
  std::fclose(f);
  return path;
}

void wavRoundTrips() {
  std::vector<float> x(300);
  for (std::size_t i = 0; i < x.size(); ++i) x[i] = 0.9f * std::sin(0.05f * float(i));
  const Encoding encodings[] = {{1, 16, false}, {1, 24, false}, {1, 32, false}, {3, 32, false}, {1, 24, true}, {3, 32, true}};
  for (const Encoding& e : encodings) {
    const std::string path = writeWav(x, 44100, e);
    const squatch::host::Wav w = squatch::host::readWav(path.c_str());
    std::remove(path.c_str());
    double worst = 0.0;
    for (std::size_t i = 0; i < x.size() && i < w.samples.size(); ++i) worst = std::fmax(worst, std::fabs(w.samples[i] - x[i]));
    const bool ok = w.error.empty() && w.rate == 44100.0 && w.samples.size() == x.size() && worst <= 1.0 / 32768.0;
    char what[96];
    std::snprintf(what, sizeof what, "wav tag %d, %d bits%s: first channel read back (error %.3g)", e.tag, e.bits,
                  e.extensible ? ", extensible" : "", worst);
    check(ok, what);
  }
  check(!squatch::host::readWav("/nonexistent/squatch.wav").error.empty(), "a missing file is an error");
}

void resamplingKeepsATone() {
  const double from = 44100.0, to = 48000.0, hz = 1000.0;
  std::vector<float> x(44100);
  for (std::size_t i = 0; i < x.size(); ++i) x[i] = static_cast<float>(0.5 * std::sin(2.0 * M_PI * hz * double(i) / from));
  const std::vector<float> y = squatch::host::resample(x, from, to);
  double worst = 0.0;
  for (std::size_t i = 2000; i + 2000 < y.size(); ++i)
    worst = std::fmax(worst, std::fabs(y[i] - 0.5 * std::sin(2.0 * M_PI * hz * double(i) / to)));
  check(y.size() == 48000 && worst < 1e-3, "44.1 to 48 kHz: a 1 kHz tone keeps its frequency and level");
}

void shapingTrimsAndNormalises() {
  std::vector<float> h(1000, 0.0f);
  for (int i = 0; i < 100; ++i) h[i] = 0.5f * std::exp(-0.05f * float(i));
  squatch::host::shapeResponse(h, -80.0, 4096);
  double energy = 0.0;
  for (float v : h) energy += double(v) * v;
  check(h.size() < 1000 && h.size() >= 100 && std::fabs(energy - 1.0) < 1e-6, "silent tail trimmed, unit energy");
  std::vector<float> g(1000, 0.1f);
  squatch::host::shapeResponse(g, -80.0, 256);
  check(g.size() == 256, "cut to the maximum length");
}

bool parse(std::vector<const char*> args, squatch::rig::Options& o) {
  args.insert(args.begin(), "squatch-rig-live");
  return squatch::rig::parseOptions(static_cast<int>(args.size()), const_cast<char**>(args.data()), o);
}

void optionsAreChecked() {
  squatch::rig::Options a, b, c, d;
  check(parse({}, a) && a.outs.size() == 1 && a.outs[0] == "system:playback_2", "the default output is playback_2");
  check(!parse({"--out", "system:playback_1"}, b), "playback_1 (the loopback cable) needs --loopback");
  check(parse({"--loopback", "--out", "system:playback_1", "--no-cab", "--no-limiter", "--in-db", "-6"}, c) && c.bypass[squatch::rig::Cab] &&
            c.bypass[squatch::rig::Limiter] && !c.bypass[squatch::rig::Gate] && c.inDb == -6.0f,
        "--loopback allows it; bypass flags and numbers are read");
  check(!parse({"--in-db", "99"}, d) && !parse({"--gate-db"}, d) && !parse({"--frobnicate"}, d), "bad values are refused");
}

}  // namespace

int main() {
  wavRoundTrips();
  resamplingKeepsATone();
  shapingTrimsAndNormalises();
  optionsAreChecked();
  std::printf("rig hosts: %d checks, %d failure(s)\n", gChecks, gFailures);
  return gFailures ? 1 : 0;
}
