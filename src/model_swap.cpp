// SPDX-License-Identifier: MIT
// Adapted from the model-swap plan in namchain by Rik Hemsley (MIT); links and notices in
// model_swap.hpp and THIRD_PARTY_NOTICES.md.
#include <squatch/dsp/blocks/model_swap.hpp>

namespace squatch::dsp {
namespace {
// The handoff's three orderings. SQUATCH_BREAK_HANDOFF exists only so the thread test can prove
// it would notice them weakened (make test-tsan-negative): 1 drops the release of publish(),
// 2 drops the audio thread's acquire, 3 drops the release that gives the old block back.
#ifdef SQUATCH_BREAK_HANDOFF
constexpr std::memory_order kPublish = SQUATCH_BREAK_HANDOFF == 1 ? std::memory_order_relaxed : std::memory_order_acq_rel;
constexpr std::memory_order kTake = SQUATCH_BREAK_HANDOFF == 2 ? std::memory_order_relaxed : std::memory_order_acquire;
constexpr std::memory_order kGiveBack = SQUATCH_BREAK_HANDOFF == 3 ? std::memory_order_relaxed : std::memory_order_release;
#else
constexpr std::memory_order kPublish = std::memory_order_acq_rel;
constexpr std::memory_order kTake = std::memory_order_acquire;
constexpr std::memory_order kGiveBack = std::memory_order_release;
#endif
}  // namespace

void ModelSwap::prepare(const Setup& s, Arena& mem) {
  ready_ = false;
  block_[0]->prepare(s, mem);
  block_[1]->prepare(s, mem);
  fadeFrames_ = static_cast<int>(kFadeMs * 1e-3 * s.sampleRate + 0.5);
  if (fadeFrames_ < 1) fadeFrames_ = 1;
  fade_.prepare(fadeFrames_, mem);
  scratch_ = mem.alloc<float>(static_cast<std::size_t>(s.maxBlock));
  wet_ = mem.alloc<float>(static_cast<std::size_t>(s.maxBlock));
  ready_ = fade_.ready() && scratch_ && wet_ && !mem.failed();
  state_.store(Idle, std::memory_order_release);
  swaps_.store(0, std::memory_order_relaxed);
}

Block* ModelSwap::standby() {
  int expect = Idle;
  if (!ready_ || !state_.compare_exchange_strong(expect, Loading, std::memory_order_acq_rel)) return nullptr;
  return block_[1 - active_.load(std::memory_order_relaxed)];
}

bool ModelSwap::publish(int warmFrames) {
  // The two blocks must delay the signal alike: crossfading misaligned outputs is a comb filter, and
  // a Bypass around this block has fixed its dry delay already. Refused, the caller cancel()s.
  if (block_[0]->latency() != block_[1]->latency()) return false;
  if (warmFrames >= 0 && state_.load(std::memory_order_acquire) == Loading) warm_.store(warmFrames, std::memory_order_relaxed);
  int expect = Loading;
  return state_.compare_exchange_strong(expect, Requested, kPublish);
}

void ModelSwap::cancel() {
  int expect = Loading;
  state_.compare_exchange_strong(expect, Idle, std::memory_order_acq_rel);
}

void ModelSwap::set(ParamId id, float value) {
  if (id < kMaxParams) value_[id] = value, known_[id] = true;
  block_[active_.load(std::memory_order_relaxed)]->set(id, value);
  const int s = state_.load(std::memory_order_acquire);
  if (s == Warming || s == Fading) block_[1 - active_.load(std::memory_order_relaxed)]->set(id, value);
}

// The standby is ours from here: give it the parameters the playing block has.
void ModelSwap::begin() {
  Block& next = *block_[1 - active_.load(std::memory_order_relaxed)];
  for (int id = 0; id < kMaxParams; ++id)
    if (known_[id]) next.set(static_cast<ParamId>(id), value_[id]);
  warmLeft_ = warm_.load(std::memory_order_relaxed);
  state_.store(Warming, std::memory_order_release);
}

// The fade has ended inside a block: the incoming block plays, the other stands by.
void ModelSwap::finish() {
  fade_.finish();
  active_.store(1 - active_.load(std::memory_order_relaxed), std::memory_order_relaxed);
  swaps_.fetch_add(1, std::memory_order_relaxed);
  state_.store(Idle, kGiveBack);
}

void ModelSwap::reset() {
  // A swap nobody has started yet (a stage that is off is not processed) is taken now and finished,
  // so the new block is the one reset below and nothing waits for a fade that has no audio to run on.
  if (state_.load(kTake) == Requested) begin();
  if (state_.load(std::memory_order_acquire) >= Warming) finish();  // complete the swap rather than lose it
  block_[active_.load(std::memory_order_relaxed)]->reset();
}

void ModelSwap::process(const float* in, float* out, int n) {
  if (!ready_) return clear(out, n);
  if (state_.load(kTake) == Requested) begin();
  Block& cur = *block_[active_.load(std::memory_order_relaxed)];
  Block& next = *block_[1 - active_.load(std::memory_order_relaxed)];
  const int s = state_.load(std::memory_order_acquire);
  if (s < Warming) return cur.process(in, out, n);
  cur.process(in, wet_, n);          // the playing block, before `out` may overwrite an aliased input
  next.process(in, scratch_, n);     // the standby, warming or fading
  if (s == Warming) {
    copy(wet_, out, n);
    warmLeft_ -= n;
    if (warmLeft_ <= 0) {
      fade_.start(fadeFrames_);
      state_.store(Fading, std::memory_order_release);
    }
    return;
  }
  fade_.mix(wet_, scratch_, out, n);
  if (!fade_.active()) finish();
}

}  // namespace squatch::dsp
