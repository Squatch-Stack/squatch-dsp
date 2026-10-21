// SPDX-License-Identifier: MIT
// CabSwap keeps its own copy of every response it is given (review S2), and ModelSwap::publish
// refuses a block whose delay differs from the playing block's (S7, which CabSwap and NamAmp now
// act on). Built with AddressSanitizer by `make test`: the first test frees the caller's buffer and
// then prepares again, so a CabSwap that kept the caller's pointer is a heap-use-after-free, and
// fails (negative control done by hand: restoring `source_ = ir` makes it abort).
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "../../tests/check.hpp"
#include "../common/cab_swap.hpp"

using namespace squatch::dsp;
using squatch::host::CabSwap;

namespace {

constexpr int kBlock = 128;

std::vector<float> impulse(float gain, int taps) {
  std::vector<float> h(static_cast<std::size_t>(taps), 0.0f);
  h[0] = gain;
  return h;
}

void prepare(CabSwap& cs, std::unique_ptr<unsigned char[]>& memory) {
  const Setup s{48000.0, kBlock};
  Arena measure;
  cs.prepare(s, measure);
  const std::size_t bytes = measure.required() + 64 * Arena::kAlign;
  memory.reset(new unsigned char[bytes]);
  Arena mem(memory.get(), bytes);
  cs.prepare(s, mem);
}

// One block of an impulse; the response's first tap is what comes out first.
float firstSample(CabSwap& cs) {
  float in[kBlock] = {1.0f}, out[kBlock];
  cs.process(in, out, kBlock);
  return out[0];
}

}  // namespace

TEST(cabSwapOwnsItsTapsAfterTheCallersBufferIsFreed) {
  CabSwap cs;
  std::unique_ptr<unsigned char[]> memory;
  {
    auto first = std::make_unique<std::vector<float>>(impulse(0.5f, 700));
    cs.configure(first->data(), 700, false);
  }  // freed: prepare() below must not read it
  prepare(cs, memory);
  CHECK(cs.ready(), "prepared");
  for (float gain : {0.25f, 0.125f}) {
    std::string err;
    {
      auto heap = std::make_unique<std::vector<float>>(impulse(gain, 900));
      err = cs.load(heap->data(), 900);
    }  // freed as soon as load returns, as the rig's local is
    CHECK(err.empty(), "load: %s", err.c_str());
    for (int i = 0; i < 40 && cs.swapping(); ++i) firstSample(cs);
    CHECK(!cs.swapping(), "the swap completed");
  }
  std::unique_ptr<unsigned char[]> again;
  prepare(cs, again);  // re-prepares both cabinets from what CabSwap kept
  CHECK(cs.ready() && cs.length() == 900, "re-prepared, %d taps", cs.length());
  const float got = firstSample(cs);
  CHECK(std::fabs(got - 0.125f) < 1e-3f, "the response that plays is the last one loaded (%.5f)", got);
}

TEST(cabSwapRefusesAHugeResponse) {
  CabSwap cs;
  std::unique_ptr<unsigned char[]> memory;
  cs.configure(nullptr, 0);
  prepare(cs, memory);
  const std::vector<float> h(static_cast<std::size_t>(CabSwap::kMaxTaps) + 1, 0.0f);
  CHECK(!cs.load(h.data(), static_cast<int>(h.size())).empty(), "above kMaxTaps is refused");
  CHECK(!cs.swapping(), "and nothing is left under way");
}

namespace {
struct Delayed final : Block {
  int lat;
  explicit Delayed(int l) : lat(l) {}
  void prepare(const Setup&, Arena&) override {}
  void process(const float* in, float* out, int n) override { copy(in, out, n); }
  void reset() override {}
  void set(ParamId, float) override {}
  float get(ParamId) const override { return 0.0f; }
  int latency() const override { return lat; }
};
}  // namespace

TEST(publishRefusesABlockWithAnotherDelayAndCancelGivesItBack) {
  Delayed a(0), b(88);
  ModelSwap swap(a, b);
  std::unique_ptr<unsigned char[]> memory(new unsigned char[1 << 16]);
  Arena mem(memory.get(), 1 << 16);
  swap.prepare(Setup{48000.0, kBlock}, mem);
  CHECK(swap.standby() == &b, "the standby is b");
  CHECK(!swap.publish(0), "a different delay is refused");
  CHECK(!swap.swapping(), "a refused block is not under way");
  swap.cancel();
  CHECK(swap.idle(), "cancel gives the standby back");
  Delayed c(88), d(88);
  ModelSwap same(c, d);
  Arena mem2(memory.get(), 1 << 16);
  same.prepare(Setup{48000.0, kBlock}, mem2);
  same.standby();
  CHECK(same.publish(0), "equal delays are accepted");
}
