// SPDX-License-Identifier: MIT
// The main thread of squatch-tuner-live, for any audio backend: every 1/rate s, the newest Frame
// becomes one JSON line on stdout. Non-real-time. The backend (JACK or CoreAudio) supplies only what
// differs: its rate and period, the capture latency, when the newest audio reached the input, and
// the server's own load figure.
//
//   Backend::kWarnDigitalSilence   true where all-zero input for two seconds means "no permission"
//   double rate(), uint32_t period()
//   void refreshLatency(), double captureMs()
//   double inputTimeMs(const Frame&)        wall-clock ms (epoch) the newest audio reached the input
//   double serverLoadPct()                  the audio server's load, 0 where there is none
#pragma once
#include "tuner_host.hpp"

namespace squatch::host::live {

inline uint64_t processCpuNs() {
  rusage u{};
  getrusage(RUSAGE_SELF, &u);
  const auto ns = [](const timeval& t) { return static_cast<uint64_t>(t.tv_sec) * 1000000000ull + static_cast<uint64_t>(t.tv_usec) * 1000ull; };
  return ns(u.ru_utime) + ns(u.ru_stime);
}

struct Load {
  double dspPct = 0.0;        // time in processBlock() / time available, since the last line
  double worstPct = 0.0;      // longest processBlock() so far / one period
  double cpuPct = 0.0;        // this process, all threads, % of one core
  double serverPct = 0.0;     // the audio server's own figure for the whole graph (JACK)
};

template <class Backend>
class Publisher {
 public:
  Publisher(Core& core, Backend& be, int input) : core_(core), be_(be), input_(input) {
    be_.refreshLatency();
    last_ = Sample{0, 0, processCpuNs(), nowNs()};
  }

  void run(float rateHz) {
    const auto step = static_cast<uint64_t>(1e9 / rateHz);
    uint64_t deadline = nowNs();
    while (running().load()) {
      sleepUntil(deadline, step);
      if (core_.latest.update()) {
        have_ = true;
        freshNs_ = nowNs();
      }
      // A stalled graph (driver stopped, server suspended) stops the callback; the last frame must
      // not be repeated as if it were live, or the page shows a frozen note. Silence lets the
      // bridge declare the stream stale.
      if (have_ && nowNs() - freshNs_ < kStaleNs) line(core_.latest.front());
    }
  }

 private:
  struct Sample {
    uint64_t callbacks, busyNs, cpuNs, wallNs;
  };
  static constexpr uint64_t kStaleNs = 250000000ull;  // 250 ms: about 190 periods at 64 / 48 kHz

  Load load(const Frame& f) {
    const Sample now{f.callbacks, f.busyNs, processCpuNs(), nowNs()};
    const double periodNs = 1e9 * be_.period() / be_.rate();
    Load l;
    if (now.callbacks > last_.callbacks)
      l.dspPct = 100.0 * static_cast<double>(now.busyNs - last_.busyNs) / (periodNs * static_cast<double>(now.callbacks - last_.callbacks));
    if (now.wallNs > last_.wallNs) l.cpuPct = 100.0 * static_cast<double>(now.cpuNs - last_.cpuNs) / static_cast<double>(now.wallNs - last_.wallNs);
    l.worstPct = 100.0 * f.worstNs / periodNs;
    l.serverPct = be_.serverLoadPct();
    last_ = now;
    return l;
  }

  // An input that has delivered nothing but exact zeros for two seconds is not a quiet guitar: on macOS it is
  // a microphone permission that was refused. Say so once, on stderr.
  void warnIfSilent(const Frame& f) {
    if constexpr (Backend::kWarnDigitalSilence) {
      if (f.peak > 0.0f) heard_ = true;
      if (!heard_ && !warned_ && static_cast<double>(f.samples) > 2.0 * be_.rate()) {
        warned_ = true;
        std::fprintf(stderr, "squatch-tuner-live: the input has been digital silence for 2 s. If this is a microphone, allow your terminal "
                             "under System Settings > Privacy & Security > Microphone, or pick another input with --device.\n");
      }
    }
  }

  void line(const Frame& f) {
    const float a4 = liveA4().load();
    const Load l = load(f);
    be_.refreshLatency();
    warnIfSilent(f);
    printDisplay(f.reading, f.peak, a4, {f.samples, be_.rate()});
    std::printf(",\"input\":%d,\"tin\":%.1f,\"capMs\":%.2f,\"period\":%u,", input_, be_.inputTimeMs(f), be_.captureMs(), be_.period());
    std::printf("\"dsp\":%.2f,\"dspWorst\":%.1f,\"cpu\":%.2f,\"jack\":%.1f}\n", l.dspPct, l.worstPct, l.cpuPct, l.serverPct);
    if (std::fflush(stdout) != 0) running().store(false);  // the reader has gone
  }

  Core& core_;
  Backend& be_;
  int input_;
  Sample last_{};
  bool have_ = false, heard_ = false, warned_ = false;
  uint64_t freshNs_ = 0;
};

}  // namespace squatch::host::live
