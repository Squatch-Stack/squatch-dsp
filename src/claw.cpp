// SPDX-License-Identifier: MIT
#include <cmath>
#include <squatch/dsp/blocks/claw.hpp>

namespace squatch::dsp {
namespace {

constexpr double kPi = 3.14159265358979323846;
// The board's component values.
constexpr double kRin = 1.0e6, kCin = 22.0e-9;          // input coupling to the bias rail
constexpr double kRf = 47.0e3, kCf = 100.0e-12;         // LM308 feedback
constexpr double kCg = 4.7e-6 + 1.0e-9;                 // gain leg: 4.7 u || 1 n
constexpr double kRdistMax = 100.0e3, kRdistMin = 10.0; // C100k rheostat
constexpr double kRo = 1.0e3;                           // into the diodes
constexpr double kC15 = 100.0e-9, kC16 = 3.3e-9;        // filter network
constexpr double kRfiltMax = 100.0e3, kRfiltMin = 1.0;  // B100k rheostat
constexpr double kRvol = 100.0e3;                       // A100k volume pot, end to end
constexpr double kCout = 1.0e-6, kRload = 100.0e3;      // output coupling
// The bias rail: 100 k / 100 k from 9 V, decoupled by 100 u. Everything in the stage and the diodes
// returns to it, so the clipping current (up to 3 mA) moves it by up to 0.1 V, and the input
// coupling sets the op-amp's input against it: the rail's sag feeds back into the gain stage.
// Leaving it out left the model saturated milliseconds longer than the board at full gain.
constexpr double kRbias = 50.0e3, kCbias = 100.0e-6;
// The LM308 with the board's 30 pF compensation: a 600 kHz gain-bandwidth and a 0.2 V/us slew rate,
// as it behaves in the board's SPICE deck.
constexpr double kGbw = 2.0 * kPi * 600.0e3;            // rad/s
constexpr double kSlew = 0.2001e6;                      // V/s
// Its output swing: +3.58 / -3.62 V about the bias in the SPICE trace of the board at full gain
// (plugins/spice_rat.py, case 3).
constexpr double kRail = 3.6;                           // V about the bias
// 1N4148 as the deck models it (D1N4148_ADI): Is, N; thermal voltage at ngspice's 27 C.
constexpr double kIs = 2.52e-9, kNvt = 1.752 * 0.025865;
// The 2N5457 source follower (the deck's 2N5457 model): drain current beta (Vgs - Vto)^2 into the
// 10 k source resistor and, through the 1 uF output coupling, the 100 k load. The gate idles at
// 0 V and the source about 1.06 V above it, so a swing below about -1.37 V cuts it off: the board
// clamps hard negative peaks there.
constexpr double kBeta = 1.125e-3, kVto = -1.372, kRs = 10.0e3;
// 8th-order Butterworth as four sections (Crunch's).
constexpr double kButterQ[4] = {0.50979558, 0.60134489, 0.89997622, 2.56291545};

}  // namespace

void Claw::Svf::design(double fc, double fs, double q) {
  const double g = std::tan(kPi * fc / fs), k = 1.0 / q;
  const double d1 = 1.0 / (1.0 + g * (g + k));
  a1 = static_cast<float>(d1);
  a2 = static_cast<float>(g * d1);
  a3 = static_cast<float>(g * g * d1);
}

Claw::Claw() {
  for (int k = 0; k < kParamCount; ++k) p_[k].jump(kParams[k].def);
  design();
}

void Claw::prepare(const Setup& s, Arena&) {
  fs_ = s.sampleRate;
  T_ = 1.0 / (fs_ * kOversample);
  for (int k = 0; k < kParamCount; ++k) p_[k].prepare(s.sampleRate, kParams[k].smoothMs);
  const double os = fs_ * kOversample, fc = 0.4 * fs_;
  for (int k = 0; k < 4; ++k) {
    up_[k].design(fc, os, kButterQ[k]);
    down_[k].design(fc, os, kButterQ[k]);
  }
  const double K = 2.0 * fs_;
  const double wi = 1.0 / (kRin * kCin);
  inA_ = K / (K + wi);
  vbA_ = 2.0 / T_ / (2.0 / T_ + wi);
  gout_ = 2.0 * kCout / T_;
  gcf_ = 2.0 * kCf / T_;
  gcg_ = 2.0 * kCg / T_;
  g15_ = 2.0 * kC15 / T_;
  g16_ = 2.0 * kC16 / T_;
  gv_ = 1.0 / kRvol;
  design();
  reset();
}

void Claw::reset() {
  inX_ = inY_ = 0.0;
  // The source's quiescent point: beta Rs u^2 + u = -Vto with u = Vgs - Vto.
  const double br = kBeta * kRs;
  const double u = (-1.0 + std::sqrt(1.0 + 4.0 * br * -kVto)) / (2.0 * br);
  vs_ = br * u * u;
  vout_ = 0.0;
  hout_ = gout_ * vs_;  // the coupling cap charged to the quiescent source voltage, no current
  vo_ = ediff_ = hcf_ = hcg_ = 0.0;
  vcl_ = vclP_ = f2_ = h15_ = h16_ = 0.0;
  vb_ = vbX_ = vbY_ = ig_ = id_ = 0.0;
  for (int k = 0; k < 4; ++k) up_[k].ic1 = up_[k].ic2 = down_[k].ic1 = down_[k].ic2 = 0.0f;
  peak_ = 0.0f;
  clipped_ = false;
}

void Claw::design() {
  rd_ = std::fmax(kRdistMax * std::pow(10.0, -3.0 * p_[Distortion].value()), kRdistMin);
  gf_ = 1.0 / std::fmax(kRfiltMax * p_[Filter].value(), kRfiltMin);
  volFrac_ = (std::pow(81.0, static_cast<double>(p_[Volume].value())) - 1.0) / 80.0;
  // What stageOut and clip solve each step, with everything that only the settings move folded in.
  const double gf = 1.0 / kRf + gcf_, k = 0.5 * T_ * kGbw;
  sK_ = k;
  sGf_ = gf;
  sHg_ = 1.0 / (1.0 + gcg_ * rd_);
  sGg_ = gcg_ * sHg_;
  sLin_ = 1.0 / (gf * (1.0 + k) + sGg_);
  sSat_ = 1.0 / (gf + sGg_);
  const double m11 = g15_ + gf_, m12 = -gf_, m22 = gf_ + g16_ + gv_, det = m11 * m22 - m12 * m12;
  cA1_ = m22 * g15_ / det;
  cA2_ = -m12 * g15_ / det;
  cB11_ = -m22 / det;  // b1 = cB11 h15 + cB12 h16
  cB12_ = -m12 / det;
  cB21_ = m12 / det;   // b2 = cB21 h15 + cB22 h16
  cB22_ = m11 / det;
  cA_ = g15_ * (1.0 - cA1_);
}

// The LM308 stage, in the time domain, so the feedback network always sees the op-amp's real
// output. The op-amp is an integrator of gain-bandwidth GBW (dVo/dt = GBW (Vp - Vn)) that stops at
// its rails and cannot move faster than its slew; the network is Rf || Cf from the output to the
// inverting node, and Cg in series with the Distortion rheostat from there to the bias rail. When
// the op-amp saturates, Cg keeps charging from the clamped output, which pulls the inverting node
// and so the clipping's duty cycle back toward 50 %, as on the board. Trapezoidal throughout.
double Claw::stageOut(double vp) {
  const double gf = sGf_;                             // Rf || Cf, as a conductance
  const double gg = sGg_;                             // Cg into Rd, seen from the inverting node
  const double hg = hcg_ * sHg_;
  const double k = sK_;
  // KCL at Vn: gf (Vo - Vn) - hcf = gg Vn - hg; integrator: Vo = vo_ + k (vp - Vn + ediff_)
  const double c0 = vo_ + k * (vp + ediff_);          // Vo = c0 - k Vn
  double vn = (gf * c0 - hcf_ + hg) * sLin_;
  double vo = c0 - k * vn;
  const double step = kSlew * T_;
  const double lim = std::fmin(kRail - vb_, vo_ + step), low = std::fmax(-kRail - vb_, vo_ - step);
  if (vo > lim || vo < low) {                         // saturated or slewing: Vo is fixed
    vo = vo > lim ? lim : low;
    vn = (gf * vo - hcf_ + hg) * sSat_;
  }
  ediff_ = vp - vn;
  const double icf = gcf_ * (vo - vn) - hcf_;
  const double ig = gg * vn - hg;
  hcf_ = gcf_ * (vo - vn) + icf;
  const double vcg = vn - rd_ * ig;                   // across Cg
  hcg_ = gcg_ * vcg + ig;
  ig_ = ig;
  vo_ = vo;
  return vo;
}

// The clipper node CL (1 k from the op-amp, the 1N4148 pair to the bias rail) and its load, the
// filter network (C15 to F1, the Filter rheostat to F2, C16 and the Volume pot to ground), by
// trapezoidal companion models. The network is linear, so the current it takes from CL is
// a V + b for this step; Newton then solves CL's one equation with the diodes.
double Claw::clip(double vo) {
  // [F1, F2] = alpha V + beta from  [[g15 + gf, -gf], [-gf, gf + g16 + gv]] [F1 F2] = [g15 V - h15, h16]
  const double a1 = cA1_, a2 = cA2_;
  const double b1 = cB11_ * h15_ + cB12_ * h16_, b2 = cB21_ * h15_ + cB22_ * h16_;
  // Those are against ground; CL is solved against the bias rail, which sits vb above it.
  const double a = cA_, b = -g15_ * b1 - h15_ + a * vb_;
  // Start from the line through the last two solutions: one step usually lands within tolerance.
  double v = 2.0 * vcl_ - vclP_;
  vclP_ = vcl_;
  for (int it = 0; it < 12; ++it) {
    const double e = std::exp(v * (1.0 / kNvt)), ie = 1.0 / e;
    const double f = (vo - v) * (1.0 / kRo) - kIs * (e - ie) - a * v - b;
    const double df = -(1.0 / kRo) - kIs * (1.0 / kNvt) * (e + ie) - a;
    double dv = -f / df;
    dv = dv > 0.1 ? 0.1 : (dv < -0.1 ? -0.1 : dv);
    v += dv;
    if (std::fabs(dv) < 1e-7) break;
  }
  vcl_ = v;
  id_ = (vo - v) / kRo - a * v - b;  // what the diodes take, by KCL: no third exponential
  clipped_ = clipped_ || std::fabs(v) > 0.5;
  const double va = v + vb_;  // CL against ground
  const double f1 = a1 * va + b1, f2 = a2 * va + b2;
  const double i15 = a * v + b, i16 = g16_ * f2 - h16_;
  h15_ = g15_ * (va - f1) + i15;
  h16_ = g16_ * f2 + i16;
  f2_ = f2;
  return f2;
}

// The source node: beta (Vg - Vs - Vto)^2 = Vs / Rs + i_c, with the coupling cap (trapezoidal
// companion, conductance gout) into the load: i_c = (gout (Vs - Vout) - hout) and Vout = i_c Rload,
// so i_c = (gout Vs - hout) / (1 + gout Rload). With u = Vg - Vs - Vto that is the quadratic
// beta u^2 + G u - c = 0, G = 1 / Rs + gc, c = G (Vg - Vto) - hc; c <= 0 is cut off (u = 0).
double Claw::buffer(double vg) {
  const double gc = gout_ / (1.0 + gout_ * kRload), hc = hout_ / (1.0 + gout_ * kRload);
  const double G = 1.0 / kRs + gc, w = vg - kVto, c = G * w - hc;
  const double u = c > 0.0 ? 2.0 * c / (G + std::sqrt(G * G + 4.0 * kBeta * c)) : 0.0;
  const double vs = c > 0.0 ? w - u : hc / G;
  vs_ = vs;
  const double ic = gc * vs - hc;
  vout_ = ic * kRload;
  hout_ = gout_ * (vs - vout_) + ic;
  return vout_;
}

float Claw::sample(float x) {
  inY_ = inA_ * (inY_ + x - inX_);
  inX_ = x;
  const float in = static_cast<float>(inY_);
  double y = 0.0;
  for (int j = 0; j < kOversample; ++j) {
    float u = j == 0 ? in * static_cast<float>(kOversample) : 0.0f;
    for (Svf& f : up_) u = f.run(u);
    // the op-amp's input is the coupled guitar less what the coupling passes of the rail's sag
    vbY_ = vbA_ * (vbY_ + vb_ - vbX_);
    vbX_ = vb_;
    const double vo = stageOut(u - vbY_), f2 = clip(vo);
    vb_ += T_ / kCbias * (id_ + ig_ - vb_ / kRbias);
    float s = static_cast<float>(buffer(volFrac_ * f2));
#ifdef CLAW_TRACE
    CLAW_TRACE(vo, vcl_, f2, vs_, vout_);
#endif
    for (Svf& f : down_) s = f.run(s);
    if (j == 0) y = s;
  }
  return static_cast<float>(y);
}

void Claw::process(const float* in, float* out, int n) {
  float peak = 0.0f;
  clipped_ = false;
  for (int done = 0; done < n;) {
    const bool moving = smoothing();
    const int len = moving ? (n - done < kControlRate ? n - done : kControlRate) : n - done;
    if (moving) {
      for (Smoother& s : p_) s.advance(len);
      design();
    }
    for (int i = done; i < done + len; ++i) {
      out[i] = sample(in[i]);
      peak = std::fmax(peak, std::fabs(out[i]));
    }
    done += len;
  }
  peak_ = peak;
}

void Claw::set(ParamId id, float value) {
  if (id >= kParamCount) return;
  p_[id].setTarget(kParams[id].clamp(value));
  if (!smoothing()) design();
}

float Claw::get(ParamId id) const {
  if (id < kParamCount) return p_[id].target();
  if (id == Out) return kParams[Out].clamp(peak_ > 0.0f ? 20.0f * std::log10(peak_) : kParams[Out].min);
  if (id == Clip) return clipped_ ? 1.0f : 0.0f;
  return NAN;
}

bool Claw::smoothing() const {
  for (const Smoother& s : p_)
    if (s.smoothing()) return true;
  return false;
}

}  // namespace squatch::dsp
