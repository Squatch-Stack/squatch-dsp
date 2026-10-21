// SPDX-License-Identifier: MIT
#include <algorithm>
#include <squatch/dsp/blocks/bypass.hpp>

namespace squatch::dsp {
namespace {
const ParamDesc kParams[] = {
    {Bypass::Engaged, "Engaged", "", 0.0f, 1.0f, 1.0f, 0.0f},
};
static_assert(sizeof(kParams) / sizeof(kParams[0]) == Bypass::kCount, "one descriptor per parameter");

int framesOf(float ms, double rate) { return std::max(1, static_cast<int>(ms * 1e-3 * rate + 0.5)); }
}  // namespace

Span<const ParamDesc> Bypass::params() { return kParams; }

void Bypass::prepare(const Setup& s, Arena& mem) {
  ready_ = false;
  inner_.prepare(s, mem);
  lat_ = inner_.latency() > 0 ? inner_.latency() : 0;  // known once prepared
  fadeFrames_ = framesOf(fadeMs_, s.sampleRate);
  fadeOutFrames_ = framesOf(fadeOutMs_, s.sampleRate);
  fade_.prepare(std::max(fadeFrames_, fadeOutFrames_), mem);
  wet_ = mem.alloc<float>(static_cast<std::size_t>(s.maxBlock));
  dry_ = delay_ = nullptr;
  if (lat_ > 0) {
    dry_ = mem.alloc<float>(static_cast<std::size_t>(s.maxBlock));
    delay_ = mem.alloc<float>(static_cast<std::size_t>(lat_));
  }
  ready_ = fade_.ready() && wet_ && (lat_ == 0 || (dry_ && delay_)) && !mem.failed();
  settled_ = fadeTo_ = target_;
  delayPos_ = warmLeft_ = 0;
}

void Bypass::reset() {
  inner_.reset();
  fade_.finish();
  settled_ = fadeTo_ = target_;
  warmLeft_ = 0;
  if (delay_) clear(delay_, lat_);
  delayPos_ = 0;
}

void Bypass::set(ParamId id, float value) {
  if (id != Engaged) return;
  target_ = kParams[Engaged].clamp(value) >= 0.5f;
  if (!fade_.active()) begin(target_);  // else applied when the fade ends
}

// Starts the handover to `engage`. Switching on restarts the block from silence and, if it has
// latency, warms it on the live input first (process() starts the fade when the warm-up is done).
void Bypass::begin(bool engage) {
  if (warmLeft_ > 0) {  // a switch on is warming: a switch off abandons it, another on changes nothing
    if (!engage) warmLeft_ = 0;
    return;
  }
  if (engage == settled_ && !fade_.active()) return;
  fadeTo_ = engage;
  if (engage && !keepWarm_) {  // a kept-warm block is already in the state the live input gives it
    inner_.reset();
    if (lat_ > 0) {
      warmLeft_ = lat_;
      return;
    }
  }
  fade_.start(engage ? fadeFrames_ : fadeOutFrames_);
}

// The dry input as the wet side will have it: delayed by lat_ (the ring is fed on every block).
const float* Bypass::alignedDry(const float* in, int n) {
  if (lat_ == 0) return in;
  for (int i = 0; i < n; ++i) {
    dry_[i] = delay_[delayPos_];
    delay_[delayPos_] = in[i];
    if (++delayPos_ == lat_) delayPos_ = 0;
  }
  return dry_;
}

void Bypass::process(const float* in, float* out, int n) {
  if (!ready_) return clear(out, n);
  const float* dry = alignedDry(in, n);  // before `out` may overwrite an aliased input
  if (warmLeft_ > 0) {
    inner_.process(in, wet_, n);  // warming: heard by nobody
    copy(dry, out, n);
    warmLeft_ -= n;
    if (warmLeft_ <= 0) {
      warmLeft_ = 0;
      fade_.start(fadeFrames_);
    }
    return;
  }
  if (!fade_.active()) {
    if (settled_) return inner_.process(in, out, n);
    if (keepWarm_) inner_.process(in, wet_, n);  // heard by nobody; before `out` may overwrite an aliased input
    return copy(dry, out, n);
  }
  // Dry fades out and wet in when engaging; the other way round when disengaging.
  inner_.process(in, wet_, n);
  if (fadeTo_) fade_.mix(dry, wet_, out, n);
  else fade_.mix(wet_, dry, out, n);
  if (!fade_.active()) {
    settled_ = fadeTo_;
    if (target_ != settled_) begin(target_);  // a switch arrived mid-fade
  }
}

}  // namespace squatch::dsp
