// SPDX-License-Identifier: MIT
// parity-block: run ONE squatch-dsp block over a raw float32 mono file, for tools/parity.
//
//   parity-block TYPEID IN.f32 OUT.f32 [--rate 48000] [--block 128] [--tail SECONDS]
//                [--ir IR.f32] [--nam MODEL.nam] [--nam-normalize 0|1] key=value ...
//
// TYPEID is a registered block ("cc.squatch.gate") or "nam" (the rig's NamAmp adapter, only when
// built with SQUATCH_WITH_NAM). key=value sets a contract parameter by its key, before the audio.
// The block is fed --block samples at a time (the browser's render quantum is 128), after half a
// second of silence so every parameter smoother has arrived. Output is IN's length plus --tail
// seconds of silence (reverb tails). One JSON line on stdout: the latency the block reports.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <squatch/dsp/registry.hpp>
#include <string>
#include <vector>

#ifdef SQUATCH_WITH_NAM
#include "../../hosts/jack/nam_amp.hpp"
#endif

using namespace squatch::dsp;

static std::vector<float> readF32(const char* path) {
  std::vector<float> v;
  FILE* f = std::fopen(path, "rb");
  if (!f) return v;
  float buf[4096];
  for (std::size_t n; (n = std::fread(buf, sizeof(float), 4096, f)) > 0;) v.insert(v.end(), buf, buf + n);
  std::fclose(f);
  return v;
}

struct Options {
  std::string type, inPath, outPath;
  double rate = 48000.0, tail = 0.0;
  int block = 128;
  const char *irPath = nullptr, *namPath = nullptr;
  bool namNormalize = true;
  std::vector<std::pair<std::string, float>> params;
};

static bool parseArgs(int argc, char** argv, Options& o) {
  o.type = argv[1], o.inPath = argv[2], o.outPath = argv[3];
  for (int i = 4; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&]() { return i + 1 < argc ? argv[++i] : ""; };
    if (a == "--rate") o.rate = std::atof(next());
    else if (a == "--block") o.block = std::atoi(next());
    else if (a == "--tail") o.tail = std::atof(next());
    else if (a == "--ir") o.irPath = next();
    else if (a == "--nam") o.namPath = next();
    else if (a == "--nam-normalize") o.namNormalize = std::atoi(next()) != 0;
    else if (a.find('=') != std::string::npos)
      o.params.emplace_back(a.substr(0, a.find('=')), std::strtof(a.c_str() + a.find('=') + 1, nullptr));
    else return std::fprintf(stderr, "unknown argument %s\n", a.c_str()), false;
  }
  return true;
}

#ifdef SQUATCH_WITH_NAM
static squatch::host::NamAmp gNam;
#endif

// The block named by the options: the NAM adapter, or a registered block built in `storage`.
static Block* makeBlock(const Options& o, void* storage, std::size_t bytes, std::vector<float>& irTaps) {
  Block* b = nullptr;
#ifdef SQUATCH_WITH_NAM
  if (o.type == "nam") {
    if (!o.namPath) return std::fprintf(stderr, "nam needs --nam MODEL\n"), nullptr;
    const std::string err = gNam.load(o.namPath, o.namNormalize);
    if (!err.empty()) return std::fprintf(stderr, "nam: %s\n", err.c_str()), nullptr;
    b = &gNam;
  }
#endif
  if (!b) b = createBlock(o.type.c_str(), storage, bytes);
  if (!b) return std::fprintf(stderr, "no block %s\n", o.type.c_str()), nullptr;
  if (o.type == "cc.squatch.cabinet") {
    if (!o.irPath) return std::fprintf(stderr, "cabinet needs --ir\n"), nullptr;
    irTaps = readF32(o.irPath);
    static_cast<Cabinet*>(b)->configure(irTaps.data(), static_cast<int>(irTaps.size()));
  }
  return b;
}

static bool applyParams(Block* b, const Options& o) {
  for (const auto& [key, value] : o.params) {
    if (o.type == "nam") {
      b->set(key == "input" ? 0 : 1, value);
      continue;
    }
    const ParamDesc* d = findKey(b->parameters(), key.c_str());
    if (!d) return std::fprintf(stderr, "%s has no parameter %s\n", o.type.c_str(), key.c_str()), false;
    b->set(d->id, value);
  }
  return true;
}

// Half a second of silence so every smoother has arrived, then the input and the tail.
static std::vector<float> run(Block* b, const Options& o, std::vector<float> in) {
  const std::size_t total = in.size() + static_cast<std::size_t>(o.tail * o.rate);
  in.resize(total, 0.0f);
  const std::size_t blk = static_cast<std::size_t>(o.block);
  std::vector<float> scratch(blk), zero(blk, 0.0f), out(total);
  for (int i = 0; i < static_cast<int>(0.5 * o.rate) / o.block; ++i) b->process(zero.data(), scratch.data(), o.block);
  for (std::size_t p = 0; p < total; p += blk) {
    const int n = static_cast<int>(std::min(blk, total - p));
    b->process(&in[p], &out[p], n);
  }
  return out;
}

int main(int argc, char** argv) {
  Options o;
  if (argc < 4) return std::fprintf(stderr, "usage: parity-block TYPEID IN.f32 OUT.f32 [opts] key=value...\n"), 2;
  if (!parseArgs(argc, argv, o)) return 2;
  std::vector<float> in = readF32(o.inPath.c_str());
  if (in.empty()) return std::fprintf(stderr, "cannot read %s\n", o.inPath.c_str()), 2;

  std::vector<unsigned char> mem(32u << 20);
  Arena arena(mem.data(), mem.size());
  alignas(kBlockAlign) static unsigned char at[kBlockSize];
  std::vector<float> irTaps;
  Block* b = makeBlock(o, at, sizeof at, irTaps);
  if (!b) return 2;
  Setup s;
  s.sampleRate = o.rate;
  s.maxBlock = o.block;
  b->prepare(s, arena);
  if (arena.failed()) return std::fprintf(stderr, "arena too small\n"), 1;
  if (!applyParams(b, o)) return 2;

  const std::vector<float> out = run(b, o, std::move(in));
  FILE* f = std::fopen(o.outPath.c_str(), "wb");
  if (!f) return std::fprintf(stderr, "cannot write %s\n", o.outPath.c_str()), 2;
  std::fwrite(out.data(), sizeof(float), out.size(), f);
  std::fclose(f);
  std::printf("{\"type\":\"%s\",\"latency\":%d}\n", o.type.c_str(), b->latency());
  if (o.type != "nam") destroyBlock(b);
  return 0;
}
