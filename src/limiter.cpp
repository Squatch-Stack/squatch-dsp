// SPDX-License-Identifier: MIT
#include <algorithm>
#include <cmath>
#include <squatch/dsp/blocks/limiter.hpp>

namespace squatch::dsp {
namespace {

// NaN is not audio: silence. Huge values are clamped so the detector's sums stay finite.
float sanitize(float x) {
  return x == x ? std::fmin(std::fmax(x, -Limiter::kMaxInput), Limiter::kMaxInput) : 0.0f;
}

// Bessel I0 by its power series: enough terms for beta up to about 10.
double besselI0(double x) {
  double sum = 1.0, term = 1.0;
  for (int k = 1; k < 30; ++k) {
    term *= x * x / (4.0 * k * k);
    sum += term;
  }
  return sum;
}

double kaiser(double z, double beta) { return besselI0(beta * std::sqrt(std::fmax(0.0, 1.0 - z * z))) / besselI0(beta); }

}  // namespace

Limiter::Limiter() {
  for (int k = 0; k < kParamCount; ++k) value_[k] = kParams[k].def;
  drive_.jump(dbToGain(value_[Drive]));
  output_.jump(dbToGain(value_[Output]));
  designInterpolator();
  update();
}

void Limiter::prepare(const Setup& s, Arena& mem) {
  sampleRate_ = s.sampleRate;
  channels_ = s.layout == Layout::Stereo ? 2 : 1;
  sizeWindows(s.sampleRate);
  ready_ = true;
  for (int c = 0; c < channels_; ++c) {
    delay_[c] = mem.alloc<float>(static_cast<std::size_t>(latency_));
    hist_[c] = mem.alloc<float>(2 * kTaps);
    ready_ = ready_ && delay_[c] && hist_[c];
  }
  holdVal_ = mem.alloc<float>(static_cast<std::size_t>(holdLen_));
  holdAt_ = mem.alloc<std::uint32_t>(static_cast<std::size_t>(holdLen_));
  box_ = mem.alloc<float>(static_cast<std::size_t>(boxLen_));
  ready_ = ready_ && holdVal_ && holdAt_ && box_;
  drive_.prepare(s.sampleRate, kParams[Drive].smoothMs);
  output_.prepare(s.sampleRate, kParams[Output].smoothMs);
  update();
  reset();
}

void Limiter::process(const float* in, float* out, int n) {
  if (!ready_) return clear(out, n);
  leastGain_ = 1.0f;
  for (int i = 0; i < n; ++i) tick(&in[i], &out[i], 1);
}

// Linked stereo when prepared for it; anything else is the mono path on the first channel.
void Limiter::processChannels(const AudioIO& io, int n) {
  if (channels_ < 2 || io.inChannels < 2 || io.outChannels < 2) return Block::processChannels(io, n);
  if (!ready_) return clear(io.out[0], n), clear(io.out[1], n);
  leastGain_ = 1.0f;
  for (int i = 0; i < n; ++i) {
    const float x[kMaxChannels] = {io.in[0][i], io.in[1][i]};
    float y[kMaxChannels];
    tick(x, y, 2);
    io.out[0][i] = y[0];
    io.out[1][i] = y[1];
  }
}

void Limiter::reset() {
  if (!ready_) return;
  for (int c = 0; c < channels_; ++c) clear(delay_[c], latency_), clear(hist_[c], 2 * kTaps);
  delayPos_ = histPos_ = holdHead_ = holdCount_ = boxPos_ = 0;
  for (int i = 0; i < boxLen_; ++i) box_[i] = 1.0f;
  boxSum_ = static_cast<float>(boxLen_);
  now_ = 0;
  released_ = gain_ = leastGain_ = 1.0f;
}

void Limiter::set(ParamId id, float value) {
  if (id >= kParamCount) return;
  value_[id] = kParams[id].clamp(value);
  update();
}

float Limiter::get(ParamId id) const {
  if (id < kParamCount) return value_[id];
  if (id == Reduction) return kParams[Reduction].clamp(-gainToDb(std::fmax(leastGain_, 1e-6f)));
  return NAN;
}

// The windows: the true-peak design's, or (test only) the browser's L-sample ones with L - 1 of delay.
void Limiter::sizeWindows(double sampleRate) {
  if (samplePeak_) {
    lookahead_ = std::max(2, static_cast<int>(std::lround(kBrowserLookaheadMs * 1e-3 * sampleRate)));
    latency_ = lookahead_ - 1;
    holdLen_ = boxLen_ = lookahead_;
    return;
  }
  lookahead_ = static_cast<int>(std::ceil(kLookaheadMs * 1e-3 * sampleRate));
  latency_ = lookahead_ + kTaps / 2;
  holdLen_ = lookahead_ + 2;
  boxLen_ = lookahead_ + 1;
}

// One frame of 1 or 2 channels; x is read before y is written, so they may alias.
void Limiter::tick(const float* x, float* y, int channels) {
  const float drive = drive_.next(), trim = output_.next();
  float clean[kMaxChannels];
  for (int c = 0; c < channels; ++c) clean[c] = sanitize(x[c]) * drive;
  const float peak = detect(clean, channels);
  const float need = peak > ceiling_ ? ceiling_ / peak : 1.0f;
  gain_ = smooth(hold(need));
  leastGain_ = std::fmin(leastGain_, gain_);
  ++now_;
  for (int c = 0; c < channels; ++c) {
    const float delayed = delay_[c][delayPos_];
    delay_[c][delayPos_] = clean[c];
    y[c] = std::fmin(std::fmax(delayed * gain_, -ceiling_), ceiling_) * trim;
  }
  delayPos_ = delayPos_ + 1 == latency_ ? 0 : delayPos_ + 1;
}

// The estimated true peak over the channels, with the margin.
float Limiter::detect(const float* x, int channels) {
  if (samplePeak_) {
    float peak = 0.0f;
    for (int c = 0; c < channels; ++c) peak = std::fmax(peak, std::fabs(x[c]));
    return peak;
  }
  for (int c = 0; c < channels; ++c) hist_[c][histPos_] = hist_[c][histPos_ + kTaps] = x[c];
  histPos_ = histPos_ + 1 == kTaps ? 0 : histPos_ + 1;
  float peak = 0.0f;
  for (int c = 0; c < channels; ++c) peak = std::fmax(peak, truePeak(hist_[c] + histPos_));
  return peak * margin_;
}

// The stretch between the sample kTaps/2 back and the one after it (window oldest first): the
// sample peak plus twice the excess of the interpolated points over it.
float Limiter::truePeak(const float* w) const {
  const float samplePeak = std::fmax(std::fabs(w[kTaps / 2 - 1]), std::fabs(w[kTaps / 2]));
  float acc[kPhases + 1] = {};  // the padding lane has zero taps and stays 0
  for (int k = 0; k < kTaps; ++k)
    for (int p = 0; p <= kPhases; ++p) acc[p] += w[k] * fir_[k][p];
  float between = 0.0f;
  for (float a : acc) between = std::fmax(between, std::fabs(a));
  return samplePeak + kExcessWeight * std::fmax(between - samplePeak, 0.0f);
}

// Sliding minimum over the last holdLen_ values (monotonic deque in a ring).
float Limiter::hold(float need) {
  if (holdCount_ > 0 && now_ - holdAt_[holdHead_] >= static_cast<std::uint32_t>(holdLen_)) {
    holdHead_ = holdHead_ + 1 == holdLen_ ? 0 : holdHead_ + 1;
    --holdCount_;
  }
  auto slot = [this](int i) { return holdHead_ + i >= holdLen_ ? holdHead_ + i - holdLen_ : holdHead_ + i; };
  while (holdCount_ > 0 && holdVal_[slot(holdCount_ - 1)] >= need) --holdCount_;
  const int back = slot(holdCount_++);
  holdVal_[back] = need;
  holdAt_[back] = now_;
  return holdVal_[holdHead_];
}

// Instant attack to the held gain, exponential release towards it, then the moving average.
// The sum is recomputed once per lap of the ring so rounding cannot accumulate.
float Limiter::smooth(float held) {
  released_ = held < released_ ? held : released_ + (held - released_) * releaseCoef_;
  boxSum_ += released_ - box_[boxPos_];
  box_[boxPos_] = released_;
  if (++boxPos_ == boxLen_) {
    boxPos_ = 0;
    boxSum_ = 0.0f;
    for (int i = 0; i < boxLen_; ++i) boxSum_ += box_[i];
  }
  return boxSum_ / static_cast<float>(boxLen_);  // a division, so all-ones is exactly 1
}

// Kaiser-windowed sinc (beta 8), one row per fractional position, each normalised to unity
// gain at DC.
void Limiter::designInterpolator() {
  const double pi = 3.14159265358979323846, half = kTaps / 2;
  for (int p = 0; p < kPhases; ++p) {
    const double frac = (p + 1) / double(kPhases + 1);
    double sum = 0.0, h[kTaps];
    for (int k = 0; k < kTaps; ++k) {
      const double u = frac - (k - (half - 1));
      const double sinc = u == 0.0 ? 1.0 : std::sin(pi * u) / (pi * u);
      h[k] = sinc * kaiser(u / half, 8.0);
      sum += h[k];
    }
    for (int k = 0; k < kTaps; ++k) fir_[k][p] = static_cast<float>(h[k] / sum);
  }
}

void Limiter::update() {
  drive_.setTarget(dbToGain(value_[Drive]));
  output_.setTarget(dbToGain(value_[Output]));
  ceiling_ = dbToGain(value_[Ceiling]);
  margin_ = dbToGain(kMarginDb);
  const double samples = value_[Release] * 1e-3 * sampleRate_;
  releaseCoef_ = static_cast<float>(1.0 - std::exp(-1.0 / samples));
}

}  // namespace squatch::dsp
