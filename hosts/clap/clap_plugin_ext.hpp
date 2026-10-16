// SPDX-License-Identifier: MIT
// ClapPlugin<B>'s extensions (params, audio ports and their configurations, state, latency,
// tail), and the factory and entry point one plugin exports. Included by clap_plugin.hpp.
#pragma once
#include <climits>

namespace squatch::clap {

template <class B>
bool ClapPlugin<B>::paramInfo(const clap_plugin_t*, uint32_t index, clap_param_info_t* info) {
  if (index >= static_cast<uint32_t>(kCount) || !info) return false;
  fillInfo(params()[index], info);
  return true;
}

template <class B>
bool ClapPlugin<B>::paramValue(const clap_plugin_t* p, clap_id id, double* value) {
  const dsp::ParamDesc* d = byStableId(id);
  if (!d || !value) return false;
  *value = toHost(*d, self(p).value(*d));
  return true;
}

template <class B>
bool ClapPlugin<B>::paramText(const clap_plugin_t*, clap_id id, double value, char* out, uint32_t size) {
  const dsp::ParamDesc* d = byStableId(id);
  if (!d || !out || size == 0) return false;
  format(*d, value, out, size);
  return true;
}

template <class B>
bool ClapPlugin<B>::paramParse(const clap_plugin_t*, clap_id id, const char* text, double* value) {
  const dsp::ParamDesc* d = byStableId(id);
  return d && value && parse(*d, text, *value);
}

// Audio thread when active, main thread when not: either way nothing else is processing.
template <class B>
void ClapPlugin<B>::paramFlush(const clap_plugin_t* p, const clap_input_events_t* in, const clap_output_events_t*) {
  ClapPlugin& s = self(p);
  if (s.active_ && s.resync_.exchange(false, std::memory_order_acquire)) s.applyAll();
  const uint32_t n = in ? in->size(in) : 0;
  for (uint32_t i = 0; i < n; ++i) s.apply(in->get(in, i));
}

inline void fillPort(const PortConfig& c, bool input, clap_audio_port_info_t* info) {
  const int channels = input ? c.in : c.out;
  info->id = input ? 0 : 1;
  copyName(info->name, sizeof info->name, input ? "Input" : "Output");
  info->flags = CLAP_AUDIO_PORT_IS_MAIN;
  info->channel_count = static_cast<uint32_t>(channels);
  info->port_type = channels == 2 ? CLAP_PORT_STEREO : CLAP_PORT_MONO;
  info->in_place_pair = c.in == c.out ? (input ? 1 : 0) : CLAP_INVALID_ID;
}

template <class B>
bool ClapPlugin<B>::portInfo(const clap_plugin_t* p, uint32_t index, bool input, clap_audio_port_info_t* info) {
  if (index != 0 || !info) return false;
  fillPort(kConfigs[self(p).config_], input, info);
  return true;
}

template <class B>
bool ClapPlugin<B>::configGet(const clap_plugin_t*, uint32_t index, clap_audio_ports_config_t* config) {
  if (index >= 3 || !config) return false;
  const PortConfig& c = kConfigs[index];
  config->id = c.id;
  copyName(config->name, sizeof config->name, c.name);
  config->input_port_count = config->output_port_count = 1;
  config->has_main_input = config->has_main_output = true;
  config->main_input_channel_count = static_cast<uint32_t>(c.in);
  config->main_output_channel_count = static_cast<uint32_t>(c.out);
  config->main_input_port_type = c.in == 2 ? CLAP_PORT_STEREO : CLAP_PORT_MONO;
  config->main_output_port_type = c.out == 2 ? CLAP_PORT_STEREO : CLAP_PORT_MONO;
  return true;
}

template <class B>
bool ClapPlugin<B>::configSelect(const clap_plugin_t* p, clap_id id) {
  ClapPlugin& s = self(p);
  if (s.active_ || id >= 3) return false;
  s.config_ = static_cast<int>(id);
  return true;
}

template <class B>
bool ClapPlugin<B>::configPort(const clap_plugin_t*, clap_id id, uint32_t index, bool input, clap_audio_port_info_t* info) {
  if (id >= 3 || index != 0 || !info) return false;
  fillPort(kConfigs[id], input, info);
  return true;
}

namespace ext_detail {

/// One main port, mono or stereo, with a port type (if any) that agrees with its channel count.
inline bool acceptable(const clap_audio_port_configuration_request& r) {
  const uint32_t ch = r.channel_count;
  if (r.port_index != 0 || ch < 1 || ch > 2) return false;
  return !r.port_type || std::strcmp(r.port_type, ch == 2 ? CLAP_PORT_STEREO : CLAP_PORT_MONO) == 0;
}

/// The configuration a set of requests asks for, starting from `current`; -1 if it is not ours.
inline int requested(int current, const clap_audio_port_configuration_request* r, uint32_t n) {
  int in = kConfigs[current].in, out = kConfigs[current].out;
  for (uint32_t i = 0; r && i < n; ++i) {
    if (!acceptable(r[i])) return -1;
    (r[i].is_input ? in : out) = static_cast<int>(r[i].channel_count);
  }
  for (int k = 0; k < 3; ++k)
    if (kConfigs[k].in == in && kConfigs[k].out == out) return k;
  return -1;
}

}  // namespace ext_detail

template <class B>
bool ClapPlugin<B>::canConfigure(const clap_plugin_t* p, const clap_audio_port_configuration_request* r, uint32_t n) {
  const ClapPlugin& s = self(p);
  return !s.active_ && ext_detail::requested(s.config_, r, n) >= 0;
}

template <class B>
bool ClapPlugin<B>::configure(const clap_plugin_t* p, const clap_audio_port_configuration_request* r, uint32_t n) {
  ClapPlugin& s = self(p);
  const int k = s.active_ ? -1 : ext_detail::requested(s.config_, r, n);
  if (k >= 0) s.config_ = k;
  return k >= 0;
}

template <class B>
bool ClapPlugin<B>::stateSave(const clap_plugin_t* p, const clap_ostream_t* stream) {
  ClapPlugin& s = self(p);
  if (!stream) return false;
  const StateValues values{s.snapshot(), s.file_, s.learned_.saved()};
  return writeAll(stream, formatState(B::kInfo, params(), values, kFileKey));
}

// Before activate(), this only sets what activate() will use; while active, the audio thread
// picks the values up at its next buffer, and a new file loads in the background.
template <class B>
bool ClapPlugin<B>::stateLoad(const clap_plugin_t* p, const clap_istream_t* stream) {
  ClapPlugin& s = self(p);
  std::string text;
  StateValues loaded;
  if (!stream || !readAll(stream, text) || !parseState(text, B::kInfo, params(), kFileKey, loaded)) return false;
  if (!acceptsLearned(loaded.learned)) return false;
  s.adopt(loaded);
  return true;
}

// Main thread: a spare block, never prepared, must take every learned blob a state carries (a
// block that learns nothing refuses them all), so a load that would fail later on the audio
// thread fails here instead and changes nothing.
template <class B>
bool ClapPlugin<B>::acceptsLearned(const std::vector<std::string>& blobs) {
  std::unique_ptr<B> probe;
  for (const std::string& b : blobs) {
    if (b.empty()) continue;
    if (!probe) probe.reset(new (std::nothrow) B());
    if (!probe || !probe->loadState(b.data(), static_cast<int>(b.size()))) return false;
  }
  return true;
}

template <class B>
void ClapPlugin<B>::adopt(const StateValues& loaded) {
  for (const dsp::ParamDesc& d : params())
    if (d.settable()) setValue(d, loaded.values[d.id]);
  if (hostParams_ && hostParams_->rescan) hostParams_->rescan(host_, CLAP_PARAM_RESCAN_VALUES);
  const bool newFile = kFileKey && loaded.file != file_;
  file_ = loaded.file;
  learned_.load(loaded.learned, active_);
  if (!active_) return;
  resync_.store(true, std::memory_order_release);
  if (newFile) banks_.request(file_, activation_, snapshot());
}

template <class B>
uint32_t ClapPlugin<B>::tail(const clap_plugin_t* p) {
  const int t = self(p).tail_.load(std::memory_order_relaxed);
  return t < 0 ? UINT32_MAX : static_cast<uint32_t>(t);
}

template <class B>
const void* ClapPlugin<B>::extension(const clap_plugin_t*, const char* id) {
  static constexpr clap_plugin_params_t kParams{paramCount, paramInfo, paramValue, paramText, paramParse, paramFlush};
  static constexpr clap_plugin_audio_ports_t kPorts{portCount, portInfo};
  static constexpr clap_plugin_audio_ports_config_t kConfigsExt{configCount, configGet, configSelect};
  static constexpr clap_plugin_audio_ports_config_info_t kConfigInfo{configCurrent, configPort};
  static constexpr clap_plugin_configurable_audio_ports_t kConfigurable{canConfigure, configure};
  static constexpr clap_plugin_state_t kState{stateSave, stateLoad};
  static constexpr clap_plugin_latency_t kLatency{latency};
  static constexpr clap_plugin_tail_t kTail{tail};
  const struct {
    const char* id;
    const void* ext;
  } table[] = {{CLAP_EXT_PARAMS, &kParams},
               {CLAP_EXT_AUDIO_PORTS, &kPorts},
               {CLAP_EXT_AUDIO_PORTS_CONFIG, &kConfigsExt},
               {CLAP_EXT_AUDIO_PORTS_CONFIG_INFO, &kConfigInfo},
               {CLAP_EXT_AUDIO_PORTS_CONFIG_INFO_COMPAT, &kConfigInfo},
               {CLAP_EXT_CONFIGURABLE_AUDIO_PORTS, &kConfigurable},
               {CLAP_EXT_CONFIGURABLE_AUDIO_PORTS_COMPAT, &kConfigurable},
               {CLAP_EXT_STATE, &kState},
               {CLAP_EXT_LATENCY, &kLatency},
               {CLAP_EXT_TAIL, &kTail}};
  for (const auto& e : table)
    if (id && std::strcmp(id, e.id) == 0) return e.ext;
  return nullptr;
}

/// The factory and entry point of one plugin, B described by D.
template <class B, const clap_plugin_descriptor_t* D>
struct Entry {
  static bool init(const char*) { return true; }
  static void deinit() {}
  static uint32_t count(const clap_plugin_factory_t*) { return 1; }
  static const clap_plugin_descriptor_t* descriptor(const clap_plugin_factory_t*, uint32_t index) {
    return index == 0 ? D : nullptr;
  }
  static const clap_plugin_t* create(const clap_plugin_factory_t*, const clap_host_t* host, const char* id) {
    if (!host || !id || std::strcmp(id, D->id) != 0 || !clap_version_is_compatible(host->clap_version)) return nullptr;
    auto* plugin = new (std::nothrow) ClapPlugin<B>(host, D);
    return plugin ? plugin->clap() : nullptr;
  }
  static const void* factory(const char* id) {
    static constexpr clap_plugin_factory_t kFactory{count, descriptor, create};
    return id && std::strcmp(id, CLAP_PLUGIN_FACTORY_ID) == 0 ? &kFactory : nullptr;
  }
  static constexpr clap_plugin_entry_t entry{CLAP_VERSION_INIT, init, deinit, factory};
};

}  // namespace squatch::clap

/// Defines squatch_clap_entry_<KEY>: a plugin for BLOCK with its contract's identity. The
/// per-format entry (hosts/clap/entry.cpp.in) exports it as clap_entry.
#define SQUATCH_CLAP_PLUGIN(KEY, BLOCK, DESCRIPTION, ...)                                                        \
  namespace {                                                                                                  \
  const char* const kFeatures_##KEY[] = {__VA_ARGS__, nullptr};                                                \
  constexpr clap_plugin_descriptor_t kDescriptor_##KEY{                                                        \
      CLAP_VERSION_INIT, BLOCK::kInfo.typeId, BLOCK::kInfo.name, BLOCK::kInfo.vendor, "", "", "",              \
      BLOCK::kInfo.version, DESCRIPTION, kFeatures_##KEY};                                                     \
  }                                                                                                            \
  extern "C" const clap_plugin_entry_t squatch_clap_entry_##KEY =                                              \
      squatch::clap::Entry<BLOCK, &kDescriptor_##KEY>::entry;
