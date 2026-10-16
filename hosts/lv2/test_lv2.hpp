// SPDX-License-Identifier: MIT
// The smoke host's own minimal LV2 host (hosts/lv2/test_lv2.cpp): a URID map, dlopen of a
// bundle's binary, an instance with every control port on a float, and the registry through
// which each test_<name>_lv2.cpp adds the checks for its plugin.
#pragma once
#include <dlfcn.h>
#include <lv2/core/lv2.h>
#include <lv2/urid/urid.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace lv2test {

constexpr double kRate = 48000.0;
constexpr double kPi = 3.14159265358979323846;
inline int gChecks = 0, gFailures = 0;

inline void check(bool ok, const char* what, double value) {
  ++gChecks;
  if (!ok) ++gFailures;
  std::printf("%s %s (%.4g)\n", ok ? "ok  " : "FAIL", what, value);
}

// ---- a minimal host ---------------------------------------------------------------------------

inline std::vector<std::string> gUris;
inline LV2_URID mapUri(LV2_URID_Map_Handle, const char* uri) {
  for (std::size_t i = 0; i < gUris.size(); ++i)
    if (gUris[i] == uri) return static_cast<LV2_URID>(i + 1);
  gUris.emplace_back(uri);
  return static_cast<LV2_URID>(gUris.size());
}
inline LV2_URID_Map gMap = {nullptr, mapUri};
inline const LV2_Feature gMapFeature = {LV2_URID__map, &gMap};
inline const LV2_Feature* const gFeatures[] = {&gMapFeature, nullptr};

struct Loaded {
  void* lib = nullptr;
  const LV2_Descriptor* d = nullptr;
};

inline Loaded load(const std::string& dir, const char* bundle, const std::string& ext) {
  Loaded l;
  const std::string path = dir + "/" + bundle + ".lv2/" + bundle + "." + ext;
  l.lib = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (!l.lib) return std::printf("FAIL dlopen %s: %s\n", path.c_str(), dlerror()), l;
  using Fn = const LV2_Descriptor* (*)(uint32_t);
  auto fn = reinterpret_cast<Fn>(dlsym(l.lib, "lv2_descriptor"));
  l.d = fn ? fn(0) : nullptr;
  return l;
}

struct Instance {
  const LV2_Descriptor* d;
  LV2_Handle h;
  std::vector<float> controls;
  unsigned audioIn = 0, audioOut = 0;
  Instance(const LV2_Descriptor* desc, const std::vector<float>& defaults) : d(desc), controls(defaults) {
    h = d->instantiate(d, kRate, "", gFeatures);
  }
  ~Instance() {
    if (h) d->cleanup(h);
  }
  void connect(unsigned in, unsigned out) {
    audioIn = in, audioOut = out;
    for (unsigned i = 0; i < controls.size(); ++i)
      if (i != in && i != out) d->connect_port(h, i, &controls[i]);
  }
  // x through the plugin in host calls of `block` samples, into y.
  void run(const float* x, float* y, std::size_t n, std::size_t block) {
    for (std::size_t i = 0; i < n; i += block) {
      const auto len = static_cast<uint32_t>(std::min(block, n - i));
      d->connect_port(h, audioIn, const_cast<float*>(x + i));
      d->connect_port(h, audioOut, y + i);
      d->run(h, len);
    }
  }
};

template <class Ports>
std::vector<float> defaults(const Ports& ports, unsigned count) {  // every port, audio ones unused
  std::vector<float> v(count);
  for (unsigned i = 0; i < count; ++i) v[i] = ports[i].def;
  return v;
}


inline double amplitudeAt(const float* x, std::size_t n, double hz) {
  double c = 0, s = 0;
  for (std::size_t i = 0; i < n; ++i) {
    const double w = 2 * kPi * hz * static_cast<double>(i) / kRate;
    c += x[i] * std::cos(w), s += x[i] * std::sin(w);
  }
  return 2.0 * std::hypot(c, s) / static_cast<double>(n);
}

inline double db(double v) { return 20.0 * std::log10(v + 1e-30); }

/// One plugin's checks, run when its bundle is in the build directory.
struct PluginTest {
  const char* bundle;  // squatch-<name>: the bundle directory and the binary's base name
  const char* uri;
  void (*run)(const Loaded&);
};

inline std::vector<PluginTest>& pluginTests() {
  static std::vector<PluginTest> tests;
  return tests;
}

/// For a static initialiser in each test_<name>_lv2.cpp.
inline bool addPluginTest(const PluginTest& t) {
  pluginTests().push_back(t);
  return true;
}

}  // namespace lv2test
