// SPDX-License-Identifier: MIT
// What squatch-tuner-live does apart from talking to an audio system, shared by the JACK host
// (hosts/jack) and the CoreAudio host (hosts/coreaudio): the options, the real-time block step
// (input -> Tuner -> Latest<Frame>), the JSON line a display needs, the stdin commands, the 1/rate
// publisher, and --file (a recording through the same step, offline). No JACK or CoreAudio types
// here, so the offline test can drive the same path on any machine.
//
// Real-time rule: Core::processBlock() allocates nothing, takes no lock and makes no system call
// beyond the clock read. Everything else runs on the main or the command thread.
#pragma once
#include <sys/resource.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <vector>

#include "display.hpp"
#include "host_clock.hpp"
#include "ir_file.hpp"
#include "latest.hpp"

namespace squatch::host::live {

constexpr float kMinA4 = 430.0f, kMaxA4 = 450.0f;
constexpr size_t kFilePeriod = 64;  // frames per block for --file: the bench's JACK period

enum class OutMode { None, Mute, Thru };

struct Options {
  int input = 1;
  OutMode out = OutMode::None;
  float a4 = 440.0f;
  float rateHz = 30.0f;
  float minHz = 38.0f;
  const char* file = nullptr;
  const char* device = nullptr;  // CoreAudio host only: a name to look for, default the system's default
  bool listDevices = false;      // CoreAudio host only
};

// What the audio callback hands over, once per callback.
struct Frame {
  squatch::tuner::TunerReading reading{};
  float peak = 0.0f;           // decaying |x| peak: falls 20 dB per second
  uint64_t cycleStamp = 0;     // the backend's own stamp of when this cycle began (JACK frame time, mach ns)
  uint64_t callbacks = 0;      // cumulative
  uint64_t samples = 0;        // input samples the tuner has heard: where in the audio `reading` is
  uint64_t busyNs = 0;         // cumulative time spent inside processBlock()
  uint32_t worstNs = 0;        // longest single processBlock() so far
};

inline std::atomic<float>& liveA4() {
  static std::atomic<float> a4{440.0f};
  return a4;
}

inline float blockPeak(const float* x, size_t n, float held) {
  for (size_t i = 0; i < n; ++i) held = std::max(held, std::fabs(x[i]));
  return held;
}

/// Everything the real-time callback touches. Keep one in static storage: the Tuner alone is ~128 kB.
struct Core {
  squatch::tuner::Tuner tuner;
  Latest<Frame> latest;
  float peak = 0.0f;
  float decayLog2PerFrame = 0.0f;
  uint64_t callbacks = 0, busyNs = 0, samples = 0;
  uint32_t worstNs = 0;

  // configure() builds tables and is not real-time safe: call it before the audio starts.
  void configure(float rate, float a4, float minHz) {
    squatch::tuner::TunerConfig cfg;
    cfg.sampleRate = rate;
    cfg.a4 = a4;
    cfg.minHz = minHz;
    tuner.configure(cfg);
    decayLog2PerFrame = 1.0f / (std::log10(2.0f) * rate);  // -20 dB/s = one decade per second
  }

  float decayed(size_t n) const { return peak * std::exp2(-decayLog2PerFrame * static_cast<float>(n)); }

