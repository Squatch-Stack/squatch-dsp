// SPDX-License-Identifier: MIT
// Cabinet: convolution with an impulse response (a speaker cabinet and its microphone), with no
// added latency.
//
// The response is split at the partition size P. Its first P taps run as a direct FIR, sample by
// sample. The rest is uniformly partitioned FFT convolution (overlap-save, FFT size 2P): each
// partition j >= 1 is delayed by at least P samples, so when an input block of P samples
// completes, the tail of the next P output samples is already fully determined and is computed
// then, in one go. Head plus tail is the exact convolution, for any call size and in place.
//
// Cost per sample: P multiply-adds for the head, plus (2 FFTs of 2P + (J-1)(P+1) complex
// multiply-adds) / P for the tail, J = ceil(length / P). The response is copied, and its spectra
// computed, in prepare(); the host loads and resamples it (see hosts/common/ir_file.hpp).
// Swapping a response while audio runs is the host's job: prepare a second instance off the audio
// thread and crossfade (hosts/common/block_swap.hpp). Controls: contracts/cabinet.params.json.
//
// After the convolution come the browser rig's Cab filters (web/js/audio/nodes.js, CabNode): a 2nd-order
// high-pass (Low cut) and low-pass (High cut), RBJ cookbook, Q 0.7 dB as WebAudio's lowpass/highpass
// define Q (a linear Q of 1.084), then Level. Both are always in the path, as the rig's are (their
// extreme settings are not "off"); coefficients are computed and run in double, the 20 Hz end of a
// float direct form being too coarse. configure(..., cuts = false) removes them, for a host with its
// own EQ; a block with no response passes its input through untouched either way.
//
// IR level. The rig scales a response it loads to RMS 0.25 per channel (nodes.js setIr) and its
// synthetic one to unity gain at 1 kHz; hosts/common/ir_file.hpp scales to unit energy. This block
// does not scale: it convolves what it is given. A response taken from the rig's library and
// scaled to unit energy plays 12 dB louder than the rig's (20 log10(1 / 0.25)); the host's job is to
// apply 0.25 / sqrt(energy) if it wants the rig's level. docs/PARITY-FIXES-1.md has the arithmetic.
// Mono: a stereo host runs one instance per channel. tail() is the response's length plus the
// filters' ring-down (kCutTailSeconds).
//
// Long responses (more than kLongTaps) take a different plan, from the table in planFor():
// a larger partition (the head is longer, there are far fewer partitions to multiply), and past
// 32768 taps the taps from 2T on go to a TailConvolver, which works in partitions of T taps and
// shares its transforms and multiplies out across the samples between blocks. See tail_convolver.hpp
// for the credit. The first 2T taps keep the plan above, so the latency is still zero.
#pragma once
#include <squatch/dsp/block.hpp>
#include <squatch/dsp/contracts/cabinet.hpp>
#include <squatch/dsp/fft.hpp>
#include <squatch/dsp/param.hpp>
#include <squatch/dsp/tail_convolver.hpp>

namespace squatch::dsp {

class Cabinet final : public Block, public contracts::CabinetContract {
 public:
  static constexpr int kDefaultPartition = 64;
  static constexpr int kMinPartition = 4, kMaxPartition = 4096;
  static constexpr double kCutTailSeconds = 0.25;  // a 20 Hz high-pass at Q 0.7 dB is 120 dB down by then
  static constexpr int kControlRate = 16;          // samples between filter redesigns while a cut moves

  /// Longer responses than this take the long plan under Engine::Auto.
  static constexpr int kLongTaps = 16384;

  /// Which plan configure() uses. Auto: the caller's partition up to kLongTaps taps, the long plan above.
  enum class Engine { Auto, Partitioned, Long };
  /// How a response is cut: the head and uniform partitions of `partition` taps cover the first
  /// 2 * tail taps (all of it when `tail` is 0), and a TailConvolver of `tail`-tap partitions the rest.
  struct Plan {
    int partition;
    int tail;
  };
  /// The plan Auto and Long give a response of `length` taps (partition is used up to kLongTaps).
  static Plan planFor(int length, int partition, Engine engine = Engine::Auto);

