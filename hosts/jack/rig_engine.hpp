// SPDX-License-Identifier: MIT
// The live rig's engine: one mono input through a fixed chain of stages to one output port,
// with every stage timed on every callback.
//
//   capture -> gate -> tuner (measures, passes through) -> compressor -> overdrive -> Claw (distortion) -> Crunch
//           -> amp (NAM) -> EQ -> cabinet -> hall -> limiter -> out
//
// A stage runs when it is available (its block could be set up: the amp needs a model, the
// cabinet a response) and switched on. Switching is immediate, not crossfaded.
//
// The JACK callback (process) is real-time safe: no allocation, locks, system calls or I/O. It
// hands one Timing record per callback to the telemetry thread through a lock-free queue, and
// takes parameter changes from the command thread through another.
#pragma once
#include <jack/jack.h>

#include <atomic>
#include <cstdint>
#include <squatch/dsp/blocks/cabinet.hpp>
#include <squatch/dsp/blocks/comp.hpp>
#include <squatch/dsp/blocks/crunch.hpp>
#include <squatch/dsp/blocks/hall.hpp>
#include <squatch/dsp/blocks/claw.hpp>
#include <squatch/dsp/blocks/trail_od.hpp>
#include <squatch/dsp/blocks/eq.hpp>
#include <squatch/dsp/blocks/gate.hpp>
#include <squatch/dsp/blocks/limiter.hpp>
#include <squatch/dsp/blocks/tuner.hpp>
#include <squatch/dsp/denormal.hpp>
#include <squatch/dsp/param_queue.hpp>

#include "jack_session.hpp"
#include "nam_amp.hpp"
#include "rig_options.hpp"
#include "rig_stages.hpp"

namespace squatch::rig {

using squatch::dsp::Block;
using squatch::dsp::ParamDesc;
using squatch::dsp::Span;

/// One callback: nanoseconds in each stage (0 when it did not run), the whole callback, and
/// the input and output peaks, for gain staging.
struct Timing {
  std::uint32_t ns[kStages] = {};
  std::uint32_t totalNs = 0;
  float inPeak = 0.0f, outPeak = 0.0f;
};

struct Slot {
  Block* block = nullptr;
  Span<const ParamDesc> params;
  bool available = false;
  std::atomic<bool> on{false};
};

constexpr std::uint16_t kSwitch = 0xFFFF;  // a ParamChange with this id switches a stage on (1) or off (0)

struct Engine {
  jack_client_t* client = nullptr;
  jack_port_t* in = nullptr;
  jack_port_t* out = nullptr;
  int maxBlock = 0;
  float* work = nullptr;  // from the arena

  squatch::dsp::Gate gate;
  extra::GateStage gateExtra;  // replaces the stock gate when an option selects it
  squatch::dsp::TunerBlock tuner;
  squatch::dsp::Compressor comp;
  squatch::dsp::TrailOD od;
  squatch::dsp::Claw claw;
  squatch::dsp::Crunch crunch;
  squatch::dsp::Hall hall;
#ifdef SQUATCH_WITH_NAM
  squatch::host::NamAmp amp;
#endif
  squatch::dsp::Cabinet cab;
  squatch::dsp::Eq eq;
  squatch::dsp::Limiter limiter;
  Slot slots[kStages];

  squatch::dsp::SpscQueue<Timing, 8192> timings;  // ~11 s of callbacks at 64 frames
  squatch::dsp::ParamQueue<256> changes;
  std::atomic<std::uint32_t> xruns{0}, dropped{0}, oversize{0};
  std::atomic<bool> muted{false};  // the output only; the chain and the tuner keep running
  std::atomic<float> inHold{0.0f}, outHold{0.0f};  // peaks since the stream last read them
  std::atomic<std::uint64_t> callbacks{0};
};

// ---------------------------------------------------------------- realtime ----

inline void applyChanges(Engine& e) {
  squatch::dsp::ParamChange c;
  while (e.changes.pop(c)) {
    if (c.target >= kStages || !e.slots[c.target].block) continue;
    if (c.id == kSwitch) e.slots[c.target].on.store(c.value > 0.5f, std::memory_order_relaxed);
    else e.slots[c.target].block->set(c.id, c.value);
  }
}

inline void runStages(Engine& e, int n, Timing& t, std::uint64_t start) {
  std::uint64_t prev = start;
  for (int s = 0; s < kStages; ++s) {
    Slot& slot = e.slots[s];
    if (!slot.available || !slot.on.load(std::memory_order_relaxed)) continue;
    slot.block->process(e.work, e.work, n);
    const std::uint64_t now = squatch::host::nowNs();
    t.ns[s] = static_cast<std::uint32_t>(now - prev);
    prev = now;
  }
}

/// Raises `hold` to `v` if it is lower: lock-free, for the peaks the stream thread takes.
inline void holdPeak(std::atomic<float>& hold, float v) {
  float prev = hold.load(std::memory_order_relaxed);
  while (v > prev && !hold.compare_exchange_weak(prev, v, std::memory_order_relaxed)) {
  }
}

inline int process(jack_nframes_t frames, void* arg) {
  Engine& e = *static_cast<Engine*>(arg);
  const std::uint64_t t0 = squatch::host::nowNs();
  const int n = static_cast<int>(frames);
  const auto* in = static_cast<const float*>(jack_port_get_buffer(e.in, frames));
  auto* out = static_cast<float*>(jack_port_get_buffer(e.out, frames));
  e.callbacks.fetch_add(1, std::memory_order_relaxed);
  if (n > e.maxBlock || !e.work) {
    e.oversize.fetch_add(1, std::memory_order_relaxed);
    squatch::dsp::clear(out, n);
    return 0;
  }
  squatch::dsp::DenormalGuard ftz;
  Timing t;
  applyChanges(e);
  squatch::dsp::copy(in, e.work, n);
  t.inPeak = squatch::dsp::peakAbs(in, n);
  runStages(e, n, t, squatch::host::nowNs());
  t.outPeak = squatch::dsp::peakAbs(e.work, n);
  if (e.muted.load(std::memory_order_relaxed)) squatch::dsp::clear(out, n);
  else squatch::dsp::copy(e.work, out, n);
  holdPeak(e.inHold, t.inPeak);
  holdPeak(e.outHold, t.outPeak);
  t.totalNs = static_cast<std::uint32_t>(squatch::host::nowNs() - t0);
  if (!e.timings.push(t)) e.dropped.fetch_add(1, std::memory_order_relaxed);
  return 0;
}

inline int onXrun(void* arg) {
  static_cast<Engine*>(arg)->xruns.fetch_add(1, std::memory_order_relaxed);
  return 0;
}

/// Samples of delay the running stages add (the limiter's lookahead; the rest add none).
inline int algorithmicLatency(const Engine& e) {
  int total = 0;
  for (const Slot& s : e.slots)
    if (s.available && s.on.load()) total += s.block->latency();
  return total;
}

}  // namespace squatch::rig
