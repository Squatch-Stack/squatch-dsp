// SPDX-License-Identifier: MIT
// squatch-tone: a test tone at an exact frequency on one JACK output, for checking the tuner
// through a real loopback cable.
//
//   squatch-tone HZ [--db DBFS] [--port system:playback_N] [--seconds S]
//
// Defaults: -20 dBFS on system:playback_1 until stopped (Ctrl-C, SIGTERM). The phase is a
// double accumulator, so the frequency is exact to the interface's own sample clock, which the
// input shares: the tuner's reading can be compared with HZ directly. It fades in and out over
// 10 ms. Once the tone is playing it prints one JSON line on stdout: the wall-clock time the
// first sample reached the output jack, and the playback latency, for end-to-end timing.
#include <jack/jack.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <string_view>
#include <thread>

#include "../common/tone_gen.hpp"
#include "jack_session.hpp"

namespace {

using squatch::host::nowNs;
using squatch::host::running;
using squatch::host::ToneGen;

struct Tone {
  jack_client_t* client = nullptr;
  jack_port_t* out = nullptr;
  ToneGen gen;
  std::atomic<jack_nframes_t> firstFrame{0};
  std::atomic<bool> started{false};
};

Tone gTone;

int process(jack_nframes_t n, void* arg) {
  Tone& t = *static_cast<Tone*>(arg);
  auto* o = static_cast<float*>(jack_port_get_buffer(t.out, n));
  const float target = t.gen.target.load(std::memory_order_relaxed);
  if (target > 0.0f && !t.started.load(std::memory_order_relaxed)) {
    t.firstFrame.store(jack_last_frame_time(t.client), std::memory_order_relaxed);
    t.started.store(true, std::memory_order_release);
  }
  t.gen.fill(o, n);
  return 0;
}

struct Args {
  double hz = 0.0, db = -20.0, seconds = 0.0;
  const char* port = "system:playback_1";
};

bool apply(std::string_view key, const char* v, Args& a) {
  if (key == "--db") return (a.db = std::strtod(v, nullptr)) <= -6.0;
  if (key == "--port") return (a.port = v) != nullptr;
  if (key == "--seconds") return (a.seconds = std::strtod(v, nullptr)) >= 0.0;
  return false;
}

bool parse(int argc, char** argv, Args& a) {
  if (argc < 2 || (a.hz = std::strtod(argv[1], nullptr)) <= 0.0) return false;
  for (int i = 2; i < argc; i += 2)
    if (i + 1 >= argc || !apply(argv[i], argv[i + 1], a)) return false;
  return true;
}

// Once the first callback has run: when did its first sample leave the output jack?
void reportStart(const Tone& t) {
  while (running().load() && !t.started.load(std::memory_order_acquire)) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  jack_latency_range_t r{};
  jack_port_get_latency_range(t.out, JackPlaybackLatency, &r);
  const double rate = jack_get_sample_rate(t.client);
  const double ageUs = static_cast<double>(jack_get_time() - jack_frames_to_time(t.client, t.firstFrame.load()));
  const double outMs = 1000.0 * r.max / rate;
  const double startMs = static_cast<double>(nowNs(CLOCK_REALTIME)) / 1e6 - ageUs / 1e3 + outMs;
  std::printf("{\"toneStart\":%.1f,\"playMs\":%.2f,\"hz\":%.6f}\n", startMs, outMs, t.gen.step * rate);
  std::fflush(stdout);
}

void playUntilStopped(Tone& t, double seconds) {
  const uint64_t end = seconds > 0.0 ? nowNs() + static_cast<uint64_t>(seconds * 1e9) : UINT64_MAX;
  while (running().load() && nowNs() < end) std::this_thread::sleep_for(std::chrono::milliseconds(20));
  t.gen.target.store(0.0f);  // fade out, then close
  std::this_thread::sleep_for(std::chrono::milliseconds(40));
}

}  // namespace

int main(int argc, char** argv) {
  Args a;
  if (!parse(argc, argv, a)) {
    std::fprintf(stderr, "usage: squatch-tone HZ [--db DBFS (<= -6)] [--port system:playback_N] [--seconds S]\n");
    return 64;
  }
  Tone& t = gTone;
  if (!(t.client = squatch::host::openClient("squatch-tone"))) return 2;
  const double rate = jack_get_sample_rate(t.client);
  t.gen.configure(a.hz, a.db, rate);
  t.out = jack_port_register(t.client, "out", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
  jack_set_process_callback(t.client, process, &t);
  if (!t.out || jack_activate(t.client) != 0) return 2;
  if (!squatch::host::connectPorts(t.client, jack_port_name(t.out), a.port)) return 2;
  t.gen.target.store(1.0f);
  reportStart(t);
  playUntilStopped(t, a.seconds);
  jack_client_close(t.client);
  return 0;
}
