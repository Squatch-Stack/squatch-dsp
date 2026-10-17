// SPDX-License-Identifier: MIT
// seed3-latency: round-trip latency measured by the Seed3 itself. Four times a second it writes
// a one-sample click (-6 dBFS) to AUDIO OUT 1 and 2 and records AUDIO IN 1 from kPre samples
// before it; with a wire from AUDIO OUT 1 to AUDIO IN 1 the click comes back after the output
// buffering, the DAC, the ADC and the input buffering: exactly the delay a signal takes from
// the input jack to the output jack of a pedal running a zero-latency effect.
//
// SQUATCH_PATH=1|2 puts the limiter (1), or gate + drive + limiter (2), between the click and AUDIO OUT 1, so the
// loopback measures the round trip of a pedal with that effect on (SQUATCH_LOOKAHEAD sets the limiter's look-ahead
// in samples; default 72). The gate is held open: a 1-sample click out of silence would otherwise be gated.
//
// The callback only records; the loop finds the click (click_detector.hpp) and prints one line
// per click and a summary every 16 (hosts/daisy/README.md). Expected: 2 x SQUATCH_BLOCK samples
// of buffering plus a constant for the converters' filters, so the three block sizes together
// separate the two.
#include <atomic>

#include "../common/click_detector.hpp"
#include "../common/seed3.hpp"

#ifndef SQUATCH_PATH
#define SQUATCH_PATH 0
#endif
#ifndef SQUATCH_LOOKAHEAD
#define SQUATCH_LOOKAHEAD 0
#endif
#if SQUATCH_PATH > 0
#include <squatch/dsp/blocks/gate.hpp>
#include <squatch/dsp/blocks/limiter.hpp>

#include "../faust/drive_block.hpp"
#endif

