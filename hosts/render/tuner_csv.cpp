// SPDX-License-Identifier: MIT
// squatch-tuner-csv: the Squatch Tuner over a recording, one CSV row per display frame (30 a
// second of audio time), on any machine. The reference every other host's rows are diffed against.
//
//   squatch-tuner-csv IN.wav [--a4 HZ] [--block N] [--quantize 16] [--at FILE] [--hash 1]
//                            [--dump-f32 F] [--dump-s16 F]
//
//   --a4 HZ        reference pitch, 430..450 (default 440)
//   --block N      samples per process() call, 1..4096 (default 64); the rows do not depend on it
//   --quantize 16  round the input to 16 bits first, as USB audio delivers it to the S3 probe
//   --at FILE      take the frames at the times in FILE's first column (a CSV with a header, t_ms),
//                  rounded to samples, instead of 30 a second: e.g. where the phone read its display
//                  (times past the end of the recording give no row)
//   --hash 1       print the frames' bit-level hash to stderr (the probe's "#end ... hash=")
//   --dump-f32 F   also write the exact samples the tuner saw, float32 little-endian (the probe's
//   --dump-s16 F   "feed f32" input), or as int16 (its "feed s16" and USB audio input; implies
//                  --quantize 16)
//
// The first channel is used. Rows: hosts/common/tuner_frames.hpp.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <vector>

#include <squatch/dsp/arena.hpp>
#include <squatch/dsp/blocks/tuner.hpp>

#include "../common/ir_file.hpp"
#include "../common/tuner_frames.hpp"

