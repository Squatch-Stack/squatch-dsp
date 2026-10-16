// SPDX-License-Identifier: MIT
// A minimal CLAP host for the plugin tests: host callbacks that count what the plugin asks for,
// streams that move at most `chunk` bytes per call, and event lists. No framework.
#pragma once
#include <clap/clap.h>
#include <clapwrapper/wrapper_host.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace host {

// ---- checks, as the repository's tests count them
struct Tally {
  int checks = 0, failures = 0;
};
Tally& tally();

#define HCHECK(cond, ...)                                          \
  do {                                                             \
    ++host::tally().checks;                                        \
    if (!(cond)) {                                                 \
      ++host::tally().failures;                                    \
      std::printf("FAIL %s:%d: %s  ", __FILE__, __LINE__, #cond);  \
      std::printf(__VA_ARGS__);                                    \
      std::printf("\n");                                           \
    }                                                              \
  } while (0)

// ---- the host
struct Host {
  clap_host_t clap{};
  std::atomic<int> restarts{0}, latencyChanges{0}, flushRequests{0}, callbacks{0}, rescans{0};
  std::vector<std::string> logs;
  const char* flavor = nullptr;  // offer clap-wrapper's host information as this flavour

  Host();
  bool logged(const char* text) const;
};

// ---- streams
struct Out {
  clap_ostream_t clap{};
  std::string data;
  std::size_t chunk = 1 << 20;
  explicit Out(std::size_t maxPerCall);
};

struct In {
  clap_istream_t clap{};
  std::string data;
  std::size_t at = 0, chunk = 1 << 20;
  In(std::string text, std::size_t maxPerCall);
};

// ---- events
struct Events {
  clap_input_events_t in{};
  clap_output_events_t out{};
  std::vector<clap_event_param_value_t> values;  // input, in time order
  std::vector<clap_event_param_value_t> sent;    // what the plugin pushed
  Events();
  void add(uint32_t time, clap_id id, double value);
  void clear() { values.clear(), sent.clear(); }
};

// ---- one plugin, from a linked-in entry or a loaded bundle
struct Plugin {
  const clap_plugin_t* p = nullptr;
  const clap_plugin_params_t* params = nullptr;
  const clap_plugin_state_t* state = nullptr;
  const clap_plugin_latency_t* latency = nullptr;
  const clap_plugin_tail_t* tail = nullptr;
  const clap_plugin_audio_ports_t* ports = nullptr;
  const clap_plugin_audio_ports_config_t* configs = nullptr;
  const clap_plugin_configurable_audio_ports_t* configurable = nullptr;

  Plugin(const clap_plugin_entry_t* entry, Host& h);
  ~Plugin();
  Plugin(const Plugin&) = delete;
  Plugin& operator=(const Plugin&) = delete;

  uint32_t count() const { return params->count(p); }
  clap_param_info_t info(uint32_t index) const;
  const clap_param_info_t* find(const char* name, clap_param_info_t& out) const;
  clap_id bypassId() const;
  double value(clap_id id) const;
  std::string save(std::size_t chunk) const;
  bool load(const std::string& text, std::size_t chunk) const;
};

// ---- audio buffers for one process() call
struct Audio {
  std::vector<float> in[2], out[2];
  float* inPtr[2] = {};
  float* outPtr[2] = {};
  clap_audio_buffer_t inBuf{}, outBuf{};
  clap_process_t process{};
  Audio(int frames, int inChannels, int outChannels, bool inPlace);
  void frames(uint32_t n) { process.frames_count = n; }
};

}  // namespace host
