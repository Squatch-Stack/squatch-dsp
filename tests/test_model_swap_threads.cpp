// SPDX-License-Identifier: MIT
// ModelSwap's handoff across two threads, under ThreadSanitizer (make test-tsan) and under ASan and
// UBSan: a control thread loads a new Cabinet response into the standby again and again, the
// audio thread plays all the while. Nothing may race, every output must come from a whole model
// (a torn load shows as a sample the models cannot produce), and the swaps must all complete.
// make test-tsan-negative builds it with the handoff's memory ordering weakened on purpose
// (SQUATCH_BREAK_HANDOFF) and requires that ThreadSanitizer then reports a race.
#include <atomic>
#include <cmath>
#include <cstdio>
#include <squatch/dsp/blocks/cabinet.hpp>
#include <squatch/dsp/blocks/model_swap.hpp>
#include <thread>
#include <vector>

#include "signals.hpp"

using namespace squatch::dsp;

namespace {

constexpr double kRate = 48000.0;
constexpr int kMaxBlock = 128;
constexpr int kSwaps = 400;
constexpr int kTaps = 96;
alignas(64) unsigned char gMem[1 << 20], gMemVoice[2][1 << 18];

// Model k is a 96-tap response whose every tap carries gain(k): a torn or half-visible load
// would mix gains, and the output could not stay inside the band the gains allow.
float gainOf(int k) { return 0.25f + 0.05f * float(k % 11); }
constexpr float kMaxGain = 0.25f + 0.05f * 10;  // gainOf's top

struct Rig {
  Cabinet a, b;
  ModelSwap swap{a, b, 200};
  std::atomic<bool> done{false};
  std::atomic<int> published{0};
  std::atomic<int> lastPublished{-1};
  long blocks = 0;
  float worst = 0.0f;  // the largest |out| / |in| seen: bounded by the loudest model if loads are whole
  bool finite = true;

  void load(int k, Cabinet& into, Arena& mem) {
    std::vector<float> h(kTaps, 0.0f);
    h[0] = gainOf(k);  // a gain, with a few taps of body so the convolver has real work
    h[kTaps - 1] = gainOf(k) * 0.01f;
    into.configure(h.data(), kTaps, Cabinet::kDefaultPartition, false);
    into.prepare(Setup{kRate, kMaxBlock}, mem);
  }
};

void control(Rig& r) {
  for (int k = 1; k <= kSwaps; ++k) {
    Block* s = nullptr;
    while (!(s = r.swap.standby())) std::this_thread::yield();
    if (k % 9 == 0) {  // a load that is given up
      r.swap.cancel();
      continue;
    }
    Cabinet& c = *static_cast<Cabinet*>(s);
    Arena mem(gMemVoice[&c == &r.a ? 0 : 1], sizeof gMemVoice[0]);  // each block keeps its own arena
    r.load(k, c, mem);
    r.published.fetch_add(1, std::memory_order_relaxed);
    r.lastPublished.store(k, std::memory_order_relaxed);
    r.swap.publish(k % 4 == 0 ? 0 : 300);
  }
  while (!r.swap.idle()) std::this_thread::yield();
  r.done.store(true, std::memory_order_release);
}

void audio(Rig& r) {
  sig::Rng rng{12345u};
  float in[kMaxBlock], out[kMaxBlock];
  while (!r.done.load(std::memory_order_acquire) || !r.swap.idle()) {
    const int n = rng.range(1, kMaxBlock);
    for (int i = 0; i < n; ++i) in[i] = rng.uniform() * 0.5f;
    if (r.blocks % 37 == 0) r.swap.set(Cabinet::Level, 0.0f);  // parameters arrive on the audio thread too
    r.swap.process(in, out, n);
    ++r.blocks;
    for (int i = 0; i < n; ++i) {
      r.finite = r.finite && std::isfinite(out[i]);
      r.worst = std::fmax(r.worst, std::fabs(out[i]));
    }
  }
}

}  // namespace

int main() {
  Rig r;
  Arena mem(gMem, sizeof gMem);
  std::vector<float> h(kTaps, 0.0f);
  h[0] = gainOf(0);
  r.a.configure(h.data(), kTaps, Cabinet::kDefaultPartition, false);  // both blocks get a response before the swap prepares them
  r.b.configure(h.data(), kTaps, Cabinet::kDefaultPartition, false);
  r.swap.prepare(Setup{kRate, kMaxBlock}, mem);
  std::thread ctl(control, std::ref(r));
  std::thread aud(audio, std::ref(r));
  ctl.join();
  aud.join();
  // 0.5 input peak, the loudest model's gain, and the tail tap and the fade's own overshoot as slack.
  const float bound = 0.5f * kMaxGain * 1.2f;
  int bad = 0;
  if (!r.finite) std::printf("model swap threads: non-finite output\n"), ++bad;
  if (r.worst > bound) std::printf("model swap threads: output %.4f above the %.4f any whole model allows\n", r.worst, bound), ++bad;
  if (r.swap.swaps() != r.published.load()) {
    std::printf("model swap threads: %d published, %ld completed\n", r.published.load(), r.swap.swaps());
    ++bad;
  }
  if (!r.swap.idle()) std::printf("model swap threads: not idle at the end\n"), ++bad;
  if (!bad) std::printf("model swap threads: %ld swaps, %ld blocks, worst output %.3f, clean\n", r.swap.swaps(), r.blocks, r.worst);
  return bad ? 1 : 0;
}
