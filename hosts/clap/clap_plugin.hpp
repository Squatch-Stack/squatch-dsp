// SPDX-License-Identifier: MIT
// ClapPlugin<B>: any squatch-dsp Block as a CLAP plugin (CLAP 1.2.x), and through clap-wrapper as
// VST3 and AUv2. Parameters, identity and version come from the block's contract.
//
// What it guarantees, from bugs other plugins shipped (research notes, frameworks review 2.1):
//  1. Mono, mono->stereo and stereo main buses (configurable-audio-ports, audio-ports-config);
//     the channel counts of every buffer are read from that buffer.
//  2. Exactly one bypass parameter (CLAP_PARAM_IS_BYPASS); processing continues while bypassed.
//  3. Latency is fixed by activate() and reported to the host then; if it ever changed while
//     active, the plugin asks for a restart rather than reporting a new figure.
//  4. State is versioned text, written and read in loops over partial transfers, and may be
//     loaded before the first activate() (Logic restores before Initialize). A load tells the
//     host to re-read every value (params rescan), so no host keeps showing the old ones.
//  5. params.flush works while inactive (it only updates the values activate() will use).
//  6. deactivate() without stop_processing() is tolerated.
//  7. frames_count is at most the activated maximum, but a larger call is split, not trusted.
//  8. clap-wrapper's host-information extension is read at init (wrapper flavour, real host).
//  9. A file (the cabinet's impulse response) loads on a background thread into a second bank and
//     crossfades in over 20 ms (banks.hpp).
// 10. What a block learned (Block v2 learned state, e.g. a noise profile) is saved and restored
//     with the parameters (learned.hpp); a trigger parameter (Role::Trigger) fires only on an
//     event the host sends, never when a state is loaded or values are re-applied.
// process() allocates nothing, takes no lock and does no I/O: hosts/clap/tests checks it with the
// repository's allocation and lock watch, and with RealtimeSanitizer.
//
// Threads: parameter values live in atomics (the mirror). The audio thread writes them from
// events and meters; the main thread reads them for get_value and state, and writes them on state
// load, then raises resync_ so the audio thread applies them at its next buffer.
#pragma once
#include <clap/clap.h>
#include <clapwrapper/wrapper_host.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <squatch/dsp/denormal.hpp>
#include <string>
#include <vector>

#include "banks.hpp"
#include "learned.hpp"
#include "param_text.hpp"
#include "state_text.hpp"

namespace squatch::clap {

/// The three configurations, by id: 1 -> 1, 1 -> 2, 2 -> 2.
struct PortConfig {
  clap_id id;
  const char* name;
  int in, out;
};
constexpr PortConfig kConfigs[] = {{0, "Mono", 1, 1}, {1, "Mono to stereo", 1, 2}, {2, "Stereo", 2, 2}};
constexpr int kDefaultConfig = 2;

template <class B>
class ClapPlugin {
 public:
  using BankT = typename Banks<B>::BankT;
  static constexpr int kCount = B::kCount;
  static constexpr const char* kFileKey = PluginTraits<B>::kFileKey;

  ClapPlugin(const clap_host_t* host, const clap_plugin_descriptor_t* desc);
  const clap_plugin_t* clap() const { return &plugin_; }

  // For hosts/clap/tests: what the plugin learned from its host, and background swaps done.
  const char* wrapperFlavor() const { return flavor_; }
  int swaps() const { return banks_.swaps(); }

 private:
  static ClapPlugin& self(const clap_plugin_t* p) { return *static_cast<ClapPlugin*>(p->plugin_data); }
  static dsp::Span<const dsp::ParamDesc> params() { return B::params(); }

