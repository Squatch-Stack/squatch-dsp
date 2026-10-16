// SPDX-License-Identifier: MIT
#include <cmath>
#include <squatch/dsp/blocks/hall.hpp>

namespace squatch::dsp {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kLineMs[Hall::kLines] = {29.7, 37.1, 41.1, 43.7, 53.3, 59.9, 67.1, 73.3};
constexpr double kAllpassMs[Hall::kAllpasses] = {4.771, 3.595, 12.73, 9.307};
constexpr float kSignL[Hall::kLines] = {1, -1, 1, -1, 1, -1, 1, -1};
constexpr float kSignR[Hall::kLines] = {1, 1, -1, -1, 1, 1, -1, -1};
constexpr double kMaxSize = 1.6, kMaxPreMs = 150.0;
constexpr float kDiffusion = 0.6f, kOutScale = 0.35f;

int samples(double ms, double fs) { return static_cast<int>(std::lround(ms * fs / 1000.0)); }

}  // namespace

Hall::Hall() {
  for (int k = 0; k < kParamCount; ++k) p_[k].jump(kParams[k].def);
}

void Hall::prepare(const Setup& s, Arena& mem) {
  sampleRate_ = s.sampleRate;
  for (int k = 0; k < kParamCount; ++k) p_[k].prepare(s.sampleRate, kParams[k].smoothMs);
  const auto take = [&](Line& l, int cap) {
    l.cap = cap;
    l.b = mem.alloc<float>(static_cast<std::size_t>(cap));
  };
  for (int k = 0; k < kLines; ++k) take(lines_[k], samples(kLineMs[k] * kMaxSize, sampleRate_) + 2);
  for (int k = 0; k < kAllpasses; ++k) take(aps_[k], samples(kAllpassMs[k] * kMaxSize, sampleRate_) + 2);
  take(pre_, samples(kMaxPreMs, sampleRate_) + 2);
  ready_ = !mem.failed();
  design();
  reset();
}

void Hall::reset() {
  if (!ready_) return;
  for (int i = 0; i < pre_.cap; ++i) pre_.b[i] = 0.0f;
  pre_.w = 0;
  for (Line& l : lines_) {
    for (int i = 0; i < l.cap; ++i) l.b[i] = 0.0f;
    l.w = 0;
  }
  for (Line& l : aps_) {
    for (int i = 0; i < l.cap; ++i) l.b[i] = 0.0f;
    l.w = 0;
  }
  for (float& d : damp_) d = 0.0f;
}

void Hall::design() {
  const double size = p_[Size].value(), decay = p_[Decay].value();
  for (int k = 0; k < kLines; ++k) {
    len_[k] = samples(kLineMs[k] * size, sampleRate_);
    if (len_[k] < 8) len_[k] = 8;
    gain_[k] = static_cast<float>(std::pow(10.0, -3.0 * len_[k] / (decay * sampleRate_)));
  }
  for (int k = 0; k < kAllpasses; ++k) {
    apLen_[k] = samples(kAllpassMs[k] * size, sampleRate_);
    if (apLen_[k] < 1) apLen_[k] = 1;
  }
  preLen_ = samples(p_[Predelay].value(), sampleRate_);
  const double t = std::tan(kPi * std::fmin(static_cast<double>(p_[Damp].value()), 0.49 * sampleRate_) / sampleRate_);
  dampG_ = static_cast<float>(t / (1.0 + t));
}

// The input's way into the tank: the pre-delay, then the diffusing allpasses.
float Hall::diffuse(float dryIn) {
  pre_.write(dryIn);
  float s = preLen_ > 0 ? pre_.read(preLen_) : dryIn;
  for (int a = 0; a < kAllpasses; ++a) {
    const float z = aps_[a].read(apLen_[a]);
    const float v = s + kDiffusion * z;
    aps_[a].write(v);
    s = z - kDiffusion * v;
  }
  return s;
}

// One step of the feedback delay network with `s` fed in: each line damped and scaled, mixed back
// through a Householder reflection. Returns the tank's two output taps, before the wet gain.
Hall::Taps Hall::tank(float s) {
  float x[kLines];
  float sum = 0.0f;
  for (int k = 0; k < kLines; ++k) {
    const float r = lines_[k].read(len_[k]);
    const float v = (r - damp_[k]) * dampG_, lp = v + damp_[k];  // one-pole low-pass, trapezoidal
    damp_[k] = lp + v;
    x[k] = lp * gain_[k];
    sum += x[k];
  }
  const float hh = (2.0f / kLines) * sum;  // Householder reflection
  Taps y{0.0f, 0.0f};
  for (int k = 0; k < kLines; ++k) {
    y.l += kSignL[k] * x[k];
    y.r += kSignR[k] * x[k];
    lines_[k].write(x[k] - hh + s);
  }
  return y;
}

void Hall::render(const float* inL, const float* inR, float* outL, float* outR, int n) {
  for (int done = 0; done < n;) {
    const bool moving = smoothing();
    const int len = moving ? (n - done < kControlRate ? n - done : kControlRate) : n - done;
    if (moving) {
      for (Smoother& s : p_) s.advance(len);
      design();
    }
    const float wet = p_[Mix].value(), dry = 1.0f - wet, width = p_[Width].value();
    for (int i = done; i < done + len; ++i) {
      const float dl = inL[i], dr = inR ? inR[i] : dl;
      const Taps y = tank(diffuse(inR ? 0.5f * (dl + dr) : dl));
      const float mid = 0.5f * (y.l + y.r), side = 0.5f * (y.l - y.r) * width;
      if (outR) outR[i] = dry * dr + wet * kOutScale * (mid - side);
      outL[i] = dry * dl + wet * kOutScale * (mid + side);
    }
    done += len;
  }
}

void Hall::process(const float* in, float* out, int n) {
  if (!ready_) {
    for (int i = 0; i < n; ++i) out[i] = in[i];
    return;
  }
  render(in, nullptr, out, nullptr, n);
}

void Hall::processChannels(const AudioIO& io, int n) {
  if (io.inChannels < 1 || io.outChannels < 1) return;
  const float* inR = io.inChannels > 1 ? io.in[1] : nullptr;
  float* outR = io.outChannels > 1 ? io.out[1] : nullptr;
  if (ready_) {
    render(io.in[0], inR, io.out[0], outR, n);
  } else {  // unprepared: pass through, a mono input to both sides
    copy(io.in[0], io.out[0], n);
    if (outR) copy(inR ? inR : io.in[0], outR, n);
  }
  for (int c = 2; c < io.outChannels; ++c) copy(io.out[1], io.out[c], n);
}

void Hall::set(ParamId id, float value) {
  if (id >= kParamCount) return;
  p_[id].setTarget(kParams[id].clamp(value));
  if (!smoothing()) design();
}

float Hall::get(ParamId id) const { return id < kParamCount ? p_[id].target() : NAN; }

bool Hall::smoothing() const {
  for (const Smoother& s : p_)
    if (s.smoothing()) return true;
  return false;
}

}  // namespace squatch::dsp
