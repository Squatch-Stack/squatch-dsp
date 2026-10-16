// SPDX-License-Identifier: MIT
#include <cmath>
#include <squatch/dsp/blocks/trail_od.hpp>

namespace squatch::dsp {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kEdge = 21000.0 / 192000.0;  // the FIR's cutoff, as a fraction of the 4x rate
constexpr double kBeta = 8.6;
constexpr double kVoiceHz = 720.0;  // the browser's Voice default
constexpr double kAsym = 0.35;      // the browser's Asym default
constexpr double kDcHz = 8.0;
constexpr double kRf = 51.0e3, kRfPot = 500.0e3, kCf = 51.0e-12;

// Bessel I0 as the browser's kaiserLowpass sums it (to a relative 1e-12, at most 50 terms).
double besselI0(double x) {
  double sum = 1.0, t = 1.0;
  for (int k = 1; k < 50; ++k) {
    t *= (x / (2.0 * k)) * (x / (2.0 * k));
    sum += t;
    if (t < 1e-12 * sum) break;
  }
  return sum;
}

// Rational tanh, exact saturation at +-1 (the browser's tanhFast).
double tanhFast(double x) {
  if (x > 4.97) return 1.0;
  if (x < -4.97) return -1.0;
  const double x2 = x * x;
  const double a = x * (135135.0 + x2 * (17325.0 + x2 * (378.0 + x2)));
  const double b = 135135.0 + x2 * (62370.0 + x2 * (3150.0 + x2 * 28.0));
  return a / b;
}

}  // namespace

void TrailOD::OnePole::cutoff(double fc, double rate) {
  const double t = std::tan(kPi * std::fmin(fc, 0.49 * rate) / rate);
  g = t / (1.0 + t);
}

double TrailOD::OnePole::lp(double x) {
  const double v = (x - s) * g;
  const double y = v + s;
  s = y + v;
  return y;
}

TrailOD::TrailOD() {
  const double m = (kTaps - 1) / 2.0, denom = besselI0(kBeta);
  double sum = 0.0;
  for (int n = 0; n < kTaps; ++n) {
    const double x = n - m, r = 2.0 * n / (kTaps - 1) - 1.0;
    const double sinc = x == 0.0 ? 2.0 * kEdge : std::sin(2.0 * kPi * kEdge * x) / (kPi * x);
    h_[n] = sinc * (besselI0(kBeta * std::sqrt(std::fmax(0.0, 1.0 - r * r))) / denom);
    sum += h_[n];
  }
  for (double& v : h_) v /= sum;
  for (int k = 0; k < kOversample; ++k)
    for (int j = 0; j < kPerPhase; ++j) {
      const int t = j * kOversample + k;
      phase_[k][j] = t < kTaps ? kOversample * h_[t] : 0.0;
    }
  for (int k = 0; k < kParamCount; ++k) p_[k].jump(kParams[k].def);
  design();
}

void TrailOD::prepare(const Setup& s, Arena&) {
  sampleRate_ = s.sampleRate;
  for (int k = 0; k < kParamCount; ++k) p_[k].prepare(s.sampleRate, kParams[k].smoothMs);
  dcR_ = std::exp(-2.0 * kPi * kDcHz / sampleRate_);
  design();
  reset();
}

void TrailOD::reset() {
  hp_.s = lp_.s = tone_.s = 0.0;
  for (double& v : xHist_) v = 0.0;
  for (double& v : uHist_) v = 0.0;
  xi_ = ui_ = 0;
  dcX1_ = dcY1_ = 0.0;
  peak_ = 0.0f;
  clipped_ = false;
}

// The coefficients for where the smoothers are: gain, bias, level and the three corners.
void TrailOD::design() {
  const double drive = p_[Drive].value(), tone = p_[Tone].value();
  const double fsHi = sampleRate_ * kOversample;
  gain_ = 1.0 + 110.0 * drive * drive;
  const double rf = kRf + drive * drive * kRfPot;
  bias_ = 0.6 * kAsym;
  biasTanh_ = std::tanh(bias_);
  level_ = std::pow(10.0, static_cast<double>(p_[Level].value()) / 20.0);
  hp_.cutoff(kVoiceHz, fsHi);
  lp_.cutoff(std::fmin(1.0 / (2.0 * kPi * rf * kCf), 0.45 * fsHi), fsHi);
  tone_.cutoff(500.0 * std::pow(2.0, tone * 4.5), sampleRate_);
}

// The diode stage at the 4x rate: the unity path plus the clipped, band-limited, boosted branch.
double TrailOD::nonlinear(double u) {
  const double v = lp_.lp(hp_.hp(u) * gain_);
  const double arg = 2.0 * v + bias_;
  clipped_ = clipped_ || std::fabs(arg) > 1.0;
  return u + 0.5 * (tanhFast(arg) - biasTanh_);
}

void TrailOD::up(double x, double* out4) {
  xi_ = (xi_ + 1) % kPerPhase;
  xHist_[xi_] = xHist_[xi_ + kPerPhase] = x;
  const int base = xi_ + kPerPhase;
  for (int k = 0; k < kOversample; ++k) {
    double s = 0.0;
    for (int j = 0; j < kPerPhase; ++j) s += phase_[k][j] * xHist_[base - j];
    out4[k] = s;
  }
}

double TrailOD::down(const double* in4) {
  for (int k = 0; k < kOversample; ++k) {
    ui_ = (ui_ + 1) % kTaps;
    uHist_[ui_] = uHist_[ui_ + kTaps] = in4[k];
  }
  const int base = ui_ + kTaps;
  double s = 0.0;
  for (int t = 0; t < kTaps; ++t) s += h_[t] * uHist_[base - t];
  return s;
}

// One base-rate sample through up, the stage, down, the DC block, Tone and Level.
double TrailOD::sample(float x) {
  double u4[kOversample], y4[kOversample];
  up(x, u4);
  for (int k = 0; k < kOversample; ++k) y4[k] = nonlinear(u4[k]);
  const double s = down(y4);
  const double d = s - dcX1_ + dcR_ * dcY1_;
  dcX1_ = s;
  dcY1_ = d;
  return tone_.lp(d) * level_;
}

void TrailOD::process(const float* in, float* out, int n) {
  float peak = 0.0f;
  clipped_ = false;
  for (int done = 0; done < n;) {
    const int len = smoothing() ? (n - done < kControlRate ? n - done : kControlRate) : n - done;
    if (smoothing()) advance(len);
    for (int i = done; i < done + len; ++i) {
      out[i] = static_cast<float>(sample(in[i]));
      peak = std::fmax(peak, std::fabs(out[i]));
    }
    done += len;
  }
  peak_ = peak;
}

void TrailOD::set(ParamId id, float value) {
  if (id >= kParamCount) return;
  p_[id].setTarget(kParams[id].clamp(value));
  if (!smoothing()) design();
}

float TrailOD::get(ParamId id) const {
  if (id < kParamCount) return p_[id].target();
  if (id == Out) return kParams[Out].clamp(peak_ > 0.0f ? 20.0f * std::log10(peak_) : kParams[Out].min);
  if (id == Clip) return clipped_ ? 1.0f : 0.0f;
  return NAN;
}

bool TrailOD::smoothing() const {
  for (const Smoother& s : p_)
    if (s.smoothing()) return true;
  return false;
}

// Moves the smoothers to the end of this sub-block and designs for where they land.
void TrailOD::advance(int n) {
  for (Smoother& s : p_) s.advance(n);
  design();
}

}  // namespace squatch::dsp