namespace {

using squatch::host::FrameRow;

struct Options {
  const char* in = nullptr;
  float a4 = 440.0f;
  int block = 64;
  bool quantize = false;
  bool hash = false;
  const char* dumpF32 = nullptr;
  const char* dumpS16 = nullptr;
  const char* at = nullptr;
};

bool applyFileOption(std::string_view key, const char* v, Options& o) {
  if (key == "--at") return (o.at = v) != nullptr;
  if (key == "--dump-f32") return (o.dumpF32 = v) != nullptr;
  if (key == "--dump-s16") return (o.quantize = (o.dumpS16 = v) != nullptr);
  return false;
}

bool applyOption(std::string_view key, const char* v, Options& o) {
  if (key == "--a4") return (o.a4 = std::strtof(v, nullptr)) >= 430.0f && o.a4 <= 450.0f;
  if (key == "--block") return (o.block = std::atoi(v)) >= 1 && o.block <= 4096;
  if (key == "--hash") return (o.hash = std::atoi(v) == 1) || std::atoi(v) == 0;
  if (key == "--quantize") return (o.quantize = std::atoi(v) == 16);
  return applyFileOption(key, v, o);
}

bool parseArgs(int argc, char** argv, Options& o) {
  if (argc < 2 || argv[1][0] == '-') return false;
  o.in = argv[1];
  for (int i = 2; i < argc; i += 2)
    if (i + 1 >= argc || !applyOption(argv[i], argv[i + 1], o)) return false;
  return true;
}

std::int16_t toS16(float x) {
  return static_cast<std::int16_t>(std::clamp(std::lround(static_cast<double>(x) * 32768.0), -32768L, 32767L));
}

void quantize(std::vector<float>& x, std::vector<std::int16_t>& q) {
  q.resize(x.size());
  for (std::size_t i = 0; i < x.size(); ++i) {
    q[i] = toS16(x[i]);
    x[i] = static_cast<float>(q[i]) / 32768.0f;  // exactly what the probe makes of the int16
  }
}

bool dump(const char* path, const void* data, std::size_t bytes) {
  FILE* f = std::fopen(path, "wb");
  if (!f) return false;
  const bool ok = std::fwrite(data, 1, bytes, f) == bytes;
  return std::fclose(f) == 0 && ok;
}

// The frame sink: the tuner as a Block, its readings through the Block's handoff.
struct Sink {
  squatch::dsp::TunerBlock& block;
  std::vector<float> out;
  std::uint32_t hash = squatch::host::kFrameHashSeed;
  void process(const float* p, int n) { block.process(p, out.data(), n); }
  squatch::tuner::TunerReading reading() {
    block.readings().update();
    return block.readings().front().reading;
  }
  void emit(const FrameRow& row) {
    char text[96];
    squatch::host::formatFrameCsv(text, sizeof text, row);
    hash = squatch::host::hashFrame(hash, row);
    std::puts(text);
  }
};

squatch::dsp::TunerBlock gBlock;  // ~128 kB: static storage

// The first column of a CSV with a header, as sample positions at `rate`.
bool readPositions(const char* path, double rate, std::vector<std::uint64_t>& at) {
  FILE* f = std::fopen(path, "r");
  if (!f) return false;
  char line[512];
  for (bool header = true; std::fgets(line, sizeof line, f); header = false) {
    const double ms = std::strtod(line, nullptr);
    if (!header && ms >= 0.0) at.push_back(static_cast<std::uint64_t>(std::llround(ms * rate / 1000.0)));
  }
  std::fclose(f);
  return std::is_sorted(at.begin(), at.end());
}

void render(const std::vector<float>& x, std::uint32_t rate, const Options& o, const std::vector<std::uint64_t>& at) {
  squatch::dsp::Arena none;
  gBlock.prepare({static_cast<double>(rate), o.block, squatch::dsp::Layout::Mono}, none);
  squatch::host::TunerFrames frames;
  frames.reset(rate, o.a4);
  if (o.at) frames.schedule(at.data(), at.size());
  Sink sink{gBlock, std::vector<float>(static_cast<std::size_t>(o.block))};
  std::puts(squatch::host::kFrameCsvHeader);
  for (std::size_t from = 0; from < x.size(); from += static_cast<std::size_t>(o.block)) {
    const std::size_t n = std::min<std::size_t>(static_cast<std::size_t>(o.block), x.size() - from);
    squatch::host::feedFrames(frames, x.data() + from, n, sink);
  }
  if (o.hash) std::fprintf(stderr, "frames=%llu hash=%08x\n", static_cast<unsigned long long>(frames.frames()), unsigned(sink.hash));
}

// Quantises when asked and writes the dumps: the exact samples the tuner will see.
bool prepareInput(const Options& o, std::vector<float>& x) {
  std::vector<std::int16_t> q;
  if (o.quantize) quantize(x, q);
  if ((o.dumpF32 && !dump(o.dumpF32, x.data(), x.size() * sizeof(float))) ||
      (o.dumpS16 && !dump(o.dumpS16, q.data(), q.size() * sizeof(std::int16_t)))) {
    std::fprintf(stderr, "cannot write the dump\n");
    return false;
  }
  return true;
}

// Reads the recording and the optional schedule, quantises, writes the dumps; false after an error message.
bool load(const Options& o, squatch::host::Wav& w, std::vector<std::uint64_t>& at) {
  w = squatch::host::readWav(o.in);
  if (!w.error.empty() || w.rate != std::floor(w.rate)) {
    std::fprintf(stderr, "%s: %s\n", o.in, w.error.empty() ? "fractional sample rate" : w.error.c_str());
    return false;
  }
  if (o.at && !readPositions(o.at, w.rate, at)) {
    std::fprintf(stderr, "%s: cannot read ascending times from its first column\n", o.at);
    return false;
  }
  return prepareInput(o, w.samples);
}

}  // namespace

int main(int argc, char** argv) {
  Options o;
  if (!parseArgs(argc, argv, o)) {
    std::fprintf(stderr, "usage: squatch-tuner-csv IN.wav [--a4 HZ] [--block N] [--quantize 16] [--at FILE] "
                         "[--hash 1] [--dump-f32 FILE] [--dump-s16 FILE]\n");
    return 64;
  }
  squatch::host::Wav w;
  std::vector<std::uint64_t> at;
  if (!load(o, w, at)) return 1;
  render(w.samples, static_cast<std::uint32_t>(w.rate), o, at);
  return 0;
}