  /// The audio callback's body. Real-time safe.
  void processBlock(const float* in, int n, uint64_t cycleStamp) {
    const uint64_t t0 = nowNs();
    tuner.process(in, n);
    samples += static_cast<uint64_t>(n);
    peak = blockPeak(in, static_cast<size_t>(n), decayed(static_cast<size_t>(n)));
    const auto dt = static_cast<uint32_t>(std::min<uint64_t>(nowNs() - t0, UINT32_MAX));
    busyNs += dt;
    worstNs = std::max(worstNs, dt);
    Frame& f = latest.back();
    f.reading = tuner.reading();
    f.peak = peak;
    f.cycleStamp = cycleStamp;
    f.callbacks = ++callbacks;
    f.samples = samples;
    f.busyNs = busyNs;
    f.worstNs = worstNs;
    latest.publish();
  }
};

// ---------------------------------------------------------------- options ----

inline bool parseOut(std::string_view v, OutMode& m) {
  if (v == "none") m = OutMode::None;
  else if (v == "mute") m = OutMode::Mute;
  else if (v == "thru") m = OutMode::Thru;
  else return false;
  return true;
}

inline bool applyOption(std::string_view key, const char* v, Options& o) {
  if (key == "--input") return (o.input = std::atoi(v)) >= 1;
  if (key == "--out") return parseOut(v, o.out);
  if (key == "--a4") return (o.a4 = std::strtof(v, nullptr)) >= kMinA4 && o.a4 <= kMaxA4;
  if (key == "--rate") return (o.rateHz = std::strtof(v, nullptr)) >= 1.0f && o.rateHz <= 120.0f;
  if (key == "--min-hz") return (o.minHz = std::strtof(v, nullptr)) >= 20.0f && o.minHz <= 200.0f;
  if (key == "--file") return (o.file = v) != nullptr;
  return false;
}

/// `deviceFlags` adds the CoreAudio host's --device NAME and --list-devices.
inline bool parseArgs(int argc, char** argv, Options& o, bool deviceFlags = false) {
  for (int i = 1; i < argc;) {
    const std::string_view key = argv[i];
    if (deviceFlags && key == "--list-devices") {
      o.listDevices = true;
      i += 1;
      continue;
    }
    if (i + 1 >= argc) return false;
    const bool ok = (deviceFlags && key == "--device") ? (o.device = argv[i + 1]) != nullptr : applyOption(key, argv[i + 1], o);
    if (!ok) return false;
    i += 2;
  }
  return true;
}

// ---------------------------------------------------------------- commands ----

inline void applyCommand(const char* line) {
  float hz = 0.0f;
  if (std::sscanf(line, "a4 %f", &hz) == 1 && hz >= kMinA4 && hz <= kMaxA4) liveA4().store(hz);
}

// Raw read(2), not fgets: a thread blocked in fgets holds stdin's FILE lock, and exit() takes
// every FILE lock to flush, so the program could never exit while this thread waits for input.
inline void readCommands() {
  char line[128];
  size_t len = 0;
  char c;
  while (read(STDIN_FILENO, &c, 1) == 1) {
    if (c != '\n') {
      if (len < sizeof line - 1) line[len++] = c;  // an overlong line is cut, not overrun
      continue;
    }
    line[len] = '\0';
    applyCommand(line);
    len = 0;
  }
}

// ---------------------------------------------------------------- display line ----

inline double dbfs(float x) { return 20.0 * std::log10(std::max(x, 1e-6f)); }

// What the page draws, as the start of one JSON object (the caller closes it). The cents and the
// uncertainty go at full float precision (%.9g, as the phone sends them): the page rounds the number
// and filters its hold itself, so it must see the values every other display sees. `samples` and
// `rate` say where in the audio the reading was taken: the page's hold runs on that clock (hold.js).
struct AudioPos {
  uint64_t samples;
  double rate;
};
inline void printDisplay(const squatch::tuner::TunerReading& r, float peak, float a4, AudioPos at) {
  const DisplayReading d = toDisplay(r, a4);
  std::printf("{\"valid\":%s,\"note\":\"%s\",\"oct\":%d,\"midi\":%d,\"cents\":%.9g,\"fast\":%.3f,\"hz\":%.4f,\"shz\":%.4f,",
              d.valid ? "true" : "false", d.note.name, d.note.octave, d.midi, d.cents, d.fastCents, d.hz, d.steadyHz);
  std::printf("\"unc\":%.9g,\"parts\":%d,\"B\":%.3g,\"a4\":%.2f,\"lvl\":%.1f,\"pk\":%.1f,\"turns\":[", r.uncertainty, r.partials,
              r.inharmonicity, a4, dbfs(r.level), dbfs(peak));
  for (int k = 0; k < r.partials; ++k) std::printf(k ? ",%.4f" : "%.4f", r.strobeTurns[k]);
  std::printf("],\"samples\":%llu,\"rate\":%.0f", static_cast<unsigned long long>(at.samples), at.rate);
}

// ---------------------------------------------------------------- --file ----

/// The real-time step without an audio system: kFilePeriod-frame blocks (the bench's JACK period) through
/// Core::processBlock, and `emit(frame, samplesHeard, tMs)` after the period that crosses each 1/rate s of audio.
template <class Emit>
inline void runSamples(const std::vector<float>& samples, double rate, const Options& o, Core& core, Emit&& emit) {
  constexpr size_t period = kFilePeriod;
  core.configure(static_cast<float>(rate), o.a4, o.minHz);
  const double step = rate / o.rateHz;
  double next = step;
  for (size_t i = 0; i + period <= samples.size(); i += period) {
    core.processBlock(&samples[i], static_cast<int>(period), 0);
    core.latest.update();
    for (; next <= static_cast<double>(i + period); next += step)
      emit(core.latest.front(), i + period, 1000.0 * static_cast<double>(i + period) / rate);
  }
}

/// --file: a recording through runSamples, one display line on stdout each. Returns the exit code.
inline int runFile(const Options& o, Core& core) {
  const Wav w = readWav(o.file);
  if (!w.error.empty()) return std::fprintf(stderr, "squatch-tuner-live: %s: %s\n", o.file, w.error.c_str()), 2;
  runSamples(w.samples, w.rate, o, core, [&](const Frame& f, uint64_t heard, double tMs) {
    printDisplay(f.reading, f.peak, o.a4, {heard, w.rate});
    std::printf(",\"t\":%.17g}\n", tMs);  // exact: the hold's 500 ms edge
  });
  return 0;
}

}  // namespace squatch::host::live
