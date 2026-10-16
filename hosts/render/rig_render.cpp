// SPDX-License-Identifier: MIT
// squatch-rig-render: the live rig's chain over a recording, on any machine, no JACK.
//
//   squatch-rig-render IN.wav OUT.wav [rig flags: --model M.nam --ir CAB.wav --in-db 6 ...]
//
// The same blocks, NAM adapter and response loader as squatch-rig-live, fed 64 samples at a
// time as JACK feeds them on the Pi: gate -> tuner -> amp -> cabinet -> EQ -> limiter. The
// output is 32-bit float WAV at the input's rate. Each stage's cost is printed per second of
// audio, so a render on this machine also says what the chain costs here. The flags are the
// live rig's (squatch-rig-live --help); --input, --out and --seconds have no meaning here.
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <squatch/dsp/blocks/cabinet.hpp>
#include <squatch/dsp/blocks/eq.hpp>
#include <squatch/dsp/blocks/gate.hpp>
#include <squatch/dsp/blocks/limiter.hpp>
#include <squatch/dsp/blocks/tuner.hpp>
#include <squatch/dsp/denormal.hpp>
#include <vector>

#include "../common/ir_file.hpp"
#include "../jack/nam_amp.hpp"
#include "../jack/rig_options.hpp"

namespace {

using squatch::dsp::Block;
using namespace squatch::rig;

constexpr int kBlock = 64;

struct Chain {
  squatch::dsp::Gate gate;
  extra::GateStage gateExtra;  // replaces the stock gate when an option selects it
  squatch::dsp::TunerBlock tuner;
#ifdef SQUATCH_WITH_NAM
  squatch::host::NamAmp amp;
#endif
  squatch::dsp::Cabinet cab;
  squatch::dsp::Eq eq;
  squatch::dsp::Limiter limiter;
  Block* stages[kStages] = {};
  double ns[kStages] = {};
  std::vector<float> response;
  std::unique_ptr<unsigned char[]> memory;
};

Chain gChain;  // static storage: the tuner alone is ~128 kB

bool loadAmp(Chain& c, const Options& o, double rate) {
#ifdef SQUATCH_WITH_NAM
  if (!o.model || o.bypass[Amp]) return false;
  const std::string err = c.amp.load(o.model, !o.noNormalize);
  if (!err.empty()) return std::fprintf(stderr, "amp: cannot load the model: %s\n", err.c_str()), false;
  const double want = c.amp.expectedRate();
  if (want > 0.0 && std::fabs(want - rate) > 0.5)
    return std::fprintf(stderr, "amp: the model is for %.0f Hz, the recording %.0f Hz; amp bypassed\n", want, rate), false;
  std::fprintf(stderr, "amp: NAM model loaded, loudness makeup %+.1f dB\n", c.amp.makeupDb());
  c.amp.set(squatch::host::NamAmp::Input, o.inDb);
  c.amp.set(squatch::host::NamAmp::Output, o.ampDb);
  return true;
#else
  (void)c, (void)rate;
  if (o.model) std::fprintf(stderr, "amp: built without NAM Core (make render NAM_CORE=...); amp bypassed\n");
  return false;
#endif
}

bool loadCab(Chain& c, const Options& o, double rate) {
  if (!o.ir || o.bypass[Cab]) return false;
  squatch::host::Response r = squatch::host::loadResponse(o.ir, rate, o.irMaxMs, !o.irNoResample);
  if (!r.error.empty()) return std::fprintf(stderr, "cab: %s; cabinet bypassed\n", r.error.c_str()), false;
  c.response = std::move(r.taps);
  c.cab.configure(c.response.data(), static_cast<int>(c.response.size()));
  c.cab.set(squatch::dsp::Cabinet::Level, o.cabDb);
  std::fprintf(stderr, "cab: %zu taps (%.1f ms)%s\n", c.response.size(), 1e3 * c.response.size() / rate,
               r.resampled ? ", resampled" : "");
  return true;
}

void place(Chain& c, const Options& o, bool amp, bool cab) {
  Block* all[kStages] = {&c.gate, &c.tuner, nullptr, &c.cab, &c.eq, &c.limiter};
  if (Block* g = c.gateExtra.block(o.extra)) all[Gate] = g;
#ifdef SQUATCH_WITH_NAM
  all[Amp] = &c.amp;
#endif
  const bool available[kStages] = {true, true, amp, cab, true, true};
  for (int s = 0; s < kStages; ++s) c.stages[s] = available[s] && !o.bypass[s] ? all[s] : nullptr;
  c.gate.set(squatch::dsp::Gate::Threshold, o.gateDb);
  c.gateExtra.configure(o.extra);
  c.eq.set(squatch::dsp::Eq::LowGain, o.eqLow);
  c.eq.set(squatch::dsp::Eq::MidGain, o.eqMid);
  c.eq.set(squatch::dsp::Eq::HighGain, o.eqHigh);
  c.limiter.set(squatch::dsp::Limiter::Ceiling, o.ceilingDb);
}

bool prepare(Chain& c, double rate) {
  const squatch::dsp::Setup setup{rate, kBlock};
  const auto run = [&](squatch::dsp::Arena& mem) {
    for (Block* b : c.stages)
      if (b) b->prepare(setup, mem);
  };
  squatch::dsp::Arena measure;
  run(measure);
  const std::size_t bytes = measure.required() + 64 * (kStages + 1) * squatch::dsp::Arena::kAlign;
  c.memory.reset(new unsigned char[bytes]);
  squatch::dsp::Arena mem(c.memory.get(), bytes);
  run(mem);
  return !mem.failed() && (!c.stages[Cab] || c.cab.ready());
}

void render(Chain& c, std::vector<float>& x) {
  squatch::dsp::DenormalGuard ftz;
  using Clock = std::chrono::steady_clock;
  for (std::size_t i = 0; i + kBlock <= x.size(); i += kBlock) {
    float* p = x.data() + i;
    for (int s = 0; s < kStages; ++s) {
      if (!c.stages[s]) continue;
      const auto t0 = Clock::now();
      c.stages[s]->process(p, p, kBlock);
      c.ns[s] += std::chrono::duration<double, std::nano>(Clock::now() - t0).count();
    }
  }
  x.resize(x.size() / kBlock * kBlock);
}

void put32(std::FILE* f, std::uint32_t v) { std::fwrite(&v, 4, 1, f); }
void put16(std::FILE* f, std::uint16_t v) { std::fwrite(&v, 2, 1, f); }

// Mono 32-bit float WAV (little-endian hosts: macOS and Linux on ARM and x86).
bool writeWav(const char* path, const std::vector<float>& x, double rate) {
  std::FILE* f = std::fopen(path, "wb");
  if (!f) return false;
  const auto bytes = static_cast<std::uint32_t>(x.size() * 4);
  std::fwrite("RIFF", 1, 4, f), put32(f, 36 + bytes), std::fwrite("WAVEfmt ", 1, 8, f);
  put32(f, 16), put16(f, 3), put16(f, 1), put32(f, static_cast<std::uint32_t>(rate));
  put32(f, static_cast<std::uint32_t>(rate) * 4), put16(f, 4), put16(f, 32);
  std::fwrite("data", 1, 4, f), put32(f, bytes);
  const bool ok = std::fwrite(x.data(), 4, x.size(), f) == x.size();
  return std::fclose(f) == 0 && ok;
}

void report(const Chain& c, std::size_t samples, double rate) {
  const double seconds = static_cast<double>(samples) / rate;
  std::fprintf(stderr, "cost per second of audio:");
  double total = 0.0;
  for (int s = 0; s < kStages; ++s) {
    if (!c.stages[s]) continue;
    std::fprintf(stderr, " %s %.2f ms", stageName(s), c.ns[s] / seconds / 1e6);
    total += c.ns[s];
  }
  std::fprintf(stderr, "; total %.2f ms (%.2f%% of real time)\n", total / seconds / 1e6, total / seconds / 1e7);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3 || argv[1][0] == '-') return std::fputs("usage: squatch-rig-render IN.wav OUT.wav [rig flags]\n", stderr), 64;
  const char* inPath = argv[1];
  const char* outPath = argv[2];
  Options o;
  argv[2] = argv[0];  // the rig's parser skips its first argument
  if (!parseOptions(argc - 2, argv + 2, o)) return printUsage(stderr), 64;
  squatch::host::Wav in = squatch::host::readWav(inPath);
  if (!in.error.empty()) return std::fprintf(stderr, "%s: %s\n", inPath, in.error.c_str()), 1;
  const bool amp = loadAmp(gChain, o, in.rate), cab = loadCab(gChain, o, in.rate);
  place(gChain, o, amp, cab);
  if (!prepare(gChain, in.rate)) return std::fputs("prepare failed\n", stderr), 1;
  if (gChain.stages[Gate] && !gChain.gateExtra.ready(o.extra, in.rate, kBlock)) return 1;
  render(gChain, in.samples);
  report(gChain, in.samples.size(), in.rate);
  if (!writeWav(outPath, in.samples, in.rate)) return std::fprintf(stderr, "%s: cannot write\n", outPath), 1;
  return 0;
}