namespace {

using namespace squatch::board;

constexpr int kInterval = 12000;  // samples between clicks: 250 ms
constexpr int kPre = 256;         // recorded before each click, for the noise floor
constexpr int kPost = 2048;       // recorded after it: 42 ms, far past any plausible latency
constexpr int kCapture = kPre + kPost;
constexpr float kClick = 0.5f;    // -6 dBFS: under the input's 3.6 Vpp with the output at 1 Vrms FS
constexpr int kHistory = 16;

::daisy::DaisySeed hw;
UsbSerial serial;
Meter<2> meter;
float capture[kCapture];
std::atomic<bool> captured{false};  // set by the callback, cleared by the loop
int sinceClick = kInterval - kPre - 1;  // audio thread only: recording starts on the first sample
int writePos = -1;                  // audio thread only: -1 while not recording

#if SQUATCH_PATH > 0
squatch::dsp::Gate gate;
squatch::board::DriveStage drive;
squatch::dsp::Limiter limiter;
alignas(16) unsigned char arenaMemory[16 * 1024];
squatch::dsp::Arena arena(arenaMemory, sizeof arenaMemory);
float clickBuf[256];

bool buildPath() {
  limiter.setLookaheadSamples(SQUATCH_LOOKAHEAD);
  const squatch::dsp::Setup setup{48000.0, kBlock};
  if (SQUATCH_PATH == 2) gate.prepare(setup, arena), drive.prepare(setup, arena);
  limiter.prepare(setup, arena);
  gate.set(squatch::dsp::Gate::Threshold, -120.0f);
  return !arena.failed() && limiter.ready() && (SQUATCH_PATH != 2 || drive.ready());
}
#endif

// One sample of the click schedule. Recording starts kPre samples before the click and only
// when the loop has finished with the last capture.
float step(float in) {
  if (++sinceClick >= kInterval) sinceClick = 0;
  if (sinceClick == kInterval - kPre && !captured.load(std::memory_order_acquire)) writePos = 0;
  if (writePos >= 0) {
    capture[writePos++] = in;
    if (writePos == kCapture) {
      writePos = -1;
      captured.store(true, std::memory_order_release);
    }
  }
  return sinceClick == 0 ? kClick : 0.0f;
}

void callback(::daisy::AudioHandle::InputBuffer in, ::daisy::AudioHandle::OutputBuffer out, size_t size) {
  const std::uint32_t start = Dwt::now();
#if SQUATCH_PATH > 0
  for (size_t i = 0; i < size; ++i) clickBuf[i] = step(in[0][i]);
  if (SQUATCH_PATH == 2) {
    gate.process(clickBuf, clickBuf, static_cast<int>(size));
    drive.process(clickBuf, clickBuf, static_cast<int>(size));
  }
  limiter.process(clickBuf, out[0], static_cast<int>(size));
  for (size_t i = 0; i < size; ++i) out[1][i] = out[0][i];
#else
  for (size_t i = 0; i < size; ++i) out[0][i] = out[1][i] = step(in[0][i]);
#endif
  const float inPeak = peakOf(in[0], size);
  const std::uint32_t work = Dwt::now() - start;
  meter.endBlock(start, work, inPeak, kClick);
}

// "78.42" and the like, without float printf.
const char* fixed2(char* buf, std::size_t size, float v) {
  const long c = std::lround(v * 100.0f);
  const long m = c < 0 ? -c : c;
  std::snprintf(buf, size, "%s%ld.%02ld", c < 0 ? "-" : "", m / 100, m % 100);
  return buf;
}

int pathLatency = 0;  // samples the limiter / chain in the path adds (SQUATCH_PATH); 0 for the bare loopback

struct History {
  float lags[kHistory] = {};
  int count = 0, clicks = 0, missed = 0;
};

void reportClick(const ClickResult& r, History& h, float rate) {
  ++h.clicks;
  char lag[24], ms[24], pk[24], noise[24];
  if (!r.found) {
    ++h.missed;
    serial.line("click %d: NOT FOUND (noise %s dBFS) - is AUDIO OUT 1 wired to AUDIO IN 1?", h.clicks,
                formatDb(noise, sizeof noise, r.noiseRms));
    return;
  }
  serial.line("click %d: lag=%s samples (%s ms) onset=%d peak=%s dBFS %s noise=%s dBFS", h.clicks,
              fixed2(lag, sizeof lag, r.lag), fixed2(ms, sizeof ms, 1000.0f * r.lag / rate), r.onset,
              formatDb(pk, sizeof pk, std::fabs(r.peak)), r.peak < 0.0f ? "inverted" : "upright",
              formatDb(noise, sizeof noise, r.noiseRms));
  h.lags[h.count++ % kHistory] = r.lag;
}

void reportSummary(const History& h, float rate) {
  const int n = h.count < kHistory ? h.count : kHistory;
  float lo = h.lags[0], hi = h.lags[0];
  for (int i = 1; i < n; ++i) lo = std::fmin(lo, h.lags[i]), hi = std::fmax(hi, h.lags[i]);
  const float med = median(h.lags, n);
  char m[24], ms[24], spread[24], conv[24];
  serial.line("LATENCY block=%d median=%s samples = %s ms spread=%s samples over %d clicks (missed %d) converters=%s samples",
              kBlock, fixed2(m, sizeof m, med), fixed2(ms, sizeof ms, 1000.0f * med / rate),
              fixed2(spread, sizeof spread, hi - lo), n, h.missed, fixed2(conv, sizeof conv, med - 2.0f * kBlock - static_cast<float>(pathLatency)));
}


// Starts the audio (after building the effect path, if this image has one). False: the path did not fit, audio stays off.
bool startAudio() {
#if SQUATCH_PATH > 0
  const bool pathOk = buildPath();
  pathLatency = limiter.latency();
  if (pathOk) hw.StartAudio(callback);
  return pathOk;
#else
  hw.StartAudio(callback);
  return true;
#endif
}

// What the once-in-ten-windows banner says besides the common lines.
void describeRun(const BoardInfo& board, bool pathOk) {
  banner(serial, board);
#if SQUATCH_PATH > 0
  serial.line("# path: %s latency=%d samples (limiter lookahead %d) %s", SQUATCH_PATH == 2 ? "gate drive limiter" : "limiter",
              limiter.latency(), SQUATCH_LOOKAHEAD > 0 ? SQUATCH_LOOKAHEAD : 72, pathOk ? "ready" : "NOT READY: audio off");
#else
  (void)pathOk;
#endif
  serial.line("# expect lag = 2 x %d samples of buffering + the converters' filter delay + the path's %d", kBlock, pathLatency);
}

void serviceClicks(History& h, float rate) {
  if (!captured.load(std::memory_order_acquire)) return;
  reportClick(analyzeClick(capture, kCapture, kPre), h, rate);
  captured.store(false, std::memory_order_release);
  if (h.clicks % kHistory == 0 && h.count > 0) reportSummary(h, rate);
}

}  // namespace

int main() {
  const BoardInfo board = boot(hw, serial);
  meter.configure(board.budget, board.blocksPerSecond);
  const bool pathOk = startAudio();
  Window<2> w;
  History h;
  std::uint32_t ungated = 0;
  for (;;) {
    ungated += ungateUsbPhyClock() ? 1 : 0;
    serial.serviceHost();
    heartbeat(hw, h.missed > 0);
    serviceClicks(h, board.sampleRate);
    if (!meter.take(w)) continue;
    if (w.seq % 10 == 1) describeRun(board, pathOk);
    reportCommon(serial, w, ungated);
  }
}
