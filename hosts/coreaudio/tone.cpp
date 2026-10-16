// SPDX-License-Identifier: MIT
// squatch-tone for macOS: a test tone at an exact frequency on one CoreAudio output channel, for
// checking the tuner through a real loopback cable. The same options and output as the JACK host
// (hosts/jack/tone.cpp).
//
//   squatch-tone HZ [--db DBFS] [--device NAME] [--port N] [--seconds S]
//   squatch-tone --list-devices
//
// Defaults: -20 dBFS on output channel 1 of the Mac's default output device until stopped (Ctrl-C,
// SIGTERM). --device NAME picks the first device whose name contains NAME (any case). --port takes a
// channel number, or the JACK spelling system:playback_N. The phase is a double accumulator (see
// common/tone_gen.hpp). Once the tone is playing it prints one JSON line on stdout: the wall-clock time
// the first sample reached the output, and the playback latency, for end-to-end timing. Both are the
// device's reported latency, not a loopback measurement.
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <thread>

#include "../common/tone_gen.hpp"
#include "coreaudio_session.hpp"

namespace {

using namespace squatch::host;

struct Tone {
  ToneGen gen;
  ca::HostClock clock;
  uint32_t channel = 0;                    // 0-based
  std::atomic<uint64_t> firstHostNs{0};    // mach ns the first audible sample is due at the device
  std::atomic<bool> started{false};
};

Tone gTone;

constexpr uint32_t kMaxStreams = 16;

void onAudio(void* ctx, const AudioBufferList*, const AudioTimeStamp*, AudioBufferList* out, const AudioTimeStamp* outTime) {
  Tone& t = *static_cast<Tone*>(ctx);
  audio::Chunk oc[kMaxStreams];
  const uint32_t n = ca::chunksOf(out, oc, kMaxStreams);
  audio::clearAll({oc, n});
  uint32_t first = 0;
  for (uint32_t i = 0; i < n; first += oc[i].channels, ++i) {
    if (t.channel < first || t.channel >= first + oc[i].channels || !oc[i].data) continue;
    const uint32_t frames = oc[i].bytes / (oc[i].channels * static_cast<uint32_t>(sizeof(float)));
    const float target = t.gen.fill(static_cast<float*>(oc[i].data) + (t.channel - first), frames, oc[i].channels);
    if (target > 0.0f && !t.started.load(std::memory_order_relaxed)) {
      t.firstHostNs.store(outTime ? t.clock.ns(outTime->mHostTime) : t.clock.nowNs(), std::memory_order_relaxed);
      t.started.store(true, std::memory_order_release);
    }
    return;
  }
}

struct Args {
  double hz = 0.0, db = -20.0, seconds = 0.0;
  uint32_t channel = 1;
  const char* device = nullptr;
  bool list = false;
};

// "3" or "system:playback_3" -> 3; 0 if it is neither.
uint32_t channelOf(const char* v) {
  const char* digits = std::strrchr(v, '_');
  const long n = std::strtol(digits ? digits + 1 : v, nullptr, 10);
  return n >= 1 && n <= 1024 ? static_cast<uint32_t>(n) : 0;
}

bool apply(std::string_view key, const char* v, Args& a) {
  if (key == "--db") return (a.db = std::strtod(v, nullptr)) <= -6.0;
  if (key == "--port") return (a.channel = channelOf(v)) != 0;
  if (key == "--seconds") return (a.seconds = std::strtod(v, nullptr)) >= 0.0;
  if (key == "--device") return (a.device = v) != nullptr;
  return false;
}

bool parse(int argc, char** argv, Args& a) {
  if (argc == 2 && std::string_view(argv[1]) == "--list-devices") return a.list = true;
  if (argc < 2 || (a.hz = std::strtod(argv[1], nullptr)) <= 0.0) return false;
  for (int i = 2; i < argc; i += 2)
    if (i + 1 >= argc || !apply(argv[i], argv[i + 1], a)) return false;
  return true;
}

void reportStart(const Tone& t, const ca::DeviceInfo& d) {
  while (running().load() && !t.started.load(std::memory_order_acquire)) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  const double outMs = 1000.0 * d.outLatency / d.rate;
  // The first sample is due at the device at firstHostNs (in the future, for a buffer being filled now),
  // and leaves the output jack outMs later.
  const double dueMs = (static_cast<double>(t.firstHostNs.load()) - static_cast<double>(t.clock.nowNs())) / 1e6;
  const double startMs = static_cast<double>(nowNs(CLOCK_REALTIME)) / 1e6 + dueMs + outMs;
  std::printf("{\"toneStart\":%.1f,\"playMs\":%.2f,\"hz\":%.6f}\n", startMs, outMs, t.gen.step * d.rate);
  std::fflush(stdout);
}

void playUntilStopped(Tone& t, double seconds) {
  const uint64_t end = seconds > 0.0 ? nowNs() + static_cast<uint64_t>(seconds * 1e9) : UINT64_MAX;
  while (running().load() && nowNs() < end) std::this_thread::sleep_for(std::chrono::milliseconds(20));
  t.gen.target.store(0.0f);  // fade out, then close
  std::this_thread::sleep_for(std::chrono::milliseconds(40));
}

int play(const Args& a) {
  const auto devices = ca::listDevices();
  ca::DeviceInfo dev;
  std::string why;
  if (!ca::pickDevice(devices, a.device, false, dev, why)) return std::fprintf(stderr, "squatch-tone: %s\n", why.c_str()), 2;
  if (!dev.floatFormat || dev.rate <= 0.0) return std::fprintf(stderr, "squatch-tone: %s: not a 32-bit float device\n", dev.name.c_str()), 2;
  if (a.channel > dev.outChannels)
    return std::fprintf(stderr, "squatch-tone: %s has %u output channel(s), not %u\n", dev.name.c_str(), dev.outChannels, a.channel), 2;
  Tone& t = gTone;
  t.channel = a.channel - 1;
  t.gen.configure(a.hz, a.db, dev.rate);
  installStopSignals();
  ca::Stream stream;
  if (!stream.start(dev.id, onAudio, &t, why)) return std::fprintf(stderr, "squatch-tone: %s\n", why.c_str()), 2;
  t.gen.target.store(1.0f);
  reportStart(t, dev);
  playUntilStopped(t, a.seconds);
  stream.stop();
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  Args a;
  if (!parse(argc, argv, a)) {
    std::fprintf(stderr, "usage: squatch-tone HZ [--db DBFS (<= -6)] [--device NAME] [--port N] [--seconds S]\n"
                         "       squatch-tone --list-devices\n");
    return 64;
  }
  if (a.list) return ca::printDevices(ca::listDevices(), stdout), 0;
  return play(a);
}
