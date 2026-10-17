// SPDX-License-Identifier: MIT
// seed3-nam: A2-Lite (NAM Core's example model, 3 channels, 23 layers) on the Seed3, alone or beside the chain.
//
//   nam-...        the amp alone: a built-in input -> A2-Lite -> AUDIO OUT 1 and 2
//   nam-chain-...  built-in input -> gate -> drive -> A2-Lite -> limiter -> OUT 1 and 2 (the chain's amp slot)
//   nam-parity-... the amp alone fed a fixed noise (LCG, amplitude 0.3); after the receptive field has been run through
//                  on silence, the board records 4,800 samples of input and output and sends them over USB serial as
//                  float bits ("D <start> <in:out> x 8" lines). tools/nam_check.py compares them with the float64
//                  reference (NAM's tools/a2_reference.py) fed the same input bits.
//
// Variants: SQUATCH_BLOCK; SQUATCH_NAM_CHAIN=1; SQUATCH_NAM_PARITY=1. Once a second the usual cpu and period lines, and
// one line per stage (hosts/daisy/README.md). No heap: the model's ~100 kB of state is static.
#include <new>

#ifndef SQUATCH_NAM_CHAIN
#define SQUATCH_NAM_CHAIN 0
#endif
#ifndef SQUATCH_NAM_PARITY
#define SQUATCH_NAM_PARITY 0
#endif
// BENCH: no audio at all. Warm up, then run the 4,800 recorded samples through the model block by block from the main loop,
// timing every block, and report "# bench: ..." and the recording. Safe for a kernel that is over budget (tools/nam_bench.py).
#ifndef SQUATCH_NAM_BENCH
#define SQUATCH_NAM_BENCH 0
#endif

#if SQUATCH_NAM_CHAIN
#include <squatch/dsp/blocks/gate.hpp>
#include <squatch/dsp/blocks/limiter.hpp>
#endif

#include "../common/gen_signal.hpp"
#include "../common/seed3.hpp"
#include "../common/timed_chain.hpp"
#if SQUATCH_NAM_CHAIN
#include "../faust/drive_block.hpp"
#endif
#include "../nam/a2lite.hpp"

#ifndef NAMOPT_DTCM
#define NAMOPT_DTCM 0
#endif
// NAMOPT_STACK: paint the unused DTCM below the stack when main starts and report the high-water mark ("# stack:" line).
// The bench image always does; the shipping images do not (the code costs flash).
#ifndef NAMOPT_STACK
#define NAMOPT_STACK 0
#endif

extern "C" char _edtcmram_bss, _estack;  // libDaisy's linker script: the end of the DTCM variables, the top of the stack

