// SPDX-License-Identifier: MIT
// TailConvolver: the far end of a very long impulse response (a reverb-length cabinet or room), in
// big partitions whose work is spread across the samples in between, so no callback runs a
// large transform.
//
// Adapted from the partition layout and work scheduling of nam::Linear (the FFT engine's "tail
// tier") in rikkus/OptimisationWorkOnNeuralAmpModelerCore by Rik Hemsley (MIT), pinned to 010a0e5:
//   https://github.com/rikkus/OptimisationWorkOnNeuralAmpModelerCore/blob/010a0e53bee7ea8867d8c65917e5bdea3babf60f/NAM/linear.cpp
//   https://github.com/rikkus/OptimisationWorkOnNeuralAmpModelerCore/blob/010a0e53bee7ea8867d8c65917e5bdea3babf60f/NAM/linear.h
//   https://github.com/rikkus/OptimisationWorkOnNeuralAmpModelerCore/blob/010a0e53bee7ea8867d8c65917e5bdea3babf60f/docs/linear.rst
// Copyright (c) 2023 Steven Atkinson (NeuralAmpModelerCore) and (c) 2026 Rik Hemsley (his
// changes); the licence text is in THIRD_PARTY_NOTICES.md. Taken: the idea and the numbers. The
// taps from 2T on are cut into partitions of T; a block's spectrum is first needed T samples
// after the block completes, so its forward transform, all its multiplies and the inverse
// transform are a job shared out across the next T samples, by estimated cost, and finished by
// force if it is still open when it falls due. Changed here: our own FFT, cut into steps
// (fft.hpp) instead of his decomposition into small transforms and a combining pass; one
// channel; no Eigen.
//
// Exactness: the output for sample t is the convolution of the input with taps [2T, length),
// whatever the call sizes. The taps below 2T are the caller's, see Cabinet.
//
// prepare() is setup time. tick() and work() are real-time safe: no allocation, lock or call
// into the system.
#pragma once
#include "arena.hpp"
#include "fft.hpp"

namespace squatch::dsp {

class TailConvolver {
 public:
  /// Setup time. Takes taps [2 * block, length) of `ir` (copied, scaled and transformed here).
  /// `block` is a power of two, 2 or more. False (and unusable) when it is not, when there are
  /// no taps from 2 * block on, or when the arena runs out.
  bool prepare(const float* ir, int length, int block, Arena& mem);
  bool ready() const { return ready_; }
  void reset();

  /// One sample: the tail's contribution to the output for it, and `x` joins the current block.
  float tick(float x) {
    const float y = play_[pos_];
    in_[blk_ * block_ + pos_] = x;
    if (++pos_ == block_) boundary();
    return y;
  }

  /// Call once per callback with the number of samples just ticked: runs that share of the job.
  void work(int n);

  int block() const { return block_; }
  int partitions() const { return parts_; }
  bool jobOpen() const { return active_; }
  long forced() const { return forced_; }  // blocks whose job was still open when it fell due

 private:
  enum Kind { Load, Reorder, Pass, Store, Multiply, Mirror, Unload };
  struct Step {
    Kind kind;
    int fftStep;
  };
  Step stepAt(int step) const;
  int unitsOf(Step s) const;
  int costOf(Step s) const;
  long long jobCost() const;
  void boundary();
  void startJob();
  void finishJob();
  void run(long long budget);
  void exec(Step s, int from, int to);
  void load(int from, int to);
  void multiply(int from, int to);
  void store(int from, int to);
  void mirror(int from, int to);

  int block_ = 0, size_ = 0, bins_ = 0, parts_ = 0, steps_ = 0;
  Fft fft_;
  float* kRe_ = nullptr;   // spectra of partitions 2..: bins 0..T, 1/(2T) folded in
  float* kIm_ = nullptr;
  float* sRe_ = nullptr;   // spectra of the last `parts_` windows, a ring
  float* sIm_ = nullptr;
  float* wRe_ = nullptr;   // the transform's working space, 2T
  float* wIm_ = nullptr;
  float* in_ = nullptr;    // three blocks of input, a ring by block
  float* play_ = nullptr;  // the tail output being played
  float* next_ = nullptr;  // the tail output the open job is making
  int blk_ = 0, pos_ = 0;
  int old_ = 0, new_ = 0;  // the input blocks the open job reads
  int slot_ = 0;           // the ring slot the open job writes
  bool active_ = false;
  int step_ = 0, cursor_ = 0;
  long long credit_ = 0, perSample_ = 0;
  long forced_ = 0;
  bool ready_ = false;
};

}  // namespace squatch::dsp
