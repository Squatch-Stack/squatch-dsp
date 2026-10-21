// SPDX-License-Identifier: MIT
#include <cmath>
#include <squatch/dsp/blocks/gate_stage.hpp>

namespace squatch::dsp {

void GateStage::set(ParamId id, float value) {
  if (!std::isfinite(value)) return;
  if (id == Param::Bypass) bypass_.set(Bypass::Engaged, value >= 0.5f ? 0.0f : 1.0f);
  else gate_.set(id, value);  // the Gate ignores ids it does not have
}

float GateStage::get(ParamId id) const {
  if (id == Param::Bypass) return bypass_.engaged() ? 0.0f : 1.0f;
  return gate_.get(id);
}

}  // namespace squatch::dsp
