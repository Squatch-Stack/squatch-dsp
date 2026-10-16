// SPDX-License-Identifier: MIT
// Meters, and what happens across threads: the main thread reading and loading state while the
// audio thread processes (the ThreadSanitizer leg's subject), and a file swapped in the background.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdlib>
#include <string>
#include <thread>
#include <unistd.h>

#include "cases.hpp"
#include "wav.hpp"

using host::Audio;
using host::Events;
using host::Host;
using host::Plugin;

namespace cases {
namespace {

constexpr double kPi = 3.14159265358979323846;

void sine(Audio& a, double hz, double amp, long& phase, uint32_t n) {
  for (uint32_t i = 0; i < n; ++i, ++phase)
    a.in[0][i] = a.in[1][i] = static_cast<float>(amp * std::sin(2 * kPi * hz * phase / 48000.0));
}

void runSine(const Plugin& pl, double hz, double amp, int buffers, Events& sent) {
  long phase = 0;
  for (int n = 0; n < buffers; ++n) {
    Audio a(256, 2, 2, false);
    sine(a, hz, amp, phase, 256);
    Events none;
    a.process.in_events = &none.in;
    a.process.out_events = &sent.out;
    processWatched(pl.p, &a.process);
  }
}

double meter(const Plugin& pl, const char* name) {
  clap_param_info_t i{};
  return pl.find(name, i) ? pl.value(i.id) : -1e9;
}

std::string withFile(std::string state, const std::string& path) {
  return state.insert(state.rfind("end\n"), "file ir " + path + "\n");
}

// The cabinet always has its low cut (a 2nd-order high-pass, 70 Hz by default) and high cut (low-pass, 9 kHz)
// in the path, so a constant input is blocked and the level is read from a 220 Hz sine instead.
constexpr double kToneHz = 220.0;
long gPhase = 0;  // the sine continues from one level() call to the next

// |H| of one of the cabinet's cuts (RBJ biquad, Q 0.7 dB as WebAudio defines it) at `hz`, at 48 kHz.
double cutGain(bool highPass, double cutHz, double hz) {
  const double w0 = 2 * kPi * cutHz / 48000.0, cw = std::cos(w0), al = std::sin(w0) / (2.0 * std::pow(10.0, 0.7 / 20.0));
  const double b = highPass ? (1.0 + cw) / 2.0 : (1.0 - cw) / 2.0, b1 = highPass ? -(1.0 + cw) : 1.0 - cw;
  const std::complex<double> z1 = std::polar(1.0, -2 * kPi * hz / 48000.0), z2 = z1 * z1;
  return std::abs((b + b1 * z1 + b * z2) / ((1.0 + al) - 2.0 * cw * z1 + (1.0 - al) * z2));
}

// The 0.25-amplitude sine's settled level behind a response of gain `ir` at the tone (1 for a single tap).
float settled(double ir) { return static_cast<float>(0.25 * ir * cutGain(true, 70.0, kToneHz) * cutGain(false, 9000.0, kToneHz)); }

// A sine of 0.25 through one buffer: its peak, or -1 when the output has a discontinuity (a second difference
// beyond what a 0.4-amplitude 220 Hz sine can make by itself): a click is not a level.
float level(const Plugin& pl) {
  Audio a(256, 2, 2, false);
  for (int i = 0; i < 256; ++i, ++gPhase)
    a.in[0][i] = a.in[1][i] = static_cast<float>(0.25 * std::sin(2 * kPi * kToneHz * gPhase / 48000.0));
  Events none;
  a.process.in_events = &none.in;
  a.process.out_events = &none.out;
  processWatched(pl.p, &a.process);
  float bend = 0.0f, peak = 0.0f;
  for (int i = 0; i < 256; ++i) {
    peak = std::fmax(peak, std::fabs(a.out[0][i]));
    if (i >= 2) bend = std::fmax(bend, std::fabs(a.out[0][i] - 2.0f * a.out[0][i - 1] + a.out[0][i - 2]));
  }
  return bend > 0.002f ? -1.0f : peak;
}

// Processes until the output settles at `want` (the loader thread is working meanwhile).
bool reaches(const Plugin& pl, float want, int& clicks) {
  const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  int near = 0;  // in a row: the start-up transient of the cuts must not pass through the target and count
  while (std::chrono::steady_clock::now() < until) {
    const float v = level(pl);
    clicks += v < 0.0f;
    near = std::fabs(v - want) < 1e-4f ? near + 1 : 0;
    if (near >= 20) return true;
  }
  return false;
}

// Runs until the host has been told the file could not be loaded, and the old response is still the one playing.
bool reportsAndKeeps(Host& h, const Plugin& pl, float want, int& clicks) {
  const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!h.logged("cannot load") && std::chrono::steady_clock::now() < until)
    if (level(pl), h.callbacks.exchange(0) > 0) pl.p->on_main_thread(pl.p);
  return h.logged("cannot load") && reaches(pl, want, clicks);
}

}  // namespace

