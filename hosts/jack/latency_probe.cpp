// SPDX-License-Identifier: MIT
// squatch-latency: true round-trip latency through a real cable, by cross-correlation.
//
//   squatch-latency [--out PORT] [--in PORT] [--db DBFS] [--bursts N]
//
// Plays N short bursts of pseudo-random noise on --out (default system:playback_1, which the
// bench cables into Input 2) and records --in (default system:capture_2) in the same callbacks.
// Because a callback writes its output and reads its input in the same cycle, the lag between
// a burst as played and as recorded is exactly what a signal passing through any zero-latency
// JACK client in the same graph experiences: converter, driver and period buffering included.
// Point --in at another client's output (e.g. squatch-rig:out, fed from capture_2) to include
// that client's own delay.
//
// Prints one JSON line: each burst's lag in samples (sub-sample, parabolic peak), the median in
// samples and ms, the peak normalised correlation (near 1 for a clean loop), and JACK's own
// latency figures for the two ports. The bursts are audible on whatever --out also feeds.
#include <jack/jack.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <string_view>
#include <thread>
#include <vector>

#include "jack_session.hpp"

namespace {

using squatch::host::running;

constexpr int kBurst = 4096;           // samples of noise per burst
constexpr int kSpacing = 24000;        // samples between burst starts: 0.5 s at 48 kHz
constexpr int kMaxLag = 12000;         // search up to 250 ms at 48 kHz

struct Args {
  const char* out = "system:playback_1";
  const char* in = "system:capture_2";
  double db = -24.0;
  int bursts = 6;
};

struct Probe {
  jack_client_t* client = nullptr;
  jack_port_t* out = nullptr;
  jack_port_t* in = nullptr;
  std::vector<float> burst, played, recorded;  // sized before activation, never resized
  std::atomic<std::size_t> at{0};
  std::atomic<bool> go{false}, done{false};
};

Probe gProbe;

int process(jack_nframes_t n, void* arg) {
  Probe& p = *static_cast<Probe*>(arg);
  auto* o = static_cast<float*>(jack_port_get_buffer(p.out, n));
  const auto* i = static_cast<const float*>(jack_port_get_buffer(p.in, n));
  const bool go = p.go.load(std::memory_order_relaxed);
  std::size_t at = p.at.load(std::memory_order_relaxed);
  for (jack_nframes_t k = 0; k < n; ++k) {
    const bool live = go && at < p.recorded.size();
    o[k] = live ? p.played[at] : 0.0f;
    if (live) p.recorded[at++] = i[k];
  }
  p.at.store(at, std::memory_order_relaxed);
  if (at >= p.recorded.size()) p.done.store(true, std::memory_order_release);
  return 0;
}

std::vector<float> makeBurst(double db) {
  std::vector<float> b(kBurst);
  const float amp = static_cast<float>(std::pow(10.0, db / 20.0));
  std::uint32_t s = 0x2545F491u;
  for (int i = 0; i < kBurst; ++i) {
    s ^= s << 13, s ^= s >> 17, s ^= s << 5;
    const float fade = static_cast<float>(std::min({1.0, i / 48.0, (kBurst - 1 - i) / 48.0}));
    b[i] = amp * fade * (s & 1u ? 1.0f : -1.0f);
  }
  return b;
}

// Normalised cross-correlation of the burst against the recording starting at `from`.
double correlate(const std::vector<float>& rec, std::size_t from, const std::vector<float>& b, double bNorm) {
  if (from + b.size() > rec.size()) return 0.0;
  double dot = 0.0, energy = 0.0;
  for (std::size_t i = 0; i < b.size(); ++i) dot += double(b[i]) * rec[from + i], energy += double(rec[from + i]) * rec[from + i];
  return energy > 0.0 ? dot / std::sqrt(energy * bNorm) : 0.0;
}

struct Peak {
  double lag = -1.0, corr = 0.0;
};

Peak findLag(const Probe& p, std::size_t start) {
  double bNorm = 0.0;
  for (float v : p.burst) bNorm += double(v) * v;
  std::vector<double> c(kMaxLag + 1, 0.0);
  int best = 0;
  for (int lag = 0; lag <= kMaxLag; ++lag) {
    c[lag] = correlate(p.recorded, start + static_cast<std::size_t>(lag), p.burst, bNorm);
    if (std::fabs(c[lag]) > std::fabs(c[best])) best = lag;
  }
  Peak k{static_cast<double>(best), c[best]};
  if (best > 0 && best < kMaxLag) {  // parabolic interpolation around the peak
    const double a = std::fabs(c[best - 1]), m = std::fabs(c[best]), b = std::fabs(c[best + 1]);
    const double den = a - 2.0 * m + b;
    if (den != 0.0) k.lag += 0.5 * (a - b) / den;
  }
  return k;
}

bool apply(std::string_view key, const char* v, Args& a) {
  if (key == "--out") return (a.out = v) != nullptr;
  if (key == "--in") return (a.in = v) != nullptr;
  if (key == "--db") return (a.db = std::strtod(v, nullptr)) <= -12.0 && a.db >= -60.0;
  if (key == "--bursts") return (a.bursts = std::atoi(v)) >= 1 && a.bursts <= 50;
  return false;
}

bool parse(int argc, char** argv, Args& a) {
  for (int i = 1; i < argc; i += 2)
    if (i + 1 >= argc || !apply(argv[i], argv[i + 1], a)) return false;
  return true;
}

double portLatency(jack_port_t* port, jack_latency_callback_mode_t mode) {
  jack_latency_range_t r{};
  jack_port_get_latency_range(port, mode, &r);
  return r.max;
}

void setUpSignal(Probe& p, const Args& a) {
  p.burst = makeBurst(a.db);
  const std::size_t total = static_cast<std::size_t>(a.bursts) * kSpacing + kMaxLag + kBurst;
  p.played.assign(total, 0.0f);
  p.recorded.assign(total, 0.0f);
  for (int k = 0; k < a.bursts; ++k) std::copy(p.burst.begin(), p.burst.end(), p.played.begin() + static_cast<long>(k) * kSpacing);
}

void report(const Probe& p, const Args& a) {
  const double rate = jack_get_sample_rate(p.client);
  std::vector<double> lags;
  double worstCorr = 1.0;
  std::printf("{\"out\":\"%s\",\"in\":\"%s\",\"rate\":%.0f,\"period\":%u,\"lags\":[", a.out, a.in, rate, jack_get_buffer_size(p.client));
  for (int k = 0; k < a.bursts; ++k) {
    const Peak pk = findLag(p, static_cast<std::size_t>(k) * kSpacing);
    std::printf(k ? ",%.2f" : "%.2f", pk.lag);
    lags.push_back(pk.lag);
    worstCorr = std::min(worstCorr, std::fabs(pk.corr));
  }
  std::sort(lags.begin(), lags.end());
  const double median = lags[lags.size() / 2];
  const double jackRt = portLatency(p.out, JackPlaybackLatency) + portLatency(p.in, JackCaptureLatency);
  std::printf("],\"medianSamples\":%.2f,\"medianMs\":%.3f,\"spreadSamples\":%.2f,\"minCorr\":%.3f,", median, 1e3 * median / rate,
              lags.back() - lags.front(), worstCorr);
  std::printf("\"jackReportedSamples\":%.0f,\"jackReportedMs\":%.3f}\n", jackRt, 1e3 * jackRt / rate);
}

bool start(Probe& p, const Args& a) {
  setUpSignal(p, a);
  p.out = jack_port_register(p.client, "out", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
  p.in = jack_port_register(p.client, "in", JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
  jack_set_process_callback(p.client, process, &p);
  if (!p.out || !p.in || jack_activate(p.client) != 0) return false;
  return squatch::host::connectPorts(p.client, jack_port_name(p.out), a.out) &&
         squatch::host::connectPorts(p.client, a.in, jack_port_name(p.in));
}

}  // namespace

int main(int argc, char** argv) {
  Args a;
  if (!parse(argc, argv, a)) {
    std::fprintf(stderr, "usage: squatch-latency [--out PORT] [--in PORT] [--db DBFS (-60..-12)] [--bursts N]\n");
    return 64;
  }
  Probe& p = gProbe;
  if (!(p.client = squatch::host::openClient("squatch-latency"))) return 2;
  if (!start(p, a)) return jack_client_close(p.client), 2;
  std::this_thread::sleep_for(std::chrono::milliseconds(300));  // let the connections settle
  p.go.store(true);
  while (running().load() && !p.done.load(std::memory_order_acquire)) std::this_thread::sleep_for(std::chrono::milliseconds(20));
  jack_deactivate(p.client);
  if (p.done.load()) report(p, a);
  jack_client_close(p.client);
  return p.done.load() ? 0 : 1;
}
