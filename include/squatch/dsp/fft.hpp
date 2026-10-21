// SPDX-License-Identifier: MIT
// A small radix-2 complex FFT, in place on split real and imaginary arrays (so the loops that
// multiply spectra vectorise). Tables come from the arena in prepare(); transform() is real-time
// safe. Our own rather than a vendored one (PLAN.md, open question 1): simplest to trust, and the
// cabinet block only needs short transforms (2 x its partition, 128 points by default).
//
// forward: X[k] = sum x[n] e^(-2 pi i k n / N). inverse is the same sum with +i and no 1/N:
// scale once where it is cheapest (the cabinet folds it into the impulse response's spectra).
#pragma once
#include "arena.hpp"

namespace squatch::dsp {

class Fft {
 public:
  /// Setup time. size is a power of two, 2 or more. False (and unusable) when it is not, or when
  /// the arena runs out.
  bool prepare(int size, Arena& mem);
  bool ready() const { return size_ > 0; }
  int size() const { return size_; }

  void forward(float* re, float* im) const { transform(re, im, 1.0f); }
  void inverse(float* re, float* im) const { transform(re, im, -1.0f); }

  /// The same transform cut into steps a caller can spread over time: step 0 reorders the input
  /// (bit reversal), steps 1..bits() are the butterfly passes. Each step has stepUnits() units;
  /// run() does the units in `units` of one step (sign +1 forward, -1 inverse). Running every step
  /// over its whole range, in order, gives exactly the bits forward()/inverse() give.
  int bits() const { return bits_; }
  int stepUnits(int step) const { return step == 0 ? swaps_ : size_ / 2; }
  struct Range {
    int from, to;
  };
  void run(float* re, float* im, float sign, int step, Range units) const;

 private:
  void transform(float* re, float* im, float sign) const;
  void reorder(float* re, float* im) const;

  int size_ = 0;
  int bits_ = 0;
  int* swap_ = nullptr;      // bit-reversal permutation, as pairs to exchange
  int swaps_ = 0;
  float* cos_ = nullptr;     // cos(2 pi k / N), k < N/2
  float* sin_ = nullptr;     // sin(2 pi k / N), k < N/2
};

}  // namespace squatch::dsp
