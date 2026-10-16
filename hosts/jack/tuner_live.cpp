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

#include <array>
#include <thread>

#include "../common/tuner_publisher.hpp"
#include "jack_session.hpp"

namespace {

using namespace squatch::host;
using namespace squatch::host::live;

// The JACK side of the host: ports, and the real-time callback around Core::processBlock.
struct Engine {
  jack_client_t* client = nullptr;
  jack_port_t* in = nullptr;
  std::array<jack_port_t*, 2> out{};
  OutMode outMode = OutMode::None;
  Core core;
};

Engine gEngine;

// ---------------------------------------------------------------- realtime ----

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
  const auto* in = static_cast<const float*>(jack_port_get_buffer(e.in, n));
  e.core.processBlock(in, static_cast<int>(n), jack_last_frame_time(e.client));
  writeOutputs(e, in, n);
  return 0;
}

// ---------------------------------------------------------------- set-up ----

void registerOutputs(Engine& e, OutMode mode) {
  e.outMode = mode;
  if (mode == OutMode::None) return;
  e.out[0] = jack_port_register(e.client, "out_1", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
  e.out[1] = jack_port_register(e.client, "out_2", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
}

void connectAll(const Engine& e, const Options& o) {
  char from[64], to[64];
  std::snprintf(from, sizeof from, "system:capture_%d", o.input);
  connectPorts(e.client, from, jack_port_name(e.in));
  for (int k = 0; k < 2; ++k) {
    if (!e.out[k]) continue;
    std::snprintf(to, sizeof to, "system:playback_%d", k + 1);
    connectPorts(e.client, jack_port_name(e.out[k]), to);
  }
}

bool setUp(Engine& e, jack_client_t* c, const Options& o) {
  e.client = c;
  e.core.configure(static_cast<float>(jack_get_sample_rate(c)), o.a4, o.minHz);
  e.in = jack_port_register(c, "in", JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
  registerOutputs(e, o.out);
  if (!e.in || (o.out != OutMode::None && !e.out[1])) return false;
  jack_set_process_callback(c, process, &e);
  if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) std::fprintf(stderr, "warning: mlockall failed\n");
  if (jack_activate(c) != 0) return false;
  connectAll(e, o);
  return true;
}

// ---------------------------------------------------------------- publisher backend ----

class JackBackend {
 public:
  static constexpr bool kWarnDigitalSilence = false;
  explicit JackBackend(const Engine& e) : e_(e), rate_(jack_get_sample_rate(e.client)), period_(jack_get_buffer_size(e.client)) {}

  double rate() const { return rate_; }
  uint32_t period() const { return period_; }
  double captureMs() const { return captureMs_; }
  double serverLoadPct() const { return jack_cpu_load(e_.client); }

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
    const auto age = static_cast<int64_t>(jack_get_time() - jack_frames_to_time(e_.client, static_cast<jack_nframes_t>(f.cycleStamp)));
    const double ageUs = static_cast<double>(age);
    return static_cast<double>(nowNs(CLOCK_REALTIME)) / 1e6 - ageUs / 1e3 - captureMs_;
  }

 private:
  const Engine& e_;
  double rate_;
  jack_nframes_t period_;
  double captureMs_ = 0.0;
};

int usage() {
  std::fprintf(stderr, "usage: squatch-tuner-live [--input N] [--out none|mute|thru] [--a4 HZ] [--rate HZ] [--min-hz HZ]\n"
                       "       squatch-tuner-live --file take.wav [--a4 HZ] [--rate HZ] [--min-hz HZ]\n");
  return 64;
}

}  // namespace

int main(int argc, char** argv) {
  Options o;
  if (!parseArgs(argc, argv, o)) return usage();
  if (o.file) return runFile(o, gEngine.core);
  liveA4().store(o.a4);
  jack_client_t* c = openClient("squatch-tuner");
  if (!c) return 2;
  if (!setUp(gEngine, c, o)) {
    std::fprintf(stderr, "squatch-tuner-live: could not register or activate the JACK client\n");
    jack_client_close(c);
    return 2;
  }
  JackBackend be(gEngine);
  be.refreshLatency();
  std::fprintf(stderr, "squatch-tuner-live: %.0f Hz, period %u, input system:capture_%d, capture latency %.2f ms, realtime %s\n", be.rate(),
               be.period(), o.input, be.captureMs(), jack_is_realtime(c) ? "yes" : "NO");
  std::thread(readCommands).detach();
  Publisher<JackBackend>(gEngine.core, be, o.input).run(o.rateHz);
  jack_client_close(c);
  return 0;
}
