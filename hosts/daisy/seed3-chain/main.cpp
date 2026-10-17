// SPDX-License-Identifier: MIT
// seed3-chain: AUDIO IN 1 -> gate -> drive -> limiter -> AUDIO OUT 1 and 2, the squatch-dsp
// blocks and a Faust-generated drive on the Seed3, with the DWT cycle counter read around every
// block. Once a second USB serial reports mean/p99/worst per block and for the whole callback,
// as a share of the block's budget (hosts/daisy/README.md).
//
// Variants (the Makefile builds each):
//   SQUATCH_BLOCK=32|48|64   samples per callback
//   SQUATCH_HOT=1            libDaisy #656 check: +24 dB trim in front, gate held open, drive
//                            level up, so noise at the callback rate is audible and measurable
//   SQUATCH_LOOKAHEAD=N      the limiter's look-ahead in samples (default 72 = 1.5 ms); latency becomes N + 16
//   SQUATCH_LIMONLY=1        only trim -> limiter, so the limiter alone takes the hot signal (SQUATCH_TRIM_DB, default 24)
//   SQUATCH_GEN=1            the input is a built-in hot test signal (gen_signal.hpp), not AUDIO IN 1
//   SQUATCH_BURN_PCT=N       a last stage that busy-waits N% of the budget (#656's ripple
//                            above ~50% CPU)
// No heap: every block is static, and the arena is a static array.
#include <new>
#include <squatch/dsp/blocks/gate.hpp>
#include <squatch/dsp/blocks/limiter.hpp>

#include "../common/bench_blocks.hpp"
#include "../common/gen_signal.hpp"
#include "../common/seed3.hpp"
#include "../common/timed_chain.hpp"
#include "../faust/drive_block.hpp"

#ifndef SQUATCH_HOT
#define SQUATCH_HOT 0
#endif
#ifndef SQUATCH_LOOKAHEAD
#define SQUATCH_LOOKAHEAD 0
#endif
#ifndef SQUATCH_LIMONLY
#define SQUATCH_LIMONLY 0
#endif
#ifndef SQUATCH_TRIM_DB
#define SQUATCH_TRIM_DB 24
#endif
#ifndef SQUATCH_GEN
#define SQUATCH_GEN 0
#endif
#ifndef SQUATCH_BURN_PCT
#define SQUATCH_BURN_PCT 0
#endif

