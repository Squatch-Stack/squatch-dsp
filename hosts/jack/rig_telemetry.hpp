// SPDX-License-Identifier: MIT
// The rig's telemetry, on the main thread: once a second it drains the Timing records the
// callback queued and prints one JSON line on stdout. Times are in microseconds, per stage:
// mean, p99 and worst over that second, plus the worst since the start.
//
//   {"t":..., "rate":48000, "period":64, "periodUs":1333.3, "callbacks":750, "xruns":0,
//    "xrunsTotal":0, "dropped":0, "oversize":0, "jack":4.1, "latencyMs":1.50, "inDb":-18.2,
//    "outDb":-7.9, "stages":{"gate":{"on":true,"mean":0.4,"p99":0.6,"worst":1.2,"worstEver":3.0},
//    ..., "total":{...}}, "tuner":{"valid":true,"note":"E","oct":2,"cents":-1.3,"hz":82.35}}
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#include "../common/display.hpp"
#include "rig_engine.hpp"

namespace squatch::rig {

struct Summary {
  double mean = 0.0, p99 = 0.0, worst = 0.0;
};

inline Summary summarise(std::vector<std::uint32_t>& ns) {
  Summary s;
  if (ns.empty()) return s;
  std::sort(ns.begin(), ns.end());
  double sum = 0.0;
  for (std::uint32_t v : ns) sum += v;
  s.mean = sum / static_cast<double>(ns.size()) / 1e3;
  s.p99 = ns[static_cast<std::size_t>(0.99 * static_cast<double>(ns.size() - 1))] / 1e3;
  s.worst = ns.back() / 1e3;
  return s;
}

class Telemetry {
 public:
  explicit Telemetry(Engine& e) : e_(e) {
    rate_ = jack_get_sample_rate(e.client);
    period_ = jack_get_buffer_size(e.client);
  }

  /// Drains the queue and prints one line. False when stdout has gone away.
  bool line() {
    drain();
    const std::uint32_t xr = e_.xruns.load();
    std::printf("{\"t\":%.0f,\"rate\":%.0f,\"period\":%u,\"periodUs\":%.1f,\"callbacks\":%zu,", nowMs(), rate_, period_,
                1e6 * period_ / rate_, ns_[kStages].size());
    std::printf("\"xruns\":%u,\"xrunsTotal\":%u,\"dropped\":%u,\"oversize\":%u,\"jack\":%.1f,\"latencyMs\":%.2f,", xr - lastXruns_,
                xr, e_.dropped.load(), e_.oversize.load(), jack_cpu_load(e_.client), 1e3 * algorithmicLatency(e_) / rate_);
    std::printf("\"inDb\":%.1f,\"outDb\":%.1f,\"stages\":{", db(inPeak_), db(outPeak_));
    for (int s = 0; s <= kStages; ++s) stage(s);
    std::printf("},");
    tuner();
    std::printf("}\n");
    lastXruns_ = xr;
    inPeak_ = outPeak_ = 0.0f;
    return std::fflush(stdout) == 0;
  }

 private:
  void drain() {
    Timing t;
    for (auto& v : ns_) v.clear();
    while (e_.timings.pop(t)) {
      for (int s = 0; s < kStages; ++s)
        if (t.ns[s]) ns_[s].push_back(t.ns[s]);
      ns_[kStages].push_back(t.totalNs);
      inPeak_ = std::max(inPeak_, t.inPeak);
      outPeak_ = std::max(outPeak_, t.outPeak);
    }
  }

  void stage(int s) {
    const Summary m = summarise(ns_[s]);
    worstEver_[s] = std::max(worstEver_[s], m.worst);
    const bool total = s == kStages;
    const bool on = total || (e_.slots[s].available && e_.slots[s].on.load());
    std::printf("%s\"%s\":{\"on\":%s,", s ? "," : "", total ? "total" : stageName(s), on ? "true" : "false");
    if (!total && !e_.slots[s].available) std::printf("\"available\":false,");
    std::printf("\"mean\":%.2f,\"p99\":%.2f,\"worst\":%.2f,\"worstEver\":%.2f}", m.mean, m.p99, m.worst, worstEver_[s]);
  }

  void tuner() {
    if (e_.tuner.readings().update()) have_ = true;
    if (!have_) return (void)std::printf("\"tuner\":null");
    const squatch::host::DisplayReading d = squatch::host::toDisplay(e_.tuner.readings().front().reading, 440.0f);
    std::printf("\"tuner\":{\"valid\":%s,\"note\":\"%s\",\"oct\":%d,\"cents\":%.2f,\"hz\":%.3f}", d.valid ? "true" : "false",
                d.note.name, d.note.octave, d.cents, d.steadyHz);
  }

  static double db(float x) { return 20.0 * std::log10(std::max(x, 1e-6f)); }
  static double nowMs() { return static_cast<double>(squatch::host::nowNs(CLOCK_REALTIME)) / 1e6; }

  Engine& e_;
  double rate_ = 48000.0;
  jack_nframes_t period_ = 0;
  std::vector<std::uint32_t> ns_[kStages + 1];
  double worstEver_[kStages + 1] = {};
  float inPeak_ = 0.0f, outPeak_ = 0.0f;
  std::uint32_t lastXruns_ = 0;
  bool have_ = false;
};

}  // namespace squatch::rig