namespace {

using namespace squatch;
using namespace squatch::board;

constexpr bool kChain = SQUATCH_NAM_CHAIN != 0;
constexpr bool kBench = SQUATCH_NAM_BENCH != 0;
constexpr bool kParity = SQUATCH_NAM_PARITY != 0 || kBench;
constexpr bool kStack = NAMOPT_STACK != 0 || kBench;
constexpr int kMaxStages = 4;
constexpr int kSlots = 2 + kMaxStages;
constexpr int kFirst = Meter<kSlots>::kFirstStage;
constexpr int kRecord = 4800;  // samples recorded for the parity check: 0.1 s

::daisy::DaisySeed hw;
UsbSerial serial;
alignas(Meter<kSlots>) unsigned char meterStorage[sizeof(Meter<kSlots>)];
Meter<kSlots>* meter = nullptr;
TimedChain<Dwt, kMaxStages> chain;
// NAMOPT_DTCM: the whole model object (rings, scratch, weights copy) lives in the 128 KB DTCM, 0 wait states, never cached.
#if NAMOPT_DTCM
__attribute__((section(".dtcmram_bss")))
#endif
nam::A2Lite amp;
#if SQUATCH_NAM_CHAIN
dsp::Gate gate;
DriveStage drive;
dsp::Limiter limiter;
#endif
HotSignal hotSignal(0.5f, kSampleRate);
float srcBuf[256];
alignas(16) unsigned char arenaMemory[4 * 1024];
dsp::Arena arena(arenaMemory, sizeof arenaMemory);

// Parity recording: the audio thread fills it once, the loop sends it.
float recIn[kParity ? kRecord : 1], recOut[kParity ? kRecord : 1];
volatile int recorded = 0;
std::uint32_t lcg = 0x5EED1234u;

float noiseSample() {
  lcg = lcg * 1664525u + 1013904223u;
  return 0.3f * (static_cast<float>(static_cast<double>(lcg) / 4294967296.0 * 2.0 - 1.0));
}

void record(const float* in, const float* out, int n) {
  for (int i = 0; i < n && recorded < kRecord; ++i, recorded = recorded + 1) recIn[recorded] = in[i], recOut[recorded] = out[i];
}

void callback(::daisy::AudioHandle::InputBuffer, ::daisy::AudioHandle::OutputBuffer out, size_t size) {
  const std::uint32_t start = Dwt::now();
  for (size_t i = 0; i < size; ++i) srcBuf[i] = kParity ? noiseSample() : hotSignal.next();
  chain.process(srcBuf, out[0], static_cast<int>(size), meter->stats() + kFirst);
  for (size_t i = 0; i < size; ++i) out[1][i] = out[0][i];
  if (kParity) record(srcBuf, out[0], static_cast<int>(size));
  const std::uint32_t work = Dwt::now() - start;
  meter->endBlock(start, work, peakOf(srcBuf, size), peakOf(out[0], size));
}

bool build() {
#if SQUATCH_NAM_CHAIN
  chain.add(gate, "gate");
  chain.add(drive, "drive");
#endif
  chain.add(amp, "nam");
#if SQUATCH_NAM_CHAIN
  chain.add(limiter, "limiter");
#endif
  chain.prepare(dsp::Setup{48000.0, kBlock}, arena);
#if SQUATCH_NAM_CHAIN
  gate.set(dsp::Gate::Threshold, -120.0f);  // the built-in test signal has silent stretches: keep the gate out of the way
  return !arena.failed() && drive.ready() && limiter.ready();
#else
  return !arena.failed();
#endif
}

// The model's history starts as silence and the conv biases settle through two receptive fields (the reference does the
// same): run them through before the audio starts, so the first recorded sample is in steady state.
// Returns the mean cycles one block took while warming up, so main can refuse to start the audio of an image that cannot
// keep up (a callback over its budget starves the main loop: no serial, no dfu command, and the board needs a button press).
std::uint32_t warmUp() {
  float zeros[kBlock] = {}, sink[kBlock];
  const int blocks = 2 * nam::A2Lite::receptiveField() / kBlock + 1;
  std::uint64_t total = 0;
  for (int i = 0; i < blocks; ++i) {
    const std::uint32_t t0 = Dwt::now();
    chain.process(zeros, sink, kBlock, meter->stats() + kFirst);
    total += Dwt::now() - t0;
    serial.serviceHost();
  }
  return static_cast<std::uint32_t>(total / static_cast<std::uint64_t>(blocks));
}

// Warm up, report the cost of a block, and start the audio only if it fits (90 % of the budget).
std::uint32_t warmCycles = 0;  // kept so the idle loop can repeat it: the boot line is gone before a host connects

bool startIfFast(const BoardInfo& board) {
  const std::uint32_t cycles = warmUp();
  warmCycles = cycles;
  const bool run = cycles < board.budget * 9 / 10;  // zeros cost the same as signal: the model has no data-dependent paths
  serial.line("# warm-up: %lu cycles/block of %lu budget (%lu.%lu%%) -> %s", static_cast<unsigned long>(cycles),
              static_cast<unsigned long>(board.budget), static_cast<unsigned long>(cycles * 100ull / board.budget),
              static_cast<unsigned long>(cycles * 1000ull / board.budget % 10), run ? "starting audio" : "OVER BUDGET, audio stays off");
  if (run) hw.StartAudio(callback);
  return run;
}

// Stack high-water mark. The stack grows down from _estack; everything between the end of the DTCM variables and the stack
// pointer is painted at the start of main (before the board and the model are set up; the few hundred bytes the startup code
// and static constructors used before that are not counted). The mark is the lowest word that no longer holds the paint.
constexpr std::uint32_t kPaint = 0xA5A5A5A5u;

std::uintptr_t stackFloor() { return (reinterpret_cast<std::uintptr_t>(&_edtcmram_bss) + 3u) & ~std::uintptr_t{3}; }

__attribute__((noinline)) void paintStack() {
  const auto top = reinterpret_cast<std::uintptr_t>(__builtin_frame_address(0)) - 128u;  // stay clear of this function's own frame
  for (std::uintptr_t p = stackFloor(); p < top; p += 4) *reinterpret_cast<volatile std::uint32_t*>(p) = kPaint;
}

void stackLine() {
  const std::uintptr_t lo = stackFloor(), hi = reinterpret_cast<std::uintptr_t>(&_estack);
  std::uintptr_t p = lo;
  while (p < hi && *reinterpret_cast<volatile std::uint32_t*>(p) == kPaint) p += 4;
  serial.line("# stack: region=%lu used_hw=%lu never_touched=%lu bytes", static_cast<unsigned long>(hi - lo),
              static_cast<unsigned long>(hi - p), static_cast<unsigned long>(p - lo));
}

// The kernel options this image was compiled with, so tools/nam_bench.py can check that what it asked for is what is running.
void optionsLine() {
  serial.line("# options:%s%s%s%s%s%s", NAMOPT_FUSED ? " FUSED" : "", NAMOPT_DTCM ? " DTCM" : "", NAMOPT_BREAK ? " BREAK" : "",
              NAMOPT_SLIDEH ? " SLIDEH" : "", NAMOPT_STACK ? " STACK" : "",
#ifdef __FAST_MATH__
              " FASTMATH"
#else
              ""
#endif
  );
}

void describe(bool ok) {
  optionsLine();
  if (kStack) stackLine();
  serial.line("# nam: A2-Lite %d weights, receptive field %d, state %u bytes, %s, latency %d samples %s", nam::kA2LiteWeightCount,
              nam::A2Lite::receptiveField(), nam::A2Lite::stateBytes(), kChain ? "gate drive nam limiter" : "alone",
              chain.latency(), ok ? "ready" : "NOT READY: audio off");
}

void reportStages(const Window<kSlots>& w) {
  char a[80], b[80], c[80], d[80];
  const char* none = "";
  formatSummary(a, sizeof a, chain.name(0), w.slot[kFirst]);
  if (chain.size() > 1) formatSummary(b, sizeof b, chain.name(1), w.slot[kFirst + 1]);
  if (chain.size() > 2) formatSummary(c, sizeof c, chain.name(2), w.slot[kFirst + 2]);
  if (chain.size() > 3) formatSummary(d, sizeof d, chain.name(3), w.slot[kFirst + 3]);
  serial.line("w=%lu %s %s %s %s", static_cast<unsigned long>(w.seq), a, chain.size() > 1 ? b : none, chain.size() > 2 ? c : none,
              chain.size() > 3 ? d : none);
}

union Bits {
  float f;
  std::uint32_t u;
};

// The recording as lines of eight "input:output" float-bit pairs; the host script rebuilds the floats.
void dumpRecording() {
  serial.line("DUMP begin %d samples block=%d", kRecord, kBlock);
  for (int i = 0; i < kRecord; i += 8) {
    char line[170];
    int n = std::snprintf(line, sizeof line, "D %d", i);
    for (int k = 0; k < 8 && i + k < kRecord; ++k) {
      Bits a{recIn[i + k]}, b{recOut[i + k]};
      n += std::snprintf(line + n, sizeof line - static_cast<std::size_t>(n), " %08lx:%08lx", static_cast<unsigned long>(a.u),
                         static_cast<unsigned long>(b.u));
    }
    serial.line("%s", line);
  }
  serial.line("DUMP end");
}

// Bench: the cost of each of the 150 recorded blocks (cycles), in steady state, with the cycle counter around process().
struct BenchResult {
  std::uint32_t warm = 0, min = 0, max = 0, mean = 0, blocks = 0;
} bench;

void runBench() {
  float x[kBlock], y[kBlock];
  dsp::DenormalGuard ftz;
  bench.warm = warmUp();
  bench.min = 0xFFFFFFFFu;
  std::uint64_t total = 0;
  for (int i = 0; i + kBlock <= kRecord; i += kBlock) {
    for (int k = 0; k < kBlock; ++k) x[k] = noiseSample();
    const std::uint32_t t0 = Dwt::now();
    amp.process(x, y, kBlock);
    const std::uint32_t c = Dwt::now() - t0;
    record(x, y, kBlock);
    total += c;
    bench.min = c < bench.min ? c : bench.min;
    bench.max = c > bench.max ? c : bench.max;
    ++bench.blocks;
  }
  bench.mean = static_cast<std::uint32_t>(total / bench.blocks);
}

void reportBench(const BoardInfo& board) {
  const auto u = [](std::uint32_t v) { return static_cast<unsigned long>(v); };
  serial.line("# bench: block=%d mean=%lu min=%lu max=%lu warm=%lu blocks=%lu budget=%lu pct=%lu.%lu", kBlock, u(bench.mean), u(bench.min),
              u(bench.max), u(bench.warm), u(bench.blocks), u(board.budget), u(bench.mean * 100ull / board.budget),
              u(bench.mean * 1000ull / board.budget % 10));
}

bool missedDeadline(const Window<kSlots>& w) { return w.late > 0 || w.overruns > 0; }

// Send the recording when it is complete, once, then every 8 s so a host that connects late still gets it.
void maybeDump(std::uint32_t now, std::uint32_t& last, int& dumped) {
  if (!kParity || recorded < kRecord || (dumped != 0 && now - last <= 8000)) return;
  last = now;
  ++dumped;
  dumpRecording();
}

}  // namespace

