// SPDX-License-Identifier: MIT
// squatch-tuner-live for macOS: the Squatch Tuner behind a CoreAudio input, publishing what a display
// needs as one JSON line per update on stdout. The same options and the same lines as the JACK host
// (hosts/jack/tuner_live.cpp), on the Mac's own audio system: nothing to install.
//
//   squatch-tuner-live [--device NAME] [--input N] [--out none|mute|thru] [--a4 HZ] [--rate HZ] [--min-hz HZ]
//   squatch-tuner-live --file take.wav [--a4 HZ] [--rate HZ] [--min-hz HZ]
//   squatch-tuner-live --list-devices
//
//   --device NAME  the audio device to use: the first whose name contains NAME, in any case.
//                  Default: the Mac's default input device. Nothing here changes the Mac's defaults.
//   --input N      channel N of that device (default 1: the UMC204HD's Input 1)
//   --out          none (default): the outputs are left silent. mute: the same, said aloud, for a pedal
//                  tuner that mutes the rig. thru: the input on outputs 1 and 2 of the device, which then
//                  must have both (an interface); a Mac's microphone and speakers are two devices.
//   --list-devices every audio device with its channels, rate and which are the defaults, then exit
//   --a4, --rate, --min-hz, --file   as the JACK host. "jack" in each line is 0 (CoreAudio has no server).
//
// First run: macOS asks once whether your terminal may use the microphone. If it is refused the input
// is digital silence, and the tuner says so on stderr after two seconds.
//
// Threads: the HAL's real-time thread runs TunerRender (no allocation, no locks); the main thread
// prints a line every 1/rate s; a command thread reads "a4 442.5" from stdin.
#include <new>
#include <thread>

#include "../common/tuner_publisher.hpp"
#include "coreaudio_session.hpp"
#include "tuner_render.hpp"

namespace {

using namespace squatch::host;
using namespace squatch::host::live;

struct Engine {
  Core core;
  alignas(64) char renderStorage[sizeof(TunerRender)];
  TunerRender* render = nullptr;
  ca::HostClock clock;
};

Engine gEngine;

constexpr uint32_t kMaxStreams = 16;

// The HAL's real-time callback: Chunks, then the shared render step.
void onAudio(void* ctx, const AudioBufferList* in, const AudioTimeStamp* inTime, AudioBufferList* out, const AudioTimeStamp*) {
  Engine& e = *static_cast<Engine*>(ctx);
  audio::Chunk ic[kMaxStreams], oc[kMaxStreams];
  const uint32_t ni = ca::chunksOf(in, ic, kMaxStreams), no = ca::chunksOf(out, oc, kMaxStreams);
  const uint64_t host = inTime && (inTime->mFlags & kAudioTimeStampHostTimeValid) ? e.clock.ns(inTime->mHostTime) : e.clock.nowNs();
  e.render->render({ic, ni}, {oc, no}, host);
}

// What the publisher needs from CoreAudio.
class CoreAudioBackend {
 public:
  static constexpr bool kWarnDigitalSilence = true;
  CoreAudioBackend(const ca::DeviceInfo& d, const ca::HostClock& clock) : dev_(d), clock_(clock) {}

  double rate() const { return dev_.rate; }
  uint32_t period() const { return period_; }
  double captureMs() const { return 1000.0 * dev_.inLatency / dev_.rate; }
  double serverLoadPct() const { return 0.0; }
  void refreshLatency() { period_ = ca::bufferFramesOf(dev_.id); }

  // Wall-clock time (ms since the epoch) at which the newest audio in this frame reached the input:
  // now, minus how long ago the buffer's last frame was captured, minus the capture latency the device
  // reports (device + safety offset + stream). The HAL's figure, not a loopback measurement.
  double inputTimeMs(const Frame& f) const {
    const auto ageNs = static_cast<int64_t>(clock_.nowNs()) - static_cast<int64_t>(f.cycleStamp);
    return static_cast<double>(nowNs(CLOCK_REALTIME)) / 1e6 - static_cast<double>(ageNs) / 1e6 - captureMs();
  }

 private:
  ca::DeviceInfo dev_;
  const ca::HostClock& clock_;
  uint32_t period_ = 0;
};

int usage() {
  std::fprintf(stderr, "usage: squatch-tuner-live [--device NAME] [--input N] [--out none|mute|thru] [--a4 HZ] [--rate HZ] [--min-hz HZ]\n"
                       "       squatch-tuner-live --file take.wav [--a4 HZ] [--rate HZ] [--min-hz HZ]\n"
                       "       squatch-tuner-live --list-devices\n");
  return 64;
}

bool checkDevice(const ca::DeviceInfo& d, const Options& o) {
  if (!d.floatFormat) return std::fprintf(stderr, "squatch-tuner-live: %s: stream format is not 32-bit float\n", d.name.c_str()), false;
  if (d.rate <= 0.0) return std::fprintf(stderr, "squatch-tuner-live: %s: no sample rate\n", d.name.c_str()), false;
  if (static_cast<uint32_t>(o.input) > d.inChannels)
    return std::fprintf(stderr, "squatch-tuner-live: %s has %u input channel(s), not %d\n", d.name.c_str(), d.inChannels, o.input), false;
  if (o.out == OutMode::Thru && d.outChannels == 0)
    return std::fprintf(stderr, "squatch-tuner-live: --out thru needs a device with outputs; %s has none (try --device NAME for an interface)\n",
                        d.name.c_str()), false;
  if (o.out == OutMode::Mute && d.outChannels == 0) std::fprintf(stderr, "note: %s has no outputs, so there is nothing to mute\n", d.name.c_str());
  return true;
}

int runLive(const Options& o) {
  const auto devices = ca::listDevices();
  ca::DeviceInfo dev;
  std::string why;
  if (!ca::pickDevice(devices, o.device, true, dev, why)) return std::fprintf(stderr, "squatch-tuner-live: %s\n", why.c_str()), 2;
  if (!checkDevice(dev, o)) return 2;
  Engine& e = gEngine;
  e.core.configure(static_cast<float>(dev.rate), o.a4, o.minHz);
  e.render = new (e.renderStorage) TunerRender(e.core, static_cast<uint32_t>(o.input - 1), o.out, dev.rate);
  liveA4().store(o.a4);
  installStopSignals();
  ca::Stream stream;
  if (!stream.start(dev.id, onAudio, &e, why)) return std::fprintf(stderr, "squatch-tuner-live: %s\n", why.c_str()), 2;
  CoreAudioBackend be(dev, e.clock);
  be.refreshLatency();
  std::fprintf(stderr, "squatch-tuner-live: %s, %.0f Hz, buffer %u frames, input channel %d, capture latency %.2f ms (CoreAudio)\n", dev.name.c_str(),
               be.rate(), be.period(), o.input, be.captureMs());
  std::thread(readCommands).detach();
  Publisher<CoreAudioBackend>(e.core, be, o.input).run(o.rateHz);
  stream.stop();
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  Options o;
  if (!parseArgs(argc, argv, o, true)) return usage();
  if (o.listDevices) return ca::printDevices(ca::listDevices(), stdout), 0;
  if (o.file) return runFile(o, gEngine.core);
  return runLive(o);
}