  // clap_plugin
  static bool init(const clap_plugin_t* p);
  static void destroy(const clap_plugin_t* p) { delete &self(p); }
  static bool activate(const clap_plugin_t* p, double rate, uint32_t minFrames, uint32_t maxFrames);
  static void deactivate(const clap_plugin_t* p);
  static bool startProcessing(const clap_plugin_t* p) { return self(p).processing_ = true; }
  static void stopProcessing(const clap_plugin_t* p) { self(p).processing_ = false; }
  static void reset(const clap_plugin_t* p);
  static clap_process_status process(const clap_plugin_t* p, const clap_process_t* process);
  static const void* extension(const clap_plugin_t* p, const char* id);
  static void onMainThread(const clap_plugin_t* p);

  // extensions (clap_plugin_ext.hpp)
  static uint32_t paramCount(const clap_plugin_t*) { return kCount; }
  static bool paramInfo(const clap_plugin_t* p, uint32_t index, clap_param_info_t* info);
  static bool paramValue(const clap_plugin_t* p, clap_id id, double* value);
  static bool paramText(const clap_plugin_t* p, clap_id id, double value, char* out, uint32_t size);
  static bool paramParse(const clap_plugin_t* p, clap_id id, const char* text, double* value);
  static void paramFlush(const clap_plugin_t* p, const clap_input_events_t* in, const clap_output_events_t* out);
  static uint32_t portCount(const clap_plugin_t*, bool) { return 1; }
  static bool portInfo(const clap_plugin_t* p, uint32_t index, bool input, clap_audio_port_info_t* info);
  static uint32_t configCount(const clap_plugin_t*) { return 3; }
  static bool configGet(const clap_plugin_t* p, uint32_t index, clap_audio_ports_config_t* config);
  static bool configSelect(const clap_plugin_t* p, clap_id id);
  static clap_id configCurrent(const clap_plugin_t* p) { return kConfigs[self(p).config_].id; }
  static bool configPort(const clap_plugin_t* p, clap_id id, uint32_t index, bool input, clap_audio_port_info_t* info);
  static bool canConfigure(const clap_plugin_t* p, const clap_audio_port_configuration_request* r, uint32_t n);
  static bool configure(const clap_plugin_t* p, const clap_audio_port_configuration_request* r, uint32_t n);
  static bool stateSave(const clap_plugin_t* p, const clap_ostream_t* stream);
  static bool stateLoad(const clap_plugin_t* p, const clap_istream_t* stream);
  static bool acceptsLearned(const std::vector<std::string>& blobs);
  static uint32_t latency(const clap_plugin_t* p) { return static_cast<uint32_t>(self(p).latency_); }
  static uint32_t tail(const clap_plugin_t* p);

  // the audio thread's work
  void apply(const clap_event_header_t* e);
  void applyAll();
  void setBlock(const dsp::ParamDesc& d, float v);
  void run(const clap_process_t* p, uint32_t from, uint32_t to);
  void runEvents(const clap_process_t* process);
  void adopt(const StateValues& loaded);
  bool prepareBanks();
  void reportLatency();
  void publishMeters(const clap_output_events_t* out, uint32_t frames);
  int meterPeriod() const;
  void setValue(const dsp::ParamDesc& d, float v) { values_[d.id].store(v, std::memory_order_relaxed); }
  float value(const dsp::ParamDesc& d) const { return values_[d.id].load(std::memory_order_relaxed); }
  std::vector<float> snapshot() const;
  void log(clap_log_severity severity, const std::string& message) const;
  static int findConfig(int in, int out);
  static const dsp::ParamDesc* byStableId(clap_id id) { return dsp::findStable(params(), id); }
  /// A cookie is used only if it points into this plugin's own table (a host may pass another's).
  static bool ours(const void* cookie) {
    const auto at = reinterpret_cast<std::uintptr_t>(cookie), first = reinterpret_cast<std::uintptr_t>(params().begin());
    return cookie && at >= first && at < reinterpret_cast<std::uintptr_t>(params().end()) &&
           (at - first) % sizeof(dsp::ParamDesc) == 0;
  }