// While audio is off: once a second, the banner and why.
void reportOverBudget(const BoardInfo& board, bool ok, std::uint32_t now, std::uint32_t& last) {
  if (now - last <= 1000) return;
  last = now, banner(serial, board), describe(ok);
  serial.line("# warm-up: %lu cycles/block of %lu budget (%lu.%lu%%) -> OVER BUDGET, audio stays off",
              static_cast<unsigned long>(warmCycles), static_cast<unsigned long>(board.budget),
              static_cast<unsigned long>(warmCycles * 100ull / board.budget),
              static_cast<unsigned long>(warmCycles * 1000ull / board.budget % 10));
}

// Bench mode: the recording and the result go out once a second; audio never starts, whatever the kernel costs.
[[noreturn]] void benchLoop(const BoardInfo& board, bool ok) {
  std::uint32_t ungated = 0, last = 0, lastBench = 0;
  int dumped = 0;
  for (;;) {
    ungated += ungateUsbPhyClock() ? 1 : 0;
    serial.serviceHost();
    heartbeat(hw, false);
    const std::uint32_t now = ::daisy::System::GetNow();
    if (now - lastBench > 1000) lastBench = now, banner(serial, board), describe(ok), reportBench(board);
    maybeDump(now, last, dumped);
  }
}

void maybeBench(const BoardInfo& board, bool ok) {
  if (!kBench) return;
  if (ok) runBench();
  benchLoop(board, ok);
}

int main() {
  if (kStack) paintStack();
  meter = new (meterStorage) Meter<kSlots>();
  const BoardInfo board = boot(hw, serial);
  meter->configure(board.budget, board.blocksPerSecond);
  serial.line("# boot ok");
  const bool ok = build();
  serial.line("# build %s", ok ? "ok" : "FAILED");
  maybeBench(board, ok);
  const bool run = ok && startIfFast(board);
  Window<kSlots> w;
  std::uint32_t ungated = 0, last = 0;
  bool alarm = !run;
  int dumped = 0;
  for (;;) {
    ungated += ungateUsbPhyClock() ? 1 : 0;
    serial.serviceHost();
    heartbeat(hw, alarm);
    const std::uint32_t now = ::daisy::System::GetNow();
    if (!run) reportOverBudget(board, ok, now, last);
    maybeDump(now, last, dumped);
    if (!meter->take(w)) continue;
    alarm = alarm || missedDeadline(w);
    if (w.seq % 10 == 1) banner(serial, board), describe(ok);
    reportCommon(serial, w, ungated);
    reportStages(w);
  }
}
