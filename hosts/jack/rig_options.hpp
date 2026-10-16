// SPDX-License-Identifier: MIT
// Command line of squatch-rig-live. Files are named at run time only (flags or environment):
// models and impulse responses are never part of this repository.
#pragma once
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "rig_stages.hpp"

// Stages that exist only in the development tree plug in through rig_extra.hpp, which is never
// exported; without it these stand-ins add nothing.
#if __has_include("rig_extra.hpp")
#include "rig_extra.hpp"
#else
#include <squatch/dsp/block.hpp>
#include <squatch/dsp/param.hpp>
namespace squatch::rig::extra {
struct Options {};
constexpr const char* kUsage = "";
inline bool toggle(std::string_view, Options&) { return false; }
inline bool named(std::string_view, const char*, Options&) { return false; }
struct GateStage {
  squatch::dsp::Block* block(const Options&) { return nullptr; }
  static squatch::dsp::Span<const squatch::dsp::ParamDesc> params() { return {}; }
  void configure(const Options&) {}
  bool ready(const Options&, double, int) { return true; }
};
}  // namespace squatch::rig::extra
#endif

namespace squatch::rig {

constexpr const char* kUsage =
    "usage: squatch-rig-live [options]\n"
    "  --input N          read system:capture_N (default 1: the UMC204HD's Input 1)\n"
    "  --out PORT         connect the output here; repeat for more (default system:playback_2;\n"
    "                     'none' connects it nowhere, for another client to take)\n"
    "  --loopback         allow system:playback_1, which is cabled back into Input 2 on the bench\n"
    "  --model PATH       NAM model (.nam) for the amp (or $SQUATCH_RIG_MODEL)\n"
    "  --ir PATH          cabinet impulse response, WAV (or $SQUATCH_RIG_IR)\n"
    "  --ir-max-ms MS     cut the response to this length (default 200)\n"
    "  --ir-no-resample   refuse a response at another rate instead of resampling it\n"
    "  --no-STAGE         bypass a stage: gate, tuner, comp, od, claw, crunch,\n"
    "                     amp, eq, cab, hall, limiter\n"
    "  --with-STAGE       switch on a stage that starts off: comp, od, claw, crunch, hall\n"
    "  --stream HZ        also print the tuner, meters and switches HZ times a second (for pedal-web)\n"
    "  --gate-db DB       gate threshold (default -60)\n"
    "  --in-db DB         amp input gain (default 0)\n"
    "  --amp-db DB        amp output trim (default 0)\n"
    "  --no-normalize     do not level the model's output by its loudness figure\n"
    "  --cab-db DB        cabinet level (default 0)\n"
    "  --eq-low DB, --eq-mid DB, --eq-high DB   EQ band gains (default 0)\n"
    "  --ceiling DB       limiter ceiling, true peak (default -1)\n"
    "  --seconds S        stop after S seconds (default: run until stopped)\n"
    "  --name NAME        JACK client name (default squatch-rig)\n";

struct Options {
  int input = 1;
  std::vector<std::string> outs;
  bool loopback = false;
  const char* model = nullptr;
  const char* ir = nullptr;
  float irMaxMs = 200.0f;
  bool irNoResample = false, noNormalize = false;
  bool bypass[kStages] = {false, false, true, true, true, true, false, false, false, true, false};  // Comp, Od, Claw, Crunch, Hall off
  float streamHz = 0.0f;
  float gateDb = -60.0f, inDb = 0.0f, ampDb = 0.0f, cabDb = 0.0f;
  float eqLow = 0.0f, eqMid = 0.0f, eqHigh = 0.0f, ceilingDb = -1.0f;
  float seconds = 0.0f;
  const char* name = "squatch-rig";
  extra::Options extra;
};

inline void printUsage(std::FILE* f) {
  std::fputs(kUsage, f);
  std::fputs(extra::kUsage, f);
}

namespace options_detail {

struct Number {
  const char* flag;
  float Options::*field;
  float lo, hi;
};

constexpr Number kNumbers[] = {
    {"--ir-max-ms", &Options::irMaxMs, 1.0f, 2000.0f}, {"--gate-db", &Options::gateDb, -100.0f, 0.0f},
    {"--in-db", &Options::inDb, -24.0f, 24.0f},        {"--amp-db", &Options::ampDb, -40.0f, 12.0f},
    {"--cab-db", &Options::cabDb, -40.0f, 20.0f},      {"--eq-low", &Options::eqLow, -18.0f, 18.0f},
    {"--eq-mid", &Options::eqMid, -18.0f, 18.0f},      {"--eq-high", &Options::eqHigh, -18.0f, 18.0f},
    {"--ceiling", &Options::ceilingDb, -24.0f, 0.0f},  {"--seconds", &Options::seconds, 0.0f, 1e7f},
    {"--stream", &Options::streamHz, 1.0f, 100.0f},
};

inline bool number(const char* text, float lo, float hi, float& out) {
  char* end = nullptr;
  const float v = std::strtof(text, &end);
  if (end == text || *end != '\0' || !(v >= lo && v <= hi)) return false;
  out = v;
  return true;
}

// Flags that take no value. True when `key` was one of them.
inline bool toggle(std::string_view key, Options& o) {
  if (key == "--loopback") return o.loopback = true;
  if (key == "--ir-no-resample") return o.irNoResample = true;
  if (key == "--no-normalize") return o.noNormalize = true;
  for (int s = 0; s < kStages; ++s) {
    if (key.substr(0, 5) == "--no-" && key.substr(5) == stageName(s)) return o.bypass[s] = true;
    if (key.substr(0, 7) == "--with-" && key.substr(7) == stageName(s)) return !(o.bypass[s] = false);
  }
  return extra::toggle(key, o.extra);
}

// Flags that take a value. False when the key is unknown or the value is not valid.
inline bool valued(std::string_view key, const char* v, Options& o) {
  for (const Number& n : kNumbers)
    if (key == n.flag) return number(v, n.lo, n.hi, o.*n.field);
  if (key == "--input") return (o.input = std::atoi(v)) >= 1 && o.input <= 64;
  if (key == "--out") return o.outs.emplace_back(v), true;
  if (key == "--model") return (o.model = v) != nullptr;
  if (key == "--ir") return (o.ir = v) != nullptr;
  if (key == "--name") return (o.name = v) != nullptr;
  return extra::named(key, v, o.extra);
}

inline const char* env(const char* name) {
  const char* v = std::getenv(name);
  return v && *v ? v : nullptr;
}

// Defaults from the environment, and the output rule: playback_1 only with --loopback.
inline bool finish(Options& o) {
  if (!o.model) o.model = env("SQUATCH_RIG_MODEL");
  if (!o.ir) o.ir = env("SQUATCH_RIG_IR");
  if (o.outs.empty()) o.outs.emplace_back("system:playback_2");
  for (const std::string& p : o.outs)
    if (p == "system:playback_1" && !o.loopback) {
      std::fprintf(stderr, "squatch-rig-live: system:playback_1 is the loopback cable; add --loopback to use it\n");
      return false;
    }
  return true;
}

}  // namespace options_detail

/// False (after a message on stderr) when the command line is not usable.
inline bool parseOptions(int argc, char** argv, Options& o) {
  for (int i = 1; i < argc; ++i) {
    if (options_detail::toggle(argv[i], o)) continue;
    if (i + 1 < argc && options_detail::valued(argv[i], argv[i + 1], o)) {
      ++i;
      continue;
    }
    std::fprintf(stderr, "squatch-rig-live: bad option or value: %s\n", argv[i]);
    return false;
  }
  return options_detail::finish(o);
}

}  // namespace squatch::rig
