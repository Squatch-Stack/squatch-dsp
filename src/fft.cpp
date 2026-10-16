// SPDX-License-Identifier: MIT
#include <cmath>
#include <squatch/dsp/fft.hpp>

namespace squatch::dsp {

namespace {

int reverseBits(int v, int bits) {
  int r = 0;
  for (int b = 0; b < bits; ++b, v >>= 1) r = (r << 1) | (v & 1);
  return r;
}

}  // namespace

bool Fft::prepare(int size, Arena& mem) {
  size_ = 0;
  if (size < 2 || (size & (size - 1)) != 0) return false;
  int bits = 0;
  while ((1 << bits) < size) ++bits;
  swap_ = mem.alloc<int>(static_cast<std::size_t>(size));
  cos_ = mem.alloc<float>(static_cast<std::size_t>(size / 2));
  sin_ = mem.alloc<float>(static_cast<std::size_t>(size / 2));
  if (!swap_ || !cos_ || !sin_) return false;
  swaps_ = 0;
  for (int i = 0; i < size; ++i) {
    const int j = reverseBits(i, bits);
    if (i < j) swap_[2 * swaps_] = i, swap_[2 * swaps_ + 1] = j, ++swaps_;
  }
  for (int k = 0; k < size / 2; ++k) {
    const double a = 6.283185307179586 * k / size;  // in double: the tables set the accuracy
    cos_[k] = static_cast<float>(std::cos(a));
    sin_[k] = static_cast<float>(std::sin(a));
  }
  size_ = size;
  return true;
}

void Fft::reorder(float* re, float* im) const {
  for (int s = 0; s < swaps_; ++s) {
    const int i = swap_[2 * s], j = swap_[2 * s + 1];
    const float r = re[i], m = im[i];
    re[i] = re[j], im[i] = im[j];
    re[j] = r, im[j] = m;
  }
}

// Iterative decimation in time. sign +1 is the forward transform (twiddle e^(-i...)).
void Fft::transform(float* re, float* im, float sign) const {
  if (size_ == 0) return;
  reorder(re, im);
  for (int half = 1; half < size_; half *= 2) {
    const int stride = size_ / (2 * half);
    for (int start = 0; start < size_; start += 2 * half) {
      for (int k = 0; k < half; ++k) {
        const float wr = cos_[k * stride], wi = -sign * sin_[k * stride];
        const int a = start + k, b = a + half;
        const float tr = re[b] * wr - im[b] * wi, ti = re[b] * wi + im[b] * wr;
        re[b] = re[a] - tr, im[b] = im[a] - ti;
        re[a] += tr, im[a] += ti;
      }
    }
  }
}

}  // namespace squatch::dsp
