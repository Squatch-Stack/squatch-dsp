// SPDX-License-Identifier: MIT
// Parameter descriptors and the smoother blocks use to move between values without clicks.
//
// A descriptor table is generated from the block's control contract
// (contracts/<key>.params.json, profile cc.squatch.control/1, by tools/build-contracts), so the
// same ids, keys, ranges and tapers reach every format: CLAP/VST3/AU here, and LV2, WAM,
// OSCQuery and MIDI-CI through the squatch-skin emitters.
//
// Version 2 of the descriptor (2026-09) appends fields, so a v1 initialiser
// {id, name, unit, min, max, def, smoothMs} still compiles and means what it did:
// - key: a stable string (the LV2 symbol, the preset key). Presets are stored by key.
// - stableId: the contract's id, never reused: the CLAP param id and the host automation id.
//   `id` stays the dense index a block uses for set() and get().
// - role: Param (set by the host), Bypass (exactly one per plugin; the host implements it and
//   keeps processing), Meter (read-only, published by get()) or Trigger (a 0..1 toggle whose
//   rising edge starts something, such as a learn; a host passes it on only as a change it was
//   sent, never when it restores a state or re-applies values, so loading a preset fires nothing).
// - taper, exponent: how a host's 0..1 control maps to the value (normalize()/denormalize()).
// - steps, labels: 0 is continuous, 1 a toggle, n > 1 has n + 1 positions; labels, when set,
//   names each position (an enumeration). clamp() rounds to the nearest position.
// - shortName, group, decimals, rateHz: display hints, and a meter's publishing rate.
#pragma once
#include <cmath>
#include <cstdint>

#include "span.hpp"

namespace squatch::dsp {

using ParamId = std::uint16_t;

enum class Role : std::uint8_t { Param, Bypass, Meter, Trigger };  // appended to, never reordered
enum class Taper : std::uint8_t { Linear, Log, Exp };

struct ParamDesc {
  ParamId id;
  const char* name;
  const char* unit;  // "dB", "ms", "Hz", "" ...
  float min;
  float max;
  float def;
  float smoothMs;  // 0: the value applies at once
  // v2
  const char* key = "";
  std::uint32_t stableId = 0;
  Role role = Role::Param;
  Taper taper = Taper::Linear;
  float exponent = 1.0f;  // Taper::Exp: value = min + (max - min) * n^exponent
  int steps = 0;
  const char* const* labels = nullptr;  // steps + 1 names, or none
  const char* shortName = "";
  const char* group = "";
  int decimals = 2;
  float rateHz = 0.0f;  // meters: how often a host needs to publish

  /// Inside [min, max] and on a step; NaN becomes the default.
  float clamp(float v) const {
    if (v != v) return def;
    const float c = std::fmin(std::fmax(v, min), max);
    return steps > 0 ? snap(c) : c;
  }

  /// The host's 0..1 position for a value, by the taper.
  float normalize(float v) const {
    const float c = clamp(v);
    if (taper == Taper::Log && min > 0.0f) return std::log(c / min) / std::log(max / min);
    const float lin = (c - min) / (max - min);
    return taper == Taper::Exp && exponent > 0.0f ? std::pow(lin, 1.0f / exponent) : lin;
  }

  /// The value at a 0..1 position (the inverse of normalize(), then clamp()).
  float denormalize(float n) const {
    const float t = n == n ? std::fmin(std::fmax(n, 0.0f), 1.0f) : normalize(def);
    if (taper == Taper::Log && min > 0.0f) return clamp(min * std::pow(max / min, t));
    return clamp(min + (max - min) * (taper == Taper::Exp ? std::pow(t, exponent) : t));
  }

  /// The label of the position nearest v, or nullptr when the parameter has none.
  const char* label(float v) const {
    if (!labels || steps <= 0) return nullptr;
    return labels[position(clamp(v))];
  }

  bool settable() const { return role != Role::Meter; }
  /// Given to a block whenever the host's value changes, restored state included (not a trigger).
  bool applied() const { return role == Role::Param; }

 private:
  int position(float c) const { return static_cast<int>(std::lround((c - min) / (max - min) * steps)); }
  float snap(float c) const { return min + static_cast<float>(position(c)) * (max - min) / static_cast<float>(steps); }
};

inline const ParamDesc* findParam(Span<const ParamDesc> params, ParamId id) {
  for (const ParamDesc& p : params)
    if (p.id == id) return &p;
  return nullptr;
}

/// By the contract's stable id (a host's automation id) or key (a preset's name for it).
inline const ParamDesc* findStable(Span<const ParamDesc> params, std::uint32_t stableId) {
  for (const ParamDesc& p : params)
    if (p.stableId == stableId) return &p;
  return nullptr;
}

inline bool sameKey(const char* a, const char* b) {
  while (*a && *a == *b) ++a, ++b;
  return *a == *b;
}

inline const ParamDesc* findKey(Span<const ParamDesc> params, const char* key) {
  for (const ParamDesc& p : params)
    if (sameKey(p.key, key)) return &p;
  return nullptr;
}

/// Linear ramp to the latest target over a fixed time. Linear rather than one-pole because it
/// arrives exactly, so a block knows when it can stop recomputing coefficients.
class Smoother {
 public:
  void prepare(double sampleRate, float ms) {
    steps_ = static_cast<int>(std::lround(ms * 1e-3 * sampleRate));
    jump(target_);
  }

  void jump(float v) {
    value_ = target_ = v;
    left_ = 0;
  }

  void setTarget(float v) {
    if (v == target_) return;
    if (steps_ <= 0) return jump(v);
    target_ = v;
    left_ = steps_;
    step_ = (target_ - value_) / static_cast<float>(steps_);
  }

  float next() {
    if (left_ > 0) value_ = --left_ == 0 ? target_ : value_ + step_;
    return value_;
  }

  /// Moves n samples along at once, for blocks that update at a control rate.
  float advance(int n) {
    if (left_ <= n) {
      jump(target_);
      return value_;
    }
    left_ -= n;
    value_ += step_ * static_cast<float>(n);
    return value_;
  }

  bool smoothing() const { return left_ > 0; }
  float value() const { return value_; }
  float target() const { return target_; }

 private:
  float value_ = 0.0f;
  float target_ = 0.0f;
  float step_ = 0.0f;
  int left_ = 0;
  int steps_ = 0;
};

}  // namespace squatch::dsp