  Cabinet();

  /// Setup time only, before prepare(). The response is read (copied) by prepare(), so it must
  /// stay valid until then; it is at the sample rate the host will run. With no response, or a
  /// partition that is not a power of two in [kMinPartition, kMaxPartition], the block passes
  /// its input through unchanged.
  void configure(const float* ir, int length, int partition = kDefaultPartition, bool cuts = true,
                 Engine engine = Engine::Auto);

  /// As configure(), with the plan given: for tests and tuning. A tail that is not a power of
  /// two no smaller than the partition is refused like a bad partition.
  void configure(const float* ir, int length, Plan plan, bool cuts = true);

  void prepare(const Setup& s, Arena& mem) override;
  void process(const float* in, float* out, int n) override;
  void reset() override;
  void set(ParamId id, float value) override;
  float get(ParamId id) const override;
  int tail() const override;
  Span<const ParamDesc> parameters() const override { return params(); }
  const BlockInfo& info() const override { return kInfo; }

  int length() const { return length_; }        // taps in use
  int partitions() const { return parts_; }     // J, the head included (of the first 2T taps with a tail)
  int farPartitions() const { return far_.ready() ? far_.partitions() : 0; }  // the TailConvolver's
  Plan plan() const { return {partition_, tailPlan_}; }  // as configured
  const TailConvolver& far() const { return far_; }
  bool ready() const { return ready_; }

 private:
  bool take(Arena& mem);
  void loadHead();
  void loadTail();
  float step(float x);
  void run(const float* in, float* out, int len);
  void finishBlock();
  void accumulate();
  int controlSegment(int left);  // advances the smoothers, redesigns a moving cut; samples to run
  void designCuts();
  float cutsAndLevel(float y);

  // One 2nd-order section, direct form I, in double.
  struct Section {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, x1 = 0, x2 = 0, y1 = 0, y2 = 0;
    double run(double x) {
      const double y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
      x2 = x1, x1 = x, y2 = y1, y1 = y;
      return y;
    }
  };
  static void designSection(Section& s, bool highPass, double hz, double rate);

  float value_ = 0.0f;
  Smoother level_, lowcut_, highcut_;
  Section hp_, lp_;
  bool cuts_ = true, cutsOn_ = false;  // asked for; in use (there is a response)
  double rate_ = 48000.0;
  const float* source_ = nullptr;
  int sourceLength_ = 0;
  int partition_ = kDefaultPartition;
  int tailPlan_ = 0;         // tail block asked for; 0 for none

  int p_ = 0, parts_ = 0, bins_ = 0, length_ = 0;
  int coreLength_ = 0;       // taps the head and uniform partitions cover
  int tailBlock_ = 0;
  bool tailOn_ = false;
  TailConvolver far_;
  float* head_ = nullptr;    // the first P taps, reversed, so a window of history is a dot product
  float* hist_ = nullptr;    // the last P inputs, stored twice so a window never wraps
  int histPos_ = 0;
  float* prev_ = nullptr;    // the previous complete input block
  float* cur_ = nullptr;     // the input block being filled
  int pos_ = 0;
  float* tail_ = nullptr;    // tail output for the block being filled
  float* workRe_ = nullptr;  // 2P
  float* workIm_ = nullptr;
  float* hRe_ = nullptr;     // spectra of partitions 1..J-1, bins 0..P, 1/(2P) folded in
  float* hIm_ = nullptr;
  float* xRe_ = nullptr;     // spectra of the last J-1 input blocks, a ring
  float* xIm_ = nullptr;
  int xNewest_ = 0;
  Fft fft_;
  bool ready_ = false;
};

}  // namespace squatch::dsp
