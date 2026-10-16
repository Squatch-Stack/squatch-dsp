// SPDX-License-Identifier: MIT
// squatch-tuner-live: the Squatch Tuner behind a live JACK input, publishing what a display
// needs as one JSON line per update on stdout.
//
//   squatch-tuner-live [--input N] [--out none|mute|thru] [--a4 HZ] [--rate HZ] [--min-hz HZ]
//   squatch-tuner-live --file take.wav [--a4 HZ] [--rate HZ] [--min-hz HZ]
//
//   --input N   read system:capture_N (default 1: the UMC204HD's Input 1)
//   --out       none (default): no output ports. mute: outputs held silent, as a pedal tuner
//               mutes the rig. thru: the input passed to playback_1 and _2 unchanged.
//   --a4 HZ     reference pitch, 430..450 (default 440). Also settable at run time on stdin.
//   --rate HZ   display updates per second (default 30)
//   --min-hz HZ lowest note to look for (default 38: bass E1; 27 for a 5-string's B0)
//   --file WAV  no JACK: feed a recording through the same tuner in 64-frame periods (the bench's
//               JACK period) and print the same display fields, one line per 1/rate s of FILE time,
//               with "t" (ms into the file). For comparing platforms on the same audio offline.
//
// Threads:
//   JACK's realtime thread   process(): input -> Tuner -> Latest<Frame>. No allocation, no
//                            locks, no system calls, no I/O.
//   main thread              every 1/rate s: the newest Frame -> a JSON line (non-realtime).
//   command thread           stdin lines: "a4 442.5". EOF just ends the thread.
// SIGTERM or SIGINT stops it cleanly; so does jackd going away or the reader of stdout closing.
#include <jack/jack.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <thread>

#include "../common/display.hpp"
#include "../common/ir_file.hpp"
#include "../common/latest.hpp"
#include "jack_session.hpp"