namespace {

using namespace squatch;
using namespace squatch::board;

constexpr bool kHot = SQUATCH_HOT != 0;
constexpr std::uint32_t kBurnPct = SQUATCH_BURN_PCT;
constexpr int kLookahead = SQUATCH_LOOKAHEAD;
constexpr bool kGen = SQUATCH_GEN != 0;
constexpr int kMaxStages = 5;  // trim, gate, drive, limiter, burn
constexpr int kSlots = 2 + kMaxStages;
constexpr int kFirst = Meter<kSlots>::kFirstStage;

::daisy::DaisySeed hw;
UsbSerial serial;
// The meter is ~15 KB with non-zero defaults: as a plain global its initial image sat in .data and cost that much
// of the 128 KB flash (the chain stopped fitting). Built in place at start-up instead, it lives in .bss.
alignas(Meter<kSlots>) unsigned char meterStorage[sizeof(Meter<kSlots>)];
Meter<kSlots>* meter = nullptr;
TimedChain<Dwt, kMaxStages> chain;
Trim trim(static_cast<float>(SQUATCH_TRIM_DB));
dsp::Gate gate;
DriveStage drive;
dsp::Limiter limiter;
HotSignal hotSignal(0.5f, kSampleRate);  // -6 dBFS peaks (+18 dBFS after the hot trim)
float genBuf[256];
volatile float minLimiterGain = 1.0f;  // the audio thread writes, the loop reads and resets
Burn<Dwt> burn;
alignas(16) unsigned char arenaMemory[16 * 1024];
dsp::Arena arena(arenaMemory, sizeof arenaMemory);

void callback(::daisy::AudioHandle::InputBuffer in, ::daisy::AudioHandle::OutputBuffer out, size_t size) {
  const std::uint32_t start = Dwt::now();
  const float* src = in[0];
  if (kGen) {
    for (size_t i = 0; i < size; ++i) genBuf[i] = hotSignal.next();
    src = genBuf;
  }
  chain.process(src, out[0], static_cast<int>(size), meter->stats() + kFirst);
  if (limiter.gain() < minLimiterGain) minLimiterGain = limiter.gain();
  for (size_t i = 0; i < size; ++i) out[1][i] = out[0][i];
  const float inPeak = peakOf(src, size), outPeak = peakOf(out[0], size);
  const std::uint32_t work = Dwt::now() - start;
  meter->endBlock(start, work, inPeak, outPeak);
}

// Normal: gate at -60 dB, drive at its defaults (drive 1, tone 0.5, level 0.5), limiter at -1 dB.
bool build(std::uint32_t budget) {
  if (SQUATCH_LIMONLY) {
    chain.add(trim, "trim");
    chain.add(limiter, "limiter");
    limiter.setLookaheadSamples(kLookahead);
    chain.prepare(dsp::Setup{48000.0, kBlock}, arena);
    return !arena.failed() && limiter.ready();
  }
  if (kHot) chain.add(trim, "trim");
  chain.add(gate, "gate");
  chain.add(drive, "drive");
  chain.add(limiter, "limiter");
  if (kBurnPct > 0) {
    burn.setCycles(budget / 100 * kBurnPct);
    chain.add(burn, "burn");
  }
  limiter.setLookaheadSamples(kLookahead);
  chain.prepare(dsp::Setup{48000.0, kBlock}, arena);
  gate.set(dsp::Gate::Threshold, kHot ? -120.0f : -60.0f);
  drive.set(DriveStage::Level, kHot ? 1.0f : 0.5f);
  return !arena.failed() && drive.ready() && limiter.ready();
}

void describe(bool ok) {
  char names[64] = "";
  for (int i = 0; i < chain.size(); ++i) std::snprintf(names + std::strlen(names), sizeof names - std::strlen(names), " %s", chain.name(i));
  serial.line("# chain:%s%s latency=%d samples (limiter lookahead %d, trim %d dB) arena=%lu/%lu bytes %s", kGen ? " gen" : "", names,
              chain.latency(), kLookahead > 0 ? kLookahead : 72, SQUATCH_TRIM_DB, static_cast<unsigned long>(arena.used()),
              static_cast<unsigned long>(arena.capacity()), ok ? "ready" : "NOT READY: audio stays off");
}

void reportStages(const Window<kSlots>& w) {
  char a[80], b[80], c[80];
  const char* none = "";
  for (int i = 0; i < chain.size(); i += 3) {
    formatSummary(a, sizeof a, chain.name(i), w.slot[kFirst + i]);
    if (i + 1 < chain.size()) formatSummary(b, sizeof b, chain.name(i + 1), w.slot[kFirst + i + 1]);
    if (i + 2 < chain.size()) formatSummary(c, sizeof c, chain.name(i + 2), w.slot[kFirst + i + 2]);
    serial.line("w=%lu %s %s %s", static_cast<unsigned long>(w.seq), a, i + 1 < chain.size() ? b : none,
                i + 2 < chain.size() ? c : none);
  }
}

// The limiter's own view: the most gain reduction this window, how many samples its final clamp had to cut (0 = the
// gain stage held the ceiling alone), and the loudest output since power-up against the -1 dBFS ceiling.
void reportLimiter(const Window<kSlots>& w) {
  static float maxOut = 0.0f;
  maxOut = std::fmax(maxOut, w.outPeak);
  const float minGain = minLimiterGain;
  minLimiterGain = 1.0f;
  const long tenths = std::lround(200.0f * std::log10(std::fmax(minGain, 1e-6f)));
  char peak[24];
  serial.line("w=%lu limiter reduction=%ld.%ld dB clamp_hits=%lu max_out_since_boot=%s dBFS (ceiling -1.0)",
              static_cast<unsigned long>(w.seq), -tenths / 10, (tenths < 0 ? -tenths : tenths) % 10,
              static_cast<unsigned long>(limiter.clampHits()), formatDb(peak, sizeof peak, maxOut));
}

}  // namespace

int main() {
  meter = new (meterStorage) Meter<kSlots>();
  const BoardInfo board = boot(hw, serial);
  meter->configure(board.budget, board.blocksPerSecond);
  const bool ok = build(board.budget);
  if (ok) hw.StartAudio(callback);
  Window<kSlots> w;
  std::uint32_t ungated = 0, lastBanner = 0;
  bool alarm = !ok;
  for (;;) {
    ungated += ungateUsbPhyClock() ? 1 : 0;
    serial.serviceHost();
    heartbeat(hw, alarm);
    const std::uint32_t now = ::daisy::System::GetNow();
    if (!ok && now - lastBanner > 1000) lastBanner = now, banner(serial, board), describe(ok);
    if (!meter->take(w)) continue;
    alarm = alarm || w.late > 0 || w.overruns > 0;
    if (w.seq % 10 == 1) banner(serial, board), describe(ok);
    reportCommon(serial, w, ungated);
    reportStages(w);
    reportLimiter(w);
  }
}