// Meters reach the host as read-only parameter values, both through get_value and as output
// events at the contract's rate: a meter that moved was sent. (A block whose meters wait for
// something to happen first, such as a learn, has its own case in an extra_*.cpp file.)
void metersReachTheHost(const Subject& s) {
  Host h;
  Plugin pl(s.entry, h);
  pl.p->activate(pl.p, 48000, 1, 256);
  pl.p->start_processing(pl.p);
  Events sent;
  const double hz = 110.0 * std::exp2(13.0 / 1200.0);
  runSine(pl, hz, std::string(s.key) == "limiter" ? 1.5 : 0.3, 150, sent);
  int meters = 0, moved = 0;
  for (uint32_t k = 0; k < pl.count(); ++k) {
    const clap_param_info_t i = pl.info(k);
    if (!(i.flags & CLAP_PARAM_IS_READONLY)) continue;
    ++meters;
    moved += pl.value(i.id) != i.default_value;
  }
  HCHECK(moved == 0 || !sent.sent.empty(), "%s: %d meters, %d moved, %zu value events sent", s.key, meters, moved,
         sent.sent.size());
  if (std::string(s.key) == "tuner")
    HCHECK(meter(pl, "Note") == 9 && meter(pl, "Octave") == 2 && std::fabs(meter(pl, "Cents") - 13) < 0.5,
           "tuner: A2 +13 c, read %g/%g %+.2f c", meter(pl, "Note"), meter(pl, "Octave"), meter(pl, "Cents"));
  if (std::string(s.key) == "limiter") HCHECK(meter(pl, "Gain reduction") > 3, "limiter: reducing %.2f dB", meter(pl, "Gain reduction"));
  if (std::string(s.key) == "gate") HCHECK(meter(pl, "Open") == 1, "gate: open on a note");
  pl.p->deactivate(pl.p);
}

// The main thread reads values, saves and loads state while the audio thread processes.
void mainThreadWhileProcessing(const Subject& s) {
  Host h;
  Plugin pl(s.entry, h);
  pl.p->activate(pl.p, 48000, 1, 256);
  const std::string saved = pl.save(64);
  std::atomic<bool> done{false};
  std::thread audio([&] {
    pl.p->start_processing(pl.p);
    Events sent;
    for (int n = 0; n < 40; ++n) runSine(pl, 220.0, 0.5, 10, sent), sent.clear();
    pl.p->stop_processing(pl.p);
    done = true;
  });
  int loads = 0;
  while (!done) {
    for (uint32_t k = 0; k < pl.count(); ++k) {
      char text[64];
      const clap_param_info_t i = pl.info(k);
      pl.params->value_to_text(pl.p, i.id, pl.value(i.id), text, sizeof text);
    }
    loads += pl.load(pl.save(16), 16) && pl.load(saved, 16);
  }
  audio.join();
  HCHECK(loads > 0 && pl.save(64) == saved, "%s: %d state round trips during processing", s.key, loads);
  pl.p->deactivate(pl.p);
}

// The cabinet's response changes while audio runs: loaded on the plugin's own thread, then a
// click-free crossfade. A file that cannot be read keeps the current response and is reported.
void fileSwapsOffThread(const Subject& s) {
  if (std::string(s.key) != "cabinet") return;
  const char* tmp = std::getenv("TMPDIR");
  const std::string dir = (tmp && *tmp ? tmp : "/tmp") + std::string("/squatch-clap-") + std::to_string(getpid());
  const std::string a = dir + "-a.wav", b = dir + "-b.wav";
  HCHECK(host::writeWav(a, {1.0f}, 48000) && host::writeWav(b, {1.0f, 1.0f}, 48000), "cabinet: test responses");
  Host h;
  Plugin pl(s.entry, h);
  gPhase = 0;
  const std::string base = pl.save(64);
  HCHECK(pl.load(withFile(base, a), 5), "cabinet: a state naming a response, before activate");
  pl.p->activate(pl.p, 48000, 1, 256);
  pl.p->start_processing(pl.p);
  for (int i = 0; i < 8; ++i) level(pl);  // the sine's own first samples bend (it starts at 0): not a click
  int clicks = 0;
  HCHECK(reaches(pl, settled(1.0), clicks), "cabinet: the first response loads at activate");
  pl.load(withFile(base, b), 5);
  const float swapped = settled(std::sqrt(2.0) * std::cos(kPi * kToneHz / 48000.0));  // two taps of 1/sqrt(2)
  HCHECK(reaches(pl, swapped, clicks) && clicks == 0, "cabinet: swapped in the background, %d clicks", clicks);
  pl.load(withFile(base, dir + "-missing.wav"), 5);
  HCHECK(reportsAndKeeps(h, pl, swapped, clicks), "cabinet: a missing file is reported, the old response kept");
  pl.p->deactivate(pl.p);
  std::remove(a.c_str()), std::remove(b.c_str());
}

}  // namespace cases