namespace {

using squatch::host::DisplayReading;
using squatch::host::Latest;
using squatch::host::nowNs;
using squatch::host::running;

constexpr float kMinA4 = 430.0f, kMaxA4 = 450.0f;

enum class OutMode { None, Mute, Thru };

struct Options {
  int input = 1;
  OutMode out = OutMode::None;
  float a4 = 440.0f;
  float rateHz = 30.0f;
  float minHz = 38.0f;
  const char* file = nullptr;
};

// What the audio callback hands over, once per callback.
struct Frame {
  squatch::tuner::TunerReading reading{};
  float peak = 0.0f;              // decaying |x| peak: falls 20 dB per second
  jack_nframes_t frameTime = 0;   // JACK frame time at the start of this cycle
  uint64_t callbacks = 0;         // cumulative
  uint64_t samples = 0;           // input samples the tuner has heard: where in the audio `reading` is
  uint64_t busyNs = 0;            // cumulative time spent inside process()
  uint32_t worstNs = 0;           // longest single process() so far
};

// Everything the realtime callback touches. Static storage: the Tuner alone is ~128 kB.
struct Engine {
  jack_client_t* client = nullptr;
  jack_port_t* in = nullptr;
  std::array<jack_port_t*, 2> out{};
  OutMode outMode = OutMode::None;
  squatch::tuner::Tuner tuner;
  Latest<Frame> latest;
  float peak = 0.0f;
  float decayLog2PerFrame = 0.0f;
  uint64_t callbacks = 0, busyNs = 0, samples = 0;
  uint32_t worstNs = 0;
};

Engine gEngine;
std::atomic<float> gA4{440.0f};

// ---------------------------------------------------------------- realtime ----

float blockPeak(const float* x, jack_nframes_t n, float held) {
  for (jack_nframes_t i = 0; i < n; ++i) held = std::max(held, std::fabs(x[i]));
  return held;
}

void writeOutputs(Engine& e, const float* in, jack_nframes_t n) {
  for (jack_port_t* p : e.out) {
    if (!p) continue;
    auto* o = static_cast<float*>(jack_port_get_buffer(p, n));
    if (e.outMode == OutMode::Thru) std::memcpy(o, in, n * sizeof(float));
    else std::memset(o, 0, n * sizeof(float));
  }
}

int process(jack_nframes_t n, void* arg) {
  Engine& e = *static_cast<Engine*>(arg);
  const uint64_t t0 = nowNs();
  const auto* in = static_cast<const float*>(jack_port_get_buffer(e.in, n));
  e.tuner.process(in, static_cast<int>(n));
  e.samples += n;
  e.peak = blockPeak(in, n, e.peak * std::exp2(-e.decayLog2PerFrame * static_cast<float>(n)));
  writeOutputs(e, in, n);
  const auto dt = static_cast<uint32_t>(std::min<uint64_t>(nowNs() - t0, UINT32_MAX));
  e.busyNs += dt;
  e.worstNs = std::max(e.worstNs, dt);
  Frame& f = e.latest.back();
  f.reading = e.tuner.reading();
  f.peak = e.peak;
  f.frameTime = jack_last_frame_time(e.client);
  f.callbacks = ++e.callbacks;
  f.samples = e.samples;
  f.busyNs = e.busyNs;
  f.worstNs = e.worstNs;
  e.latest.publish();
  return 0;
}

// ---------------------------------------------------------------- set-up ----

bool parseOut(std::string_view v, OutMode& m) {
  if (v == "none") m = OutMode::None;
  else if (v == "mute") m = OutMode::Mute;
  else if (v == "thru") m = OutMode::Thru;
  else return false;
  return true;
}

bool applyOption(std::string_view key, const char* v, Options& o) {
  if (key == "--input") return (o.input = std::atoi(v)) >= 1;
  if (key == "--out") return parseOut(v, o.out);
  if (key == "--a4") return (o.a4 = std::strtof(v, nullptr)) >= kMinA4 && o.a4 <= kMaxA4;
  if (key == "--rate") return (o.rateHz = std::strtof(v, nullptr)) >= 1.0f && o.rateHz <= 120.0f;
  if (key == "--min-hz") return (o.minHz = std::strtof(v, nullptr)) >= 20.0f && o.minHz <= 200.0f;
  if (key == "--file") return (o.file = v) != nullptr;
  return false;
}

bool parseArgs(int argc, char** argv, Options& o) {
  for (int i = 1; i < argc; i += 2)
    if (i + 1 >= argc || !applyOption(argv[i], argv[i + 1], o)) return false;
  return true;
}

void registerOutputs(Engine& e, OutMode mode) {
  e.outMode = mode;
  if (mode == OutMode::None) return;
  e.out[0] = jack_port_register(e.client, "out_1", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
  e.out[1] = jack_port_register(e.client, "out_2", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
}

void connectAll(const Engine& e, const Options& o) {
  char from[64], to[64];
  std::snprintf(from, sizeof from, "system:capture_%d", o.input);
  squatch::host::connectPorts(e.client, from, jack_port_name(e.in));
  for (int k = 0; k < 2; ++k) {
    if (!e.out[k]) continue;
    std::snprintf(to, sizeof to, "system:playback_%d", k + 1);
    squatch::host::connectPorts(e.client, jack_port_name(e.out[k]), to);
  }
}

// configure() builds tables and is not real-time safe, so it runs before activation.
bool setUp(Engine& e, jack_client_t* c, const Options& o) {
  e.client = c;
  const auto rate = static_cast<float>(jack_get_sample_rate(c));
  squatch::tuner::TunerConfig cfg;
  cfg.sampleRate = rate;
  cfg.a4 = o.a4;
  cfg.minHz = o.minHz;
  e.tuner.configure(cfg);
  e.decayLog2PerFrame = 1.0f / (std::log10(2.0f) * rate);  // -20 dB/s = one decade per second
  e.in = jack_port_register(c, "in", JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
  registerOutputs(e, o.out);
  if (!e.in || (o.out != OutMode::None && !e.out[1])) return false;
  jack_set_process_callback(c, process, &e);
  if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) std::fprintf(stderr, "warning: mlockall failed\n");
  if (jack_activate(c) != 0) return false;
  connectAll(e, o);
  return true;
}

// ---------------------------------------------------------------- commands ----

void applyCommand(const char* line) {
  float hz = 0.0f;
  if (std::sscanf(line, "a4 %f", &hz) == 1 && hz >= kMinA4 && hz <= kMaxA4) gA4.store(hz);
}

// Raw read(2), not fgets: a thread blocked in fgets holds stdin's FILE lock, and exit() takes
// every FILE lock to flush, so the program could never exit while this thread waits for input.
void readCommands() {
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

// ---------------------------------------------------------------- publisher ----

double dbfs(float x) { return 20.0 * std::log10(std::max(x, 1e-6f)); }

// What the page draws, as the start of one JSON object (the caller closes it). The cents and the
// uncertainty go at full float precision (%.9g, as the phone sends them): the page rounds the number
// and filters its hold itself, so it must see the values every other display sees. `samples` and
// `rate` say where in the audio the reading was taken: the page's hold runs on that clock (hold.js).
struct AudioPos {
  uint64_t samples;
  double rate;
};
void printDisplay(const squatch::tuner::TunerReading& r, float peak, float a4, AudioPos at) {
  const DisplayReading d = squatch::host::toDisplay(r, a4);
  std::printf("{\"valid\":%s,\"note\":\"%s\",\"oct\":%d,\"midi\":%d,\"cents\":%.9g,\"fast\":%.3f,\"hz\":%.4f,\"shz\":%.4f,",
              d.valid ? "true" : "false", d.note.name, d.note.octave, d.midi, d.cents, d.fastCents, d.hz, d.steadyHz);
  std::printf("\"unc\":%.9g,\"parts\":%d,\"B\":%.3g,\"a4\":%.2f,\"lvl\":%.1f,\"pk\":%.1f,\"turns\":[", r.uncertainty, r.partials,
              r.inharmonicity, a4, dbfs(r.level), dbfs(peak));
  for (int k = 0; k < r.partials; ++k) std::printf(k ? ",%.4f" : "%.4f", r.strobeTurns[k]);
  std::printf("],\"samples\":%llu,\"rate\":%.0f", static_cast<unsigned long long>(at.samples), at.rate);
}

uint64_t processCpuNs() {
  rusage u{};
  getrusage(RUSAGE_SELF, &u);
  const auto ns = [](const timeval& t) { return static_cast<uint64_t>(t.tv_sec) * 1000000000ull + t.tv_usec * 1000ull; };
  return ns(u.ru_utime) + ns(u.ru_stime);
}

struct Load {
  double dspPct = 0.0;        // time in process() / time available, since the last line
  double worstPct = 0.0;      // longest process() so far / one period
  double cpuPct = 0.0;        // this process, all threads, % of one core
  double jackPct = 0.0;       // jackd's own figure for the whole graph
};

class Publisher {
 public:
  Publisher(Engine& e, const Options& o) : e_(e), rate_(jack_get_sample_rate(e.client)), input_(o.input) {
    period_ = jack_get_buffer_size(e.client);
    refreshLatency();
    std::fprintf(stderr, "squatch-tuner-live: %.0f Hz, period %u, input system:capture_%d, capture latency %.2f ms, realtime %s\n",
                 rate_, period_, o.input, captureMs_, jack_is_realtime(e.client) ? "yes" : "NO");
    last_ = Sample{0, 0, processCpuNs(), nowNs()};
  }

  void run(float rateHz) {
    const auto step = static_cast<uint64_t>(1e9 / rateHz);
    uint64_t deadline = nowNs();
    while (running().load()) {
      squatch::host::sleepUntil(deadline, step);
      if (e_.latest.update()) {
        have_ = true;
        freshNs_ = nowNs();
      }
      // A stalled graph (driver stopped, jackd suspended) stops process(); the last frame must
      // not be repeated as if it were live, or the page shows a frozen note. Silence lets the
      // bridge declare the stream stale.
      if (have_ && nowNs() - freshNs_ < kStaleNs) line(e_.latest.front());
    }
  }

 private:
  struct Sample {
    uint64_t callbacks, busyNs, cpuNs, wallNs;
  };
  static constexpr uint64_t kStaleNs = 250000000ull;  // 250 ms: about 190 periods at 64 / 48 kHz
  uint64_t freshNs_ = 0;

  Load load(const Frame& f) {
    const Sample now{f.callbacks, f.busyNs, processCpuNs(), nowNs()};
    const double periodNs = 1e9 * period_ / rate_;
    Load l;
    if (now.callbacks > last_.callbacks)
      l.dspPct = 100.0 * static_cast<double>(now.busyNs - last_.busyNs) / (periodNs * static_cast<double>(now.callbacks - last_.callbacks));
    if (now.wallNs > last_.wallNs) l.cpuPct = 100.0 * static_cast<double>(now.cpuNs - last_.cpuNs) / static_cast<double>(now.wallNs - last_.wallNs);
    l.worstPct = 100.0 * f.worstNs / periodNs;
    l.jackPct = jack_cpu_load(e_.client);
    last_ = now;
    return l;
  }

  // JACK recomputes port latencies after connections change, so this is read again each line.
  void refreshLatency() {
    jack_latency_range_t r{};
    jack_port_get_latency_range(e_.in, JackCaptureLatency, &r);
    captureMs_ = 1000.0 * r.max / rate_;
  }

  // Wall-clock time (ms since the epoch) at which the newest audio in this frame reached the
  // input jack: now, minus how long ago its JACK cycle began, minus the capture latency.
  // The difference is signed: JACK's filtered cycle-start estimate can run microseconds ahead of
  // jack_get_time(), and unsigned it wrapped to 2^64 us (tin came out near -1.8e16 ms).
  double inputTimeMs(const Frame& f) const {
    const auto age = static_cast<int64_t>(jack_get_time() - jack_frames_to_time(e_.client, f.frameTime));
    const double ageUs = static_cast<double>(age);
    return static_cast<double>(nowNs(CLOCK_REALTIME)) / 1e6 - ageUs / 1e3 - captureMs_;
  }

  void line(const Frame& f) {
    const float a4 = gA4.load();
    const Load l = load(f);
    refreshLatency();
    printDisplay(f.reading, f.peak, a4, {f.samples, rate_});
    std::printf(",\"input\":%d,\"tin\":%.1f,\"capMs\":%.2f,\"period\":%u,", input_, inputTimeMs(f), captureMs_, period_);
    std::printf("\"dsp\":%.2f,\"dspWorst\":%.1f,\"cpu\":%.2f,\"jack\":%.1f}\n", l.dspPct, l.worstPct, l.cpuPct, l.jackPct);
    if (std::fflush(stdout) != 0) running().store(false);  // the reader has gone
  }

  Engine& e_;
  double rate_;
  int input_;
  jack_nframes_t period_ = 0;
  double captureMs_ = 0.0;
  Sample last_{};
  bool have_ = false;
};

int usage() {
  std::fprintf(stderr, "usage: squatch-tuner-live [--input N] [--out none|mute|thru] [--a4 HZ] [--rate HZ] [--min-hz HZ]\n"
                       "       squatch-tuner-live --file take.wav [--a4 HZ] [--rate HZ] [--min-hz HZ]\n");
  return 64;
}

// --file: the realtime path without JACK. 64-frame periods, the peak meter as in process(), and a
// display line after the period that crosses each 1/rate s of file time.
int runFile(const Options& o) {
  constexpr size_t kPeriod = 64;
  const squatch::host::Wav w = squatch::host::readWav(o.file);
  if (!w.error.empty()) return std::fprintf(stderr, "squatch-tuner-live: %s: %s\n", o.file, w.error.c_str()), 2;
  Engine& e = gEngine;
  squatch::tuner::TunerConfig cfg;
  cfg.sampleRate = static_cast<float>(w.rate);
  cfg.a4 = o.a4;
  cfg.minHz = o.minHz;
  e.tuner.configure(cfg);
  e.decayLog2PerFrame = 1.0f / (std::log10(2.0f) * cfg.sampleRate);
  const double step = w.rate / o.rateHz;
  double next = step;
  for (size_t i = 0; i + kPeriod <= w.samples.size(); i += kPeriod) {
    e.tuner.process(&w.samples[i], static_cast<int>(kPeriod));
    e.peak = blockPeak(&w.samples[i], kPeriod, e.peak * std::exp2(-e.decayLog2PerFrame * static_cast<float>(kPeriod)));
    for (; next <= static_cast<double>(i + kPeriod); next += step) {
      printDisplay(e.tuner.reading(), e.peak, o.a4, {i + kPeriod, w.rate});
      std::printf(",\"t\":%.17g}\n", 1000.0 * static_cast<double>(i + kPeriod) / w.rate);  // exact: the hold's 500 ms edge
    }
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  Options o;
  if (!parseArgs(argc, argv, o)) return usage();
  if (o.file) return runFile(o);
  gA4.store(o.a4);
  jack_client_t* c = squatch::host::openClient("squatch-tuner");
  if (!c) return 2;
  if (!setUp(gEngine, c, o)) {
    std::fprintf(stderr, "squatch-tuner-live: could not register or activate the JACK client\n");
    jack_client_close(c);
    return 2;
  }
  std::thread(readCommands).detach();
  Publisher(gEngine, o).run(o.rateHz);
  jack_client_close(c);
  return 0;
}
