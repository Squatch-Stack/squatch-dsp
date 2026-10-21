// SPDX-License-Identifier: MIT
// Adapted from nam::Linear in rikkus/OptimisationWorkOnNeuralAmpModelerCore by Rik Hemsley (MIT):
//   https://github.com/rikkus/OptimisationWorkOnNeuralAmpModelerCore/blob/010a0e53bee7ea8867d8c65917e5bdea3babf60f/NAM/linear.cpp
// Notices in tail_convolver.hpp and THIRD_PARTY_NOTICES.md.
#include <squatch/dsp/span.hpp>
#include <squatch/dsp/tail_convolver.hpp>

namespace squatch::dsp {
namespace {

constexpr long long kUnbounded = 1LL << 60;
// Estimated cost of one unit of each kind, in arbitrary but consistent weights. A wrong estimate
// only makes the job's pace uneven; it still finishes by the time it falls due.
constexpr int kLoadCost = 1, kSwapCost = 2, kButterflyCost = 6, kStoreCost = 2, kMulCost = 8, kMirrorCost = 2,
              kUnloadCost = 1;
constexpr long long kPaceNumerator = 3, kPaceDenominator = 2;  // plan to finish in two thirds of the time

bool isPowerOfTwo(int v) { return v >= 2 && v <= (1 << 20) && (v & (v - 1)) == 0; }

std::ptrdiff_t at(int row, int bins) { return static_cast<std::ptrdiff_t>(row) * bins; }

}  // namespace

bool TailConvolver::prepare(const float* ir, int length, int block, Arena& mem) {
  ready_ = false;
  if (!ir || !isPowerOfTwo(block) || length <= 2 * block) return false;
  block_ = block, size_ = 2 * block, bins_ = block + 1;
  parts_ = (length - size_ + block - 1) / block;
  const auto all = static_cast<std::size_t>(parts_) * static_cast<std::size_t>(bins_);
  const auto n = static_cast<std::size_t>(size_), t = static_cast<std::size_t>(block);
  kRe_ = mem.alloc<float>(all), kIm_ = mem.alloc<float>(all);
  sRe_ = mem.alloc<float>(all), sIm_ = mem.alloc<float>(all);
  wRe_ = mem.alloc<float>(n), wIm_ = mem.alloc<float>(n);
  in_ = mem.alloc<float>(3 * t);
  play_ = mem.alloc<float>(t), next_ = mem.alloc<float>(t);
  if (!fft_.prepare(size_, mem) || mem.failed()) return false;
  steps_ = 2 * fft_.bits() + 7;
  const float scale = 1.0f / static_cast<float>(size_);  // the inverse transform is unscaled
  for (int p = 0; p < parts_; ++p) {
    for (int i = 0; i < size_; ++i) {
      const int k = (p + 2) * block + i;
      wRe_[i] = i < block && k < length ? ir[k] * scale : 0.0f;
      wIm_[i] = 0.0f;
    }
    fft_.forward(wRe_, wIm_);
    copy(wRe_, kRe_ + at(p, bins_), bins_);
    copy(wIm_, kIm_ + at(p, bins_), bins_);
  }
  perSample_ = (jobCost() * kPaceNumerator + kPaceDenominator * block - 1) / (kPaceDenominator * block);
  ready_ = true;
  reset();
  return true;
}

void TailConvolver::reset() {
  if (!ready_) return;
  clear(sRe_, parts_ * bins_), clear(sIm_, parts_ * bins_);
  clear(wRe_, size_), clear(wIm_, size_);
  clear(in_, 3 * block_), clear(play_, block_), clear(next_, block_);
  blk_ = pos_ = old_ = new_ = slot_ = step_ = cursor_ = 0;
  active_ = false;
  credit_ = 0;
  forced_ = 0;
}

// The job's steps, in order: load the window, forward transform (reorder, then the passes),
// store the spectrum, multiply, mirror to a full spectrum, inverse transform, unload.
TailConvolver::Step TailConvolver::stepAt(int step) const {
  const int bits = fft_.bits(), store = bits + 2;
  if (step == 0) return {Load, 0};
  if (step <= bits + 1) return {step == 1 ? Reorder : Pass, step - 1};
  if (step == store) return {Store, 0};
  if (step == store + 1) return {Multiply, 0};
  if (step == store + 2) return {Mirror, 0};
  if (step <= store + 3 + bits) return {step == store + 3 ? Reorder : Pass, step - store - 3};
  return {Unload, 0};
}

int TailConvolver::unitsOf(Step s) const {
  switch (s.kind) {
    case Load: return size_;
    case Reorder: case Pass: return fft_.stepUnits(s.fftStep);
    case Store: return bins_;
    case Multiply: return parts_ * bins_;
    case Mirror: return block_ - 1;
    case Unload: return block_;
  }
  return 0;
}

int TailConvolver::costOf(Step s) const {
  switch (s.kind) {
    case Load: return kLoadCost;
    case Reorder: return kSwapCost;
    case Pass: return kButterflyCost;
    case Store: return kStoreCost;
    case Multiply: return kMulCost;
    case Mirror: return kMirrorCost;
    case Unload: return kUnloadCost;
  }
  return 1;
}

long long TailConvolver::jobCost() const {
  long long total = 0;
  for (int i = 0; i < steps_; ++i) {
    const Step s = stepAt(i);
    total += static_cast<long long>(unitsOf(s)) * costOf(s);
  }
  return total;
}

// A block of T samples is complete. The job for the previous block falls due now (it made the
// output that starts playing), and a new one starts for this block.
void TailConvolver::boundary() {
  if (active_) ++forced_, run(kUnbounded);
  float* const done = next_;
  next_ = play_, play_ = done;
  startJob();
  blk_ = blk_ == 2 ? 0 : blk_ + 1;
  pos_ = 0;
}

void TailConvolver::startJob() {
  old_ = blk_ == 0 ? 2 : blk_ - 1;
  new_ = blk_;
  step_ = cursor_ = 0;
  credit_ = 0;
  active_ = true;
}

void TailConvolver::finishJob() {
  active_ = false;
  slot_ = slot_ + 1 == parts_ ? 0 : slot_ + 1;
}

void TailConvolver::work(int n) {
  if (!active_ || n <= 0) return;
  credit_ += perSample_ * n;
  run(credit_);
}

// Up to `budget` of estimated cost, in order; always makes progress while work is left.
void TailConvolver::run(long long budget) {
  while (active_ && budget > 0) {
    const Step s = stepAt(step_);
    const int units = unitsOf(s), cost = costOf(s);
    const long long affordable = budget / cost > 0 ? budget / cost : 1;
    const int take = static_cast<int>(affordable < units - cursor_ ? affordable : units - cursor_);
    exec(s, cursor_, cursor_ + take);
    budget -= static_cast<long long>(take) * cost;
    credit_ -= static_cast<long long>(take) * cost;
    cursor_ += take;
    if (cursor_ == units) {
      cursor_ = 0;
      if (++step_ == steps_) finishJob();
    }
  }
}

void TailConvolver::exec(Step s, int from, int to) {
  switch (s.kind) {
    case Load: return load(from, to);
    case Reorder: case Pass:
      return fft_.run(wRe_, wIm_, step_ <= fft_.bits() + 1 ? 1.0f : -1.0f, s.fftStep, {from, to});
    case Store: return store(from, to);
    case Multiply: return multiply(from, to);
    case Mirror: return mirror(from, to);
    case Unload:
      for (int i = from; i < to; ++i) next_[i] = wRe_[block_ + i];
      return;
  }
}

void TailConvolver::store(int from, int to) {
  float* const re = sRe_ + at(slot_, bins_);
  float* const im = sIm_ + at(slot_, bins_);
  // The working space becomes the sum the multiplies build, so it is cleared as it is read.
  for (int k = from; k < to; ++k) re[k] = wRe_[k], im[k] = wIm_[k], wRe_[k] = wIm_[k] = 0.0f;
}

void TailConvolver::mirror(int from, int to) {
  for (int u = from; u < to; ++u) wRe_[size_ - 1 - u] = wRe_[u + 1], wIm_[size_ - 1 - u] = -wIm_[u + 1];
}

// The window: the older block, then the newer; its imaginary part is zero.
void TailConvolver::load(int from, int to) {
  for (int i = from; i < to; ++i) {
    wRe_[i] = i < block_ ? in_[old_ * block_ + i] : in_[new_ * block_ + (i - block_)];
    wIm_[i] = 0.0f;
  }
}

// w += H_p * S_(newest - p) for units [from, to), a unit being one (partition, bin).
void TailConvolver::multiply(int from, int to) {
  for (int u = from; u < to;) {
    const int p = u / bins_, k0 = u - p * bins_;
    const int end = (to - u < bins_ - k0) ? k0 + (to - u) : bins_;
    const int slot = slot_ - p < 0 ? slot_ - p + parts_ : slot_ - p;
    const float* hr = kRe_ + at(p, bins_);
    const float* hi = kIm_ + at(p, bins_);
    const float* xr = sRe_ + at(slot, bins_);
    const float* xi = sIm_ + at(slot, bins_);
    for (int k = k0; k < end; ++k) {
      wRe_[k] += hr[k] * xr[k] - hi[k] * xi[k];
      wIm_[k] += hr[k] * xi[k] + hi[k] * xr[k];
    }
    u += end - k0;
  }
}

}  // namespace squatch::dsp
