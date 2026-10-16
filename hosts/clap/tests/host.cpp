// SPDX-License-Identifier: MIT
#include "host.hpp"

#include <algorithm>

namespace host {
namespace {

Host& hostOf(const clap_host_t* h) { return *static_cast<Host*>(h->host_data); }

void log(const clap_host_t* h, clap_log_severity, const char* msg) { hostOf(h).logs.emplace_back(msg ? msg : ""); }
void latencyChanged(const clap_host_t* h) { ++hostOf(h).latencyChanges; }
void rescan(const clap_host_t* h, clap_param_rescan_flags flags) { hostOf(h).rescans += (flags & CLAP_PARAM_RESCAN_VALUES) != 0; }
void clearParam(const clap_host_t*, clap_id, clap_param_clear_flags) {}
void requestFlush(const clap_host_t* h) { ++hostOf(h).flushRequests; }
const char* flavorOf(const clap_host_t* h) { return hostOf(h).flavor; }
const char* realHost(const clap_host_t*) { return "squatch-clap-host"; }

const clap_host_log_t kLog{log};
const clap_host_latency_t kLatency{latencyChanged};
const clap_host_params_t kParams{rescan, clearParam, requestFlush};
const clap_wrapper_host_information_t kWrapped{flavorOf, realHost};

const void* extension(const clap_host_t* h, const char* id) {
  if (!std::strcmp(id, CLAP_EXT_LOG)) return &kLog;
  if (!std::strcmp(id, CLAP_EXT_LATENCY)) return &kLatency;
  if (!std::strcmp(id, CLAP_EXT_PARAMS)) return &kParams;
  if (!std::strcmp(id, CLAP_WRAPPER_HOST_INFORMATION) && hostOf(h).flavor) return &kWrapped;
  return nullptr;
}

void restart(const clap_host_t* h) { ++hostOf(h).restarts; }
void callback(const clap_host_t* h) { ++hostOf(h).callbacks; }
void nothing(const clap_host_t*) {}

int64_t write(const clap_ostream_t* s, const void* buf, uint64_t size) {
  Out& o = *static_cast<Out*>(s->ctx);
  const std::size_t n = std::min<std::size_t>(size, o.chunk);
  o.data.append(static_cast<const char*>(buf), n);
  return static_cast<int64_t>(n);
}

int64_t read(const clap_istream_t* s, void* buf, uint64_t size) {
  In& i = *static_cast<In*>(s->ctx);
  const std::size_t n = std::min<std::size_t>({static_cast<std::size_t>(size), i.chunk, i.data.size() - i.at});
  std::memcpy(buf, i.data.data() + i.at, n);
  i.at += n;
  return static_cast<int64_t>(n);
}

uint32_t size(const clap_input_events_t* l) { return static_cast<uint32_t>(static_cast<Events*>(l->ctx)->values.size()); }
const clap_event_header_t* get(const clap_input_events_t* l, uint32_t i) {
  return &static_cast<Events*>(l->ctx)->values[i].header;
}
// Like a real host, never allocates on the audio thread: a full list refuses the event.
bool push(const clap_output_events_t* l, const clap_event_header_t* e) {
  auto& sent = static_cast<Events*>(l->ctx)->sent;
  if (e->type != CLAP_EVENT_PARAM_VALUE || e->size != sizeof(clap_event_param_value_t)) return true;
  if (sent.size() == sent.capacity()) return false;
  sent.push_back(*reinterpret_cast<const clap_event_param_value_t*>(e));
  return true;
}

}  // namespace

Tally& tally() {
  static Tally t;
  return t;
}

Host::Host() {
  clap = {CLAP_VERSION_INIT, this, "squatch-clap-host", "Squatch Stack", "", "1.0", extension, restart, nothing, callback};
}

bool Host::logged(const char* text) const {
  for (const std::string& l : logs)
    if (l.find(text) != std::string::npos) return true;
  return false;
}

Out::Out(std::size_t maxPerCall) : chunk(maxPerCall) { clap = {this, write}; }
In::In(std::string text, std::size_t maxPerCall) : data(std::move(text)), chunk(maxPerCall) { clap = {this, read}; }

Events::Events() {
  in = {this, size, get};
  out = {this, push};
  values.reserve(64);
  sent.reserve(4096);
}

void Events::add(uint32_t time, clap_id id, double value) {
  clap_event_param_value_t e{{sizeof e, time, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0}, id, nullptr, -1, -1, -1, -1, value};
  values.push_back(e);
}

template <class T>
const T* ext(const clap_plugin_t* p, const char* id) {
  return static_cast<const T*>(p->get_extension(p, id));
}

Plugin::Plugin(const clap_plugin_entry_t* entry, Host& h) {
  const auto* f = static_cast<const clap_plugin_factory_t*>(entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
  p = f ? f->create_plugin(f, &h.clap, f->get_plugin_descriptor(f, 0)->id) : nullptr;
  if (!p || !p->init(p)) return;
  params = ext<clap_plugin_params_t>(p, CLAP_EXT_PARAMS);
  state = ext<clap_plugin_state_t>(p, CLAP_EXT_STATE);
  latency = ext<clap_plugin_latency_t>(p, CLAP_EXT_LATENCY);
  tail = ext<clap_plugin_tail_t>(p, CLAP_EXT_TAIL);
  ports = ext<clap_plugin_audio_ports_t>(p, CLAP_EXT_AUDIO_PORTS);
  configs = ext<clap_plugin_audio_ports_config_t>(p, CLAP_EXT_AUDIO_PORTS_CONFIG);
  configurable = ext<clap_plugin_configurable_audio_ports_t>(p, CLAP_EXT_CONFIGURABLE_AUDIO_PORTS);
}

Plugin::~Plugin() {
  if (p) p->destroy(p);
}

clap_param_info_t Plugin::info(uint32_t index) const {
  clap_param_info_t i{};
  params->get_info(p, index, &i);
  return i;
}

const clap_param_info_t* Plugin::find(const char* name, clap_param_info_t& out) const {
  for (uint32_t k = 0; k < count(); ++k)
    if (out = info(k), !std::strcmp(out.name, name)) return &out;
  return nullptr;
}

clap_id Plugin::bypassId() const {
  for (uint32_t k = 0; k < count(); ++k)
    if (info(k).flags & CLAP_PARAM_IS_BYPASS) return info(k).id;
  return CLAP_INVALID_ID;
}

double Plugin::value(clap_id id) const {
  double v = -1e300;
  params->get_value(p, id, &v);
  return v;
}

std::string Plugin::save(std::size_t chunk) const {
  Out o(chunk);
  return state->save(p, &o.clap) ? o.data : std::string();
}

bool Plugin::load(const std::string& text, std::size_t chunk) const {
  In i(text, chunk);
  return state->load(p, &i.clap);
}

Audio::Audio(int frames, int inChannels, int outChannels, bool inPlace) {
  for (int c = 0; c < 2; ++c) {
    in[c].assign(static_cast<std::size_t>(frames), 0.0f);
    out[c].assign(static_cast<std::size_t>(frames), 0.0f);
    inPtr[c] = in[c].data();
    outPtr[c] = inPlace ? in[c].data() : out[c].data();
  }
  inBuf = {inPtr, nullptr, static_cast<uint32_t>(inChannels), 0, 0};
  outBuf = {outPtr, nullptr, static_cast<uint32_t>(outChannels), 0, 0};
  process.steady_time = -1;
  process.frames_count = static_cast<uint32_t>(frames);
  process.audio_inputs = &inBuf;
  process.audio_outputs = &outBuf;
  process.audio_inputs_count = inChannels > 0 ? 1 : 0;
  process.audio_outputs_count = 1;
}

}  // namespace host
