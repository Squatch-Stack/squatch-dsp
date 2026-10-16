// SPDX-License-Identifier: MIT
// How a contract's parameter appears to a CLAP host (and, through clap-wrapper, to VST3 and AU).
//
// CLAP has no taper: a host draws and automates a parameter linearly between min and max. So a
// linear parameter is exposed in its own units, and a log or exp one (frequencies, times, Q) as
// its 0..1 position, which the host then moves through evenly; the text always shows the value in
// its units. A stepped parameter is exposed in its own units when its steps are whole numbers
// (CLAP_PARAM_IS_STEPPED requires integers), and as its position index otherwise.
#pragma once
#include <clap/ext/params.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <squatch/dsp/param.hpp>

namespace squatch::clap {

inline bool indexed(const dsp::ParamDesc& d) {
  if (d.steps <= 0) return false;
  const float size = (d.max - d.min) / static_cast<float>(d.steps);
  return size != 1.0f || d.min != std::round(d.min);
}

inline bool positional(const dsp::ParamDesc& d) { return d.steps <= 0 && d.taper != dsp::Taper::Linear; }

inline double toHost(const dsp::ParamDesc& d, float plain) {
  if (indexed(d)) return std::round(d.normalize(plain) * static_cast<float>(d.steps));
  return positional(d) ? d.normalize(plain) : d.clamp(plain);
}

inline float fromHost(const dsp::ParamDesc& d, double v) {
  if (indexed(d)) return d.denormalize(static_cast<float>(v) / static_cast<float>(d.steps));
  return positional(d) ? d.denormalize(static_cast<float>(v)) : d.clamp(static_cast<float>(v));
}

inline clap_param_info_flags flagsOf(const dsp::ParamDesc& d) {
  clap_param_info_flags f = d.steps > 0 ? CLAP_PARAM_IS_STEPPED : 0;
  if (d.labels) f |= CLAP_PARAM_IS_ENUM;
  if (d.role == dsp::Role::Meter) return f | CLAP_PARAM_IS_READONLY;
  if (d.role == dsp::Role::Bypass) f |= CLAP_PARAM_IS_BYPASS;
  return f | CLAP_PARAM_IS_AUTOMATABLE;
}

inline void copyName(char* out, std::size_t size, const char* text) {
  std::snprintf(out, size, "%s", text ? text : "");
}

inline void fillInfo(const dsp::ParamDesc& d, clap_param_info_t* info) {
  info->id = d.stableId;
  info->flags = flagsOf(d);
  info->cookie = const_cast<dsp::ParamDesc*>(&d);
  copyName(info->name, sizeof info->name, d.name);
  copyName(info->module, sizeof info->module, d.group);
  info->min_value = toHost(d, d.min);
  info->max_value = toHost(d, d.max);
  info->default_value = toHost(d, d.def);
}

/// "-12.5 dB", "440 Hz", a label, or for the bypass "Bypassed" / "Engaged".
inline void format(const dsp::ParamDesc& d, double hostValue, char* out, std::size_t size) {
  const float v = fromHost(d, hostValue);
  if (d.role == dsp::Role::Bypass) return copyName(out, size, v >= 0.5f ? "Bypassed" : "Engaged");
  if (const char* label = d.label(v)) return copyName(out, size, label);
  const char* space = d.unit && d.unit[0] ? " " : "";
  std::snprintf(out, size, "%.*f%s%s", d.decimals, static_cast<double>(v), space, d.unit ? d.unit : "");
}

namespace text_detail {

inline bool same(const char* a, const char* b) {
  for (; *a && *b; ++a, ++b)
    if ((*a | 0x20) != (*b | 0x20)) return false;
  return *a == *b;
}

inline bool word(const dsp::ParamDesc& d, const char* text, float& v) {
  for (int i = 0; d.labels && i <= d.steps; ++i)
    if (same(text, d.labels[i])) return v = d.min + (d.max - d.min) * static_cast<float>(i) / static_cast<float>(d.steps), true;
  if (d.steps == 1 && (same(text, "bypassed") || same(text, "on"))) return v = d.max, true;
  if (d.steps == 1 && (same(text, "engaged") || same(text, "off"))) return v = d.min, true;
  return false;
}

}  // namespace text_detail

/// The inverse of format(): a label, or a number in the parameter's units (a unit after it is fine).
inline bool parse(const dsp::ParamDesc& d, const char* text, double& hostValue) {
  if (!text) return false;
  float v = 0.0f;
  if (!text_detail::word(d, text, v)) {
    char* end = nullptr;
    v = std::strtof(text, &end);
    if (end == text || !std::isfinite(v)) return false;
  }
  hostValue = toHost(d, v);
  return true;
}

}  // namespace squatch::clap
