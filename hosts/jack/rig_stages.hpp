// SPDX-License-Identifier: MIT
// The live rig's stages, in signal order. No JACK here, so the option parser can be tested on a
// machine without JACK.
#pragma once

namespace squatch::rig {

// The phone app's order, with the tuner after the gate and the Claw (the op-amp distortion) after
// the Od. Comp, Od, Claw, Crunch and Hall start switched off, so a rig started without asking for them runs as before.
enum Stage : int { Gate, Tuner, Comp, Od, Claw, Crunch, Amp, Eq, Cab, Hall, Limiter, kStages };

inline const char* stageName(int s) {
  static const char* const kNames[kStages] = {"gate", "tuner", "comp", "od", "claw", "crunch", "amp", "eq", "cab", "hall", "limiter"};
  return s >= 0 && s < kStages ? kNames[s] : "?";
}

}  // namespace squatch::rig
