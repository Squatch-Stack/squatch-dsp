// SPDX-License-Identifier: MIT
#include <cmath>
#include <squatch/dsp/blocks/cabinet.hpp>
#include <squatch/dsp/span.hpp>

namespace squatch::dsp {
namespace {

const float kIdentity[1] = {1.0f};

bool validPartition(int p) {
  return p >= Cabinet::kMinPartition && p <= Cabinet::kMaxPartition && (p & (p - 1)) == 0;
}

float dot(const float* a, const float* b, int n) {
  float s0 = 0.0f, s1 = 0.0f, s2 = 0.0f, s3 = 0.0f;  // four sums: independent chains vectorise
  int i = 0;
  for (; i + 4 <= n; i += 4) s0 += a[i] * b[i], s1 += a[i + 1] * b[i + 1], s2 += a[i + 2] * b[i + 2], s3 += a[i + 3] * b[i + 3];
  for (; i < n; ++i) s0 += a[i] * b[i];
  return (s0 + s1) + (s2 + s3);
}

}  // namespace

Cabinet::Cabinet() {
  value_ = kParams[Level].def;
  level_.jump(dbToGain(value_));
  lowcut_.jump(kParams[Lowcut].def);
  highcut_.jump(kParams[Highcut].def);
}

void Cabinet::configure(const float* ir, int length, int partition, bool cuts) {
  const bool usable = ir && length > 0 && validPartition(partition);
  source_ = usable ? ir : kIdentity;
  sourceLength_ = usable ? length : 1;
  partition_ = usable ? partition : kMinPartition;
  cuts_ = cuts;
  cutsOn_ = cuts && usable;
}

int Cabinet::tail() const { return length_ + (cutsOn_ ? static_cast<int>(kCutTailSeconds * rate_) : 0); }

float Cabinet::get(ParamId id) const {
  if (id == Level) return value_;
  if (id == Lowcut) return lowcut_.target();
  if (id == Highcut) return highcut_.target();
  return NAN;
}

// The rig's BiquadFilterNode: RBJ lowpass / highpass, Q given in dB (WebAudio), f held below Nyquist.
void Cabinet::designSection(Section& s, bool highPass, double hz, double rate) {
  const double w0 = 2.0 * 3.14159265358979323846 * std::fmin(hz, 0.5 * rate) / rate;
  const double cw = std::cos(w0), al = std::sin(w0) / (2.0 * std::pow(10.0, 0.7 / 20.0));
  const double a0 = 1.0 + al, b = highPass ? (1.0 + cw) / 2.0 : (1.0 - cw) / 2.0;
  s.b0 = b / a0, s.b1 = (highPass ? -(1.0 + cw) : 1.0 - cw) / a0, s.b2 = s.b0;
  s.a1 = -2.0 * cw / a0, s.a2 = (1.0 - al) / a0;
}

void Cabinet::designCuts() {
  designSection(hp_, true, lowcut_.value(), rate_);
  designSection(lp_, false, highcut_.value(), rate_);
}

// Filters then Level for one sample of the convolution's output.
float Cabinet::cutsAndLevel(float y) {
  const float gain = level_.next();
  if (!cutsOn_) return gain * y;
  return gain * static_cast<float>(lp_.run(hp_.run(y)));
}

// How many of the next `left` samples run before a moving cut needs its filters redesigned.
int Cabinet::controlSegment(int left) {
  if (!cutsOn_ || (!lowcut_.smoothing() && !highcut_.smoothing())) return left;
  const int len = left < kControlRate ? left : kControlRate;
  lowcut_.advance(len), highcut_.advance(len);
  designCuts();
  return len;
}

bool Cabinet::take(Arena& mem) {
  const auto p = static_cast<std::size_t>(p_), tail = static_cast<std::size_t>((parts_ - 1) * bins_);
  head_ = mem.alloc<float>(p);
  hist_ = mem.alloc<float>(2 * p);
  prev_ = mem.alloc<float>(p);
  cur_ = mem.alloc<float>(p);
  tail_ = mem.alloc<float>(p);
  if (parts_ > 1) {
    workRe_ = mem.alloc<float>(2 * p), workIm_ = mem.alloc<float>(2 * p);
    hRe_ = mem.alloc<float>(tail), hIm_ = mem.alloc<float>(tail);
    xRe_ = mem.alloc<float>(tail), xIm_ = mem.alloc<float>(tail);
    fft_.prepare(2 * p_, mem);
  }
  return !mem.failed();
}

void Cabinet::prepare(const Setup& s, Arena& mem) {
  ready_ = false;
  if (!source_) configure(nullptr, 0);
  rate_ = s.sampleRate;
  level_.prepare(s.sampleRate, kParams[Level].smoothMs);
  lowcut_.prepare(s.sampleRate, kParams[Lowcut].smoothMs);
  highcut_.prepare(s.sampleRate, kParams[Highcut].smoothMs);
  designCuts();
  p_ = partition_;
  length_ = sourceLength_;
  parts_ = (length_ + p_ - 1) / p_;
  bins_ = p_ + 1;
  if (!take(mem)) return;
  loadHead();
  loadTail();
  reset();
  ready_ = true;
}

void Cabinet::loadHead() {
  for (int m = 0; m < p_; ++m) {
    const int k = p_ - 1 - m;
    head_[m] = k < length_ ? source_[k] : 0.0f;
  }
}

// Partition j (taps jP .. jP+P-1), zero-padded to 2P, transformed; bins 0..P kept, scaled by
// 1/(2P) so the inverse transform needs no scaling of its own.
void Cabinet::loadTail() {
  const float scale = 1.0f / static_cast<float>(2 * p_);
  for (int j = 1; j < parts_; ++j) {
    for (int i = 0; i < 2 * p_; ++i) {
      const int k = j * p_ + i;
      workRe_[i] = i < p_ && k < length_ ? source_[k] * scale : 0.0f;
      workIm_[i] = 0.0f;
    }
    fft_.forward(workRe_, workIm_);
    copy(workRe_, hRe_ + (j - 1) * bins_, bins_);
    copy(workIm_, hIm_ + (j - 1) * bins_, bins_);
  }
}

void Cabinet::reset() {
  if (!head_) return;
  clear(hist_, 2 * p_);
  clear(prev_, p_);
  clear(cur_, p_);
  clear(tail_, p_);
  if (parts_ > 1) clear(xRe_, (parts_ - 1) * bins_), clear(xIm_, (parts_ - 1) * bins_);
  histPos_ = pos_ = xNewest_ = 0;
  hp_.x1 = hp_.x2 = hp_.y1 = hp_.y2 = lp_.x1 = lp_.x2 = lp_.y1 = lp_.y2 = 0.0;
}

void Cabinet::set(ParamId id, float value) {
  if (id >= kParamCount) return;
  const float v = kParams[id].clamp(value);
  if (id == Level) value_ = v, level_.setTarget(dbToGain(v));
  if (id == Lowcut) lowcut_.setTarget(v);
  if (id == Highcut) highcut_.setTarget(v);
  if (!lowcut_.smoothing() && !highcut_.smoothing()) designCuts();
}

void Cabinet::process(const float* in, float* out, int n) {
  if (!ready_) return clear(out, n);
  for (int done = 0; done < n;) {
    const int len = controlSegment(n - done);
    for (int i = done; i < done + len; ++i) out[i] = cutsAndLevel(step(in[i]));  // step() reads in[i] first: in place is fine
    done += len;
  }
}

// One sample: the direct head over the last P inputs, plus the precomputed tail.
float Cabinet::step(float x) {
  hist_[histPos_] = hist_[histPos_ + p_] = x;
  const float y = dot(hist_ + histPos_ + 1, head_, p_) + tail_[pos_];
  histPos_ = histPos_ + 1 == p_ ? 0 : histPos_ + 1;
  cur_[pos_] = x;
  if (++pos_ == p_) finishBlock();
  return y;
}

// An input block is complete: its spectrum joins the ring, and the tail of the next block's
// output is computed from partitions 1..J-1 (overlap-save: the last P samples of the inverse).
void Cabinet::finishBlock() {
  pos_ = 0;
  float* const done = cur_;
  if (parts_ > 1) {
    copy(prev_, workRe_, p_);
    copy(done, workRe_ + p_, p_);
    clear(workIm_, 2 * p_);
    fft_.forward(workRe_, workIm_);
    xNewest_ = xNewest_ + 1 == parts_ - 1 ? 0 : xNewest_ + 1;
    copy(workRe_, xRe_ + xNewest_ * bins_, bins_);
    copy(workIm_, xIm_ + xNewest_ * bins_, bins_);
    accumulate();
    fft_.inverse(workRe_, workIm_);
    copy(workRe_ + p_, tail_, p_);
  }
  cur_ = prev_;
  prev_ = done;
}

// work = sum over j of H_j X_(newest - j + 1), bins 0..P, then mirrored for a real result.
void Cabinet::accumulate() {
  clear(workRe_, bins_);
  clear(workIm_, bins_);
  int slot = xNewest_;
  for (int j = 1; j < parts_; ++j) {
    const float* hr = hRe_ + (j - 1) * bins_;
    const float* hi = hIm_ + (j - 1) * bins_;
    const float* xr = xRe_ + slot * bins_;
    const float* xi = xIm_ + slot * bins_;
    for (int k = 0; k < bins_; ++k) {
      workRe_[k] += hr[k] * xr[k] - hi[k] * xi[k];
      workIm_[k] += hr[k] * xi[k] + hi[k] * xr[k];
    }
    slot = slot == 0 ? parts_ - 2 : slot - 1;
  }
  for (int k = 1; k < p_; ++k) workRe_[2 * p_ - k] = workRe_[k], workIm_[2 * p_ - k] = -workIm_[k];
}

}  // namespace squatch::dsp
