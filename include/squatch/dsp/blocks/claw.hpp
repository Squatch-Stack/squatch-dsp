// SPDX-License-Identifier: MIT
// Squatch Claw: the Squatch Claw kit board (an op-amp distortion with hard diode clipping) as a circuit
// model, stage by stage from the board's own schematic, in volts: 1.0 at the input is 1 V.
//
//   input coupling   22 nF into 1 MOhm to the bias rail (7.2 Hz high-pass)
//   LM308 stage      the op-amp as a saturating integrator (its gain-bandwidth, slew and rails)
//                    in the loop with its real feedback network: 47 k || 100 p to the inverting
//                    node, the Distortion rheostat in series with 4.7 u || 1 n from there to the
//                    bias rail. Solved implicitly each step, so when it saturates the gain-leg cap
//                    still sees the clamped output, as on the board
//   clipper          1 k into the 1N4148 pair to the bias rail, solved each sample (Newton, the
//                    SPICE model's Is and N) together with its load: 100 nF, the Filter rheostat,
//                    3.3 nF and the 100 k Volume pot
//   output           the Volume wiper (A taper) into the 2N5457 source follower (its square law,
//                    so hard negative swings cut it off as on the board) and the output coupling
//                    (1 uF into 100 k), solved together
//
// The op-amp stage, the clipper and the filter run at 4x oversampling (Crunch's 8th-order
// Butterworth up and down). The LM308's gain-bandwidth, slew and swing are as it behaves with the
// board's 30 pF in the board's SPICE deck, and the diode and JFET parameters are the deck's models.
// The block is checked against renders of that deck (tests/test_claw.cpp; the renders stay private,
// as the op-amp model they use may not be published).
//
// Parameters: contracts/claw.params.json. set()/get() take the parameter INDEX (Distortion 0, Filter 1,
// Volume 2), not the contract's stable id (1, 2, 3); a caller holding stable ids resolves them with
// findStable() first (docs/CLAW-FILTER-2026-10.md). Meters: output peak (dBFS), and whether the diodes
// conducted hard (|v| > 0.5 V) in the last call.
#pragma once
#include <squatch/dsp/block.hpp>
#include <squatch/dsp/contracts/claw.hpp>
#include <squatch/dsp/param.hpp>

namespace squatch::dsp {

class Claw final : public Block, public contracts::ClawContract {
 public:
  static constexpr int kOversample = 4;
  static constexpr int kControlRate = 16;

  Claw();
  void prepare(const Setup& s, Arena& mem) override;
  void process(const float* in, float* out, int n) override;
  void reset() override;
  void set(ParamId id, float value) override;
  float get(ParamId id) const override;
  Span<const ParamDesc> parameters() const override { return params(); }
  const BlockInfo& info() const override { return kInfo; }

 private:
  struct Svf {  // Crunch's low-pass section, for the oversampling filters
    float a1 = 1.0f, a2 = 0.0f, a3 = 0.0f, ic1 = 0.0f, ic2 = 0.0f;
    void design(double fc, double fs, double q);
    float run(float x) {
      const float v3 = x - ic2, v1 = a1 * ic1 + a2 * v3, v2 = ic2 + a2 * ic1 + a3 * v3;
      ic1 = 2.0f * v1 - ic1;
      ic2 = 2.0f * v2 - ic2;
      return v2;
    }
  };

  bool smoothing() const;
  void design();
  float sample(float x);
  double stageOut(double v);  // the LM308 stage at the oversampled rate
  double clip(double vo);     // the clipper and its load; returns the filter's output node
  double buffer(double vg);   // the JFET source follower and output coupling; returns the output

  Smoother p_[kParamCount];
  double fs_ = 48000.0, T_ = 1.0 / (48000.0 * kOversample);
  // input coupling (base rate); the JFET buffer and output coupling (oversampled)
  double inA_ = 0.0, inX_ = 0.0, inY_ = 0.0;
  double vs_ = 0.0, vout_ = 0.0, hout_ = 0.0, gout_ = 0.0;
  // the LM308 stage: the op-amp output (its integrator's state), the last input difference, the
  // two capacitors' companion histories and conductances, and the Distortion rheostat
  double vo_ = 0.0, ediff_ = 0.0, hcf_ = 0.0, hcg_ = 0.0, gcf_ = 0.0, gcg_ = 0.0, rd_ = 100.0e3;
  // clipper and filter network: node voltages and capacitor companion histories
  double vcl_ = 0.0, vclP_ = 0.0, f2_ = 0.0, h15_ = 0.0, h16_ = 0.0;
  // the 4.5 V bias rail's deviation, which the clipping current moves, and what the input coupling
  // passes of it to the op-amp's input (a highpass at the oversampled rate); the two currents the
  // stage and the clipper returned to it on the last step
  double vb_ = 0.0, vbX_ = 0.0, vbY_ = 0.0, vbA_ = 0.0, ig_ = 0.0, id_ = 0.0;
  double g15_ = 0.0, g16_ = 0.0, gf_ = 0.0, gv_ = 0.0;
  double volFrac_ = 1.0;
  // per-setting constants of the stage's and the clipper's solves (design())
  double sK_ = 0.0, sGf_ = 0.0, sHg_ = 0.0, sGg_ = 0.0, sLin_ = 0.0, sSat_ = 0.0;
  double cA1_ = 0.0, cA2_ = 0.0, cA_ = 0.0, cB11_ = 0.0, cB12_ = 0.0, cB21_ = 0.0, cB22_ = 0.0;
  Svf up_[4], down_[4];
  float peak_ = 0.0f;
  bool clipped_ = false;
};

}  // namespace squatch::dsp