  clap_plugin_t plugin_;
  const clap_host_t* host_;
  const clap_host_log_t* log_ = nullptr;
  const clap_host_latency_t* hostLatency_ = nullptr;
  const clap_host_params_t* hostParams_ = nullptr;
  const char* flavor_ = nullptr;
  std::atomic<float> values_[kCount];
  std::atomic<bool> resync_{false};
  float published_[kCount] = {};  // audio thread: meter values last sent to the host
  int config_ = kDefaultConfig;
  bool active_ = false, processing_ = false, restartAsked_ = false;
  int latency_ = 0, reportedLatency_ = -1, meterCountdown_ = 0;
  std::atomic<int> tail_{0};
  Activation activation_;
  std::string file_;  // main thread: the file the state names
  Banks<B> banks_;
  Engine engine_;
  Learned learned_;
};

template <class B>
ClapPlugin<B>::ClapPlugin(const clap_host_t* host, const clap_plugin_descriptor_t* desc) : host_(host) {
  plugin_ = {desc,   this,          init,    destroy,   activate,  deactivate, startProcessing,
             stopProcessing, reset, process, extension, onMainThread};
  for (const dsp::ParamDesc& d : params()) values_[d.id].store(d.def), published_[d.id] = d.def;
  banks_.onError([](void* h) { static_cast<const clap_host_t*>(h)->request_callback(static_cast<const clap_host_t*>(h)); },
                 const_cast<clap_host_t*>(host_));
}

// A file the loader could not read is reported here, on the main thread, after request_callback.
template <class B>
void ClapPlugin<B>::onMainThread(const clap_plugin_t* p) {
  ClapPlugin& s = self(p);
  const std::string error = s.banks_.takeError();
  if (!error.empty()) s.log(CLAP_LOG_WARNING, "cannot load " + error + "; keeping the current one");
}

template <class B>
bool ClapPlugin<B>::init(const clap_plugin_t* p) {
  ClapPlugin& s = self(p);
  const clap_host_t* h = s.host_;
  s.log_ = static_cast<const clap_host_log_t*>(h->get_extension(h, CLAP_EXT_LOG));
  s.hostLatency_ = static_cast<const clap_host_latency_t*>(h->get_extension(h, CLAP_EXT_LATENCY));
  s.hostParams_ = static_cast<const clap_host_params_t*>(h->get_extension(h, CLAP_EXT_PARAMS));
  const auto* wrapped =
      static_cast<const clap_wrapper_host_information_t*>(h->get_extension(h, CLAP_WRAPPER_HOST_INFORMATION));
  if (wrapped && wrapped->get_wrapper_flavor) s.flavor_ = wrapped->get_wrapper_flavor(h);
  const char* real = wrapped && wrapped->get_underlying_host_name ? wrapped->get_underlying_host_name(h) : nullptr;
  s.log(CLAP_LOG_INFO, std::string(B::kInfo.name) + " " + B::kInfo.version + " in " + (real ? real : h->name) +
                           (s.flavor_ ? std::string(" as ") + s.flavor_ : std::string(" (CLAP)")));
  return true;
}

template <class B>
bool ClapPlugin<B>::activate(const clap_plugin_t* p, double rate, uint32_t, uint32_t maxFrames) {
  ClapPlugin& s = self(p);
  if (s.active_ || !(rate > 0.0) || maxFrames == 0 || maxFrames > (1u << 20)) return false;
  s.activation_ = {rate, static_cast<int>(maxFrames), kConfigs[s.config_].in, kConfigs[s.config_].out};
  if (!s.prepareBanks()) return false;
  for (const dsp::ParamDesc& d : params())
    if (d.role == dsp::Role::Bypass) s.engine_.bypass(s.value(d) >= 0.5f, true);
  s.reportLatency();
  s.tail_.store(s.banks_.active().blocks[0].tail());
  s.restartAsked_ = false;
  s.meterCountdown_ = s.meterPeriod();  // the first reading after one period of audio
  s.resync_.store(false);
  s.active_ = true;
  return true;
}

// Main thread, in activate(): the blocks (and any file) for this rate, block size and layout.
template <class B>
bool ClapPlugin<B>::prepareBanks() {
  std::string error;
  if (!banks_.activate(activation_, snapshot(), file_, error)) return false;
  if (!error.empty()) log(CLAP_LOG_WARNING, "cannot load " + error + "; passing audio through");
  latency_ = banks_.active().blocks[0].latency();
  learned_.activate(banks_.active());
  return engine_.prepare(activation_, latency_);
}

// The latency may only change here, during activate(); the host hears about it if it did.
template <class B>
void ClapPlugin<B>::reportLatency() {
  if (hostLatency_ && reportedLatency_ >= 0 && latency_ != reportedLatency_) hostLatency_->changed(host_);
  reportedLatency_ = latency_;
}

// Tolerates a host that never called stop_processing (clap#515).
template <class B>
void ClapPlugin<B>::deactivate(const clap_plugin_t* p) {
  ClapPlugin& s = self(p);
  if (!s.active_) return;
  s.banks_.quiesce();
  s.processing_ = false;
  s.active_ = false;
}

template <class B>
void ClapPlugin<B>::reset(const clap_plugin_t* p) {
  ClapPlugin& s = self(p);
  if (!s.active_) return;
  BankT& b = s.banks_.active();
  for (int i = 0; i < b.instances; ++i) b.blocks[i].reset();
  s.engine_.reset();
}

template <class B>
clap_process_status ClapPlugin<B>::process(const clap_plugin_t* p, const clap_process_t* process) {
  ClapPlugin& s = self(p);
  if (!s.active_ || !process) return CLAP_PROCESS_ERROR;
  dsp::DenormalGuard ftz;
  if (s.resync_.exchange(false, std::memory_order_acquire)) s.applyAll();
  s.banks_.beginBlock(s.engine_, [&s](BankT& bank) {
    for (const dsp::ParamDesc& d : params())
      if (d.applied()) bank.set(d.id, s.value(d));
  });
  s.learned_.receive(s.banks_.active());
  s.runEvents(process);
  s.learned_.publish(s.banks_.active());
  s.publishMeters(process->out_events, process->frames_count);
  return CLAP_PROCESS_CONTINUE;
}

// Sample-accurate: the buffer is cut at every event's time, and nothing runs longer than the
// activated maximum even when the host passes more.
template <class B>
void ClapPlugin<B>::runEvents(const clap_process_t* process) {
  const clap_input_events_t* in = process->in_events;
  const uint32_t events = in ? in->size(in) : 0, frames = process->frames_count;
  uint32_t done = 0, next = 0;
  while (done < frames) {
    for (; next < events && in->get(in, next)->time <= done; ++next) apply(in->get(in, next));
    const uint32_t until = next < events ? std::min(in->get(in, next)->time, frames) : frames;
    const uint32_t end = std::min(until, done + static_cast<uint32_t>(engine_.maxFrames()));
    run(process, done, end);
    done = end;
  }
  for (; next < events; ++next) apply(in->get(in, next));
}

/// The channel pointers of a host buffer from frame `from`, at most kChannels; 0 if it has none.
inline int channels(const clap_audio_buffer_t* buffers, uint32_t count, uint32_t from, float** out) {
  if (count == 0 || !buffers[0].data32) return 0;
  int n = 0;
  for (; n < kChannels && n < static_cast<int>(buffers[0].channel_count); ++n) out[n] = buffers[0].data32[n] + from;
  return n;
}

// One stretch of the buffer with no event inside, at most maxFrames long.
template <class B>
void ClapPlugin<B>::run(const clap_process_t* p, uint32_t from, uint32_t to) {
  float* in[kChannels] = {};
  float* out[kChannels] = {};
  const int inCh = channels(p->audio_inputs, p->audio_inputs_count, from, in);
  const int outCh = channels(p->audio_outputs, p->audio_outputs_count, from, out);
  if (outCh == 0 || to <= from) return;
  engine_.run(banks_.active(), banks_.fadingOut(), Io{in, inCh, out, outCh}, static_cast<int>(to - from));
  p->audio_outputs[0].constant_mask = 0;
}

template <class B>
void ClapPlugin<B>::apply(const clap_event_header_t* e) {
  if (e->space_id != CLAP_CORE_EVENT_SPACE_ID || e->type != CLAP_EVENT_PARAM_VALUE) return;
  const auto* ev = reinterpret_cast<const clap_event_param_value_t*>(e);
  const dsp::ParamDesc* d = ours(ev->cookie) ? static_cast<const dsp::ParamDesc*>(ev->cookie) : byStableId(ev->param_id);
  if (!d || d->stableId != ev->param_id || !d->settable()) return;
  const float v = fromHost(*d, ev->value);
  setValue(*d, v);
  if (active_) setBlock(*d, v);
}

template <class B>
void ClapPlugin<B>::setBlock(const dsp::ParamDesc& d, float v) {
  if (d.role == dsp::Role::Bypass) return engine_.bypass(v >= 0.5f, false);
  banks_.active().set(d.id, v);
  if (BankT* old = banks_.fadingOut()) old->set(d.id, v);
}

// After a state load: every value but the triggers, which fire only on an event.
template <class B>
void ClapPlugin<B>::applyAll() {
  for (const dsp::ParamDesc& d : params())
    if (d.applied() || d.role == dsp::Role::Bypass) setBlock(d, value(d));
}

// Meters go to the mirror, and to the host as read-only parameter values at the contract's rate.
template <class B>
void ClapPlugin<B>::publishMeters(const clap_output_events_t* out, uint32_t frames) {
  BankT& b = banks_.active();
  tail_.store(b.blocks[0].tail(), std::memory_order_relaxed);
  if (b.blocks[0].latency() != latency_ && !restartAsked_) restartAsked_ = true, host_->request_restart(host_);
  meterCountdown_ -= static_cast<int>(frames);
  if (meterCountdown_ > 0) return;
  meterCountdown_ = meterPeriod();
  for (const dsp::ParamDesc& d : params()) {
    if (d.role != dsp::Role::Meter) continue;
    const float v = d.clamp(b.blocks[0].get(d.id));
    setValue(d, v);
    if (!out || frames == 0 || v == published_[d.id]) continue;
    clap_event_param_value_t e{{sizeof e, frames - 1, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0},
                               d.stableId, const_cast<dsp::ParamDesc*>(&d), -1, -1, -1, -1, toHost(d, v)};
    if (out->try_push(out, &e.header)) published_[d.id] = v;
  }
}

// Samples between meter readings: the fastest rate any meter's contract asks for (30 Hz if none).
template <class B>
int ClapPlugin<B>::meterPeriod() const {
  float rate = 0.0f;
  for (const dsp::ParamDesc& d : params()) rate = d.role == dsp::Role::Meter ? std::max(rate, d.rateHz) : rate;
  return static_cast<int>(activation_.sampleRate / (rate > 0.0f ? rate : 30.0f));
}

template <class B>
std::vector<float> ClapPlugin<B>::snapshot() const {
  std::vector<float> v;
  for (const dsp::ParamDesc& d : params()) v.push_back(value(d));
  return v;
}

template <class B>
void ClapPlugin<B>::log(clap_log_severity severity, const std::string& message) const {
  if (log_ && log_->log) log_->log(host_, severity, message.c_str());
}

template <class B>
int ClapPlugin<B>::findConfig(int in, int out) {
  for (int i = 0; i < 3; ++i)
    if (kConfigs[i].in == in && kConfigs[i].out == out) return i;
  return -1;
}

}  // namespace squatch::clap

#include "clap_plugin_ext.hpp"
