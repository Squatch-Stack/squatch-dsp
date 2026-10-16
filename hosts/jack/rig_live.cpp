// SPDX-License-Identifier: MIT
// squatch-rig-live: a whole guitar rig as one JACK client.
//
//   system:capture_N -> gate -> tuner -> comp -> od -> crunch -> amp (NAM) -> EQ -> cabinet (IR)
//                    -> hall -> limiter -> out           (comp, od, crunch, hall start off)
//
// `squatch-rig-live --help` lists the options (rig_options.hpp). Every stage can be bypassed;
// the amp is bypassed when the program was built without NAM Core or given no model, and the
// cabinet when given no impulse response. Telemetry: one JSON line a second on stdout
// (rig_telemetry.hpp). Start-up details and errors go to stderr.
//
// Commands on stdin, one per line:  on STAGE | off STAGE | set STAGE PARAM VALUE
// (PARAM is a parameter's name with '-' for spaces, e.g. `set eq low-gain 3`) |
// setid STAGE ID VALUE (ID is the contract's stable id, as the phone and the web page use) |
// a4 HZ | mute on | mute off.
//
// With --stream HZ, it also prints HZ lines a second for hosts/pi/pedal-web: the tuner reading in
// the phone's telemetry format, plus the input and output peaks, each stage's switch and each
// meter by "stage.id" ({"stream":1, "valid":..., "on":{...}, "m":{...}}).
//
// Threads: JACK's realtime thread runs the chain (rig_engine.hpp); the main thread prints the
// telemetry; a command thread reads stdin. Everything that allocates, reads files or builds
// tables happens before the client is activated.
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <thread>

#include "../common/ir_file.hpp"
#include "rig_options.hpp"
#include "rig_telemetry.hpp"

namespace {

using namespace squatch::rig;
using squatch::host::running;

Engine gEngine;  // static storage: the tuner alone is ~128 kB
std::atomic<float> gA4{440.0f};
std::vector<float> gResponse;
std::unique_ptr<unsigned char[]> gMemory;

// ---------------------------------------------------------------- set-up ----

bool loadAmp(Engine& e, const Options& o, double rate) {
#ifdef SQUATCH_WITH_NAM
  if (!o.model || o.bypass[Amp]) return false;
  const std::string err = e.amp.load(o.model, !o.noNormalize);
  if (!err.empty()) return std::fprintf(stderr, "amp: cannot load the model: %s\n", err.c_str()), false;
  const double want = e.amp.expectedRate();
  if (want > 0.0 && std::fabs(want - rate) > 0.5)
    return std::fprintf(stderr, "amp: the model is for %.0f Hz and JACK runs at %.0f Hz; amp bypassed\n", want, rate), false;
  std::fprintf(stderr, "amp: NAM model loaded, loudness makeup %+.1f dB\n", e.amp.makeupDb());
  e.amp.set(squatch::host::NamAmp::Input, o.inDb);
  e.amp.set(squatch::host::NamAmp::Output, o.ampDb);
  return true;
#else
  (void)e, (void)rate;
  if (o.model) std::fprintf(stderr, "amp: built without NAM Core (make rig NAM_CORE=...); amp bypassed\n");
  return false;
#endif
}

bool loadCab(Engine& e, const Options& o, double rate) {
  if (!o.ir || o.bypass[Cab]) return false;
  squatch::host::Response r = squatch::host::loadResponse(o.ir, rate, o.irMaxMs, !o.irNoResample);
  if (!r.error.empty()) return std::fprintf(stderr, "cab: %s; cabinet bypassed\n", r.error.c_str()), false;
  gResponse = std::move(r.taps);
  e.cab.configure(gResponse.data(), static_cast<int>(gResponse.size()));
  e.cab.set(squatch::dsp::Cabinet::Level, o.cabDb);
  std::fprintf(stderr, "cab: %zu samples at %.0f Hz%s -> %zu taps (%.1f ms)\n", r.fileSamples, r.fileRate,
               r.resampled ? ", resampled" : "", gResponse.size(), 1e3 * gResponse.size() / rate);
  return true;
}

void setParams(Engine& e, const Options& o) {
  using namespace squatch::dsp;
  e.gate.set(squatch::dsp::Gate::Threshold, o.gateDb);
  e.gateExtra.configure(o.extra);
  e.eq.set(squatch::dsp::Eq::LowGain, o.eqLow);
  e.eq.set(squatch::dsp::Eq::MidGain, o.eqMid);
  e.eq.set(squatch::dsp::Eq::HighGain, o.eqHigh);
  e.limiter.set(squatch::dsp::Limiter::Ceiling, o.ceilingDb);
}

void fillSlots(Engine& e, const Options& o, bool amp, bool cab) {
  const auto slot = [&](Stage s, Block* b, Span<const ParamDesc> p, bool available) {
    e.slots[s].block = b, e.slots[s].params = p, e.slots[s].available = b && available;
    e.slots[s].on.store(!o.bypass[s]);
  };
  Block* other = e.gateExtra.block(o.extra);
  slot(Gate, other ? other : &e.gate, other ? extra::GateStage::params() : squatch::dsp::Gate::params(), true);
  slot(Tuner, &e.tuner, squatch::dsp::TunerBlock::params(), true);
  slot(Comp, &e.comp, squatch::dsp::Compressor::params(), true);
  slot(Od, &e.od, squatch::dsp::TrailOD::params(), true);
  slot(Claw, &e.claw, squatch::dsp::Claw::params(), true);
  slot(Crunch, &e.crunch, squatch::dsp::Crunch::params(), true);
#ifdef SQUATCH_WITH_NAM
  slot(Amp, &e.amp, squatch::host::NamAmp::params(), amp);
#else
  slot(Amp, nullptr, {}, amp);
#endif
  slot(Eq, &e.eq, squatch::dsp::Eq::params(), true);
  slot(Cab, &e.cab, squatch::dsp::Cabinet::params(), cab);
  slot(Hall, &e.hall, squatch::dsp::Hall::params(), true);
  slot(Limiter, &e.limiter, squatch::dsp::Limiter::params(), true);
}

// Measure what every block needs, then take exactly that (plus alignment slack) and prepare.
bool prepareAll(Engine& e, const squatch::dsp::Setup& setup) {
  const auto prepare = [&](squatch::dsp::Arena& mem) {
    e.work = mem.alloc<float>(static_cast<std::size_t>(setup.maxBlock));
    for (Slot& s : e.slots)
      if (s.block) s.block->prepare(setup, mem);
  };
  squatch::dsp::Arena measure;
  prepare(measure);
  const std::size_t bytes = measure.required() + 64 * (kStages + 1) * squatch::dsp::Arena::kAlign;
  gMemory.reset(new unsigned char[bytes]);
  squatch::dsp::Arena mem(gMemory.get(), bytes);
  prepare(mem);
  std::fprintf(stderr, "memory: %zu bytes from the arena\n", mem.used());
  return !mem.failed() && (!e.slots[Cab].available || e.cab.ready()) && e.hall.ready();
}

bool connectAll(const Engine& e, const Options& o) {
  char from[64];
  std::snprintf(from, sizeof from, "system:capture_%d", o.input);
  bool ok = squatch::host::connectPorts(e.client, from, jack_port_name(e.in));
  for (const std::string& to : o.outs)
    if (to != "none") ok = squatch::host::connectPorts(e.client, jack_port_name(e.out), to.c_str()) && ok;
  return ok;
}

bool setUp(Engine& e, jack_client_t* c, const Options& o) {
  e.client = c;
  const double rate = jack_get_sample_rate(c);
  e.maxBlock = static_cast<int>(jack_get_buffer_size(c));
  const bool amp = loadAmp(e, o, rate), cab = loadCab(e, o, rate);
  fillSlots(e, o, amp, cab);
  setParams(e, o);
  if (!prepareAll(e, squatch::dsp::Setup{rate, e.maxBlock})) return std::fprintf(stderr, "prepare failed\n"), false;
  if (!e.gateExtra.ready(o.extra, rate, e.maxBlock)) return false;
  e.in = jack_port_register(c, "in", JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
  e.out = jack_port_register(c, "out", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
  if (!e.in || !e.out) return false;
  jack_set_process_callback(c, process, &e);
  jack_set_xrun_callback(c, onXrun, &e);
  if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) std::fprintf(stderr, "warning: mlockall failed\n");
  if (jack_activate(c) != 0) return false;
  return connectAll(e, o);
}

void describe(const Engine& e) {
  const double rate = jack_get_sample_rate(e.client);
  std::fprintf(stderr, "squatch-rig-live: %.0f Hz, period %d, realtime %s, stages:", rate, e.maxBlock,
               jack_is_realtime(e.client) ? "yes" : "NO");
  for (int s = 0; s < kStages; ++s)
    std::fprintf(stderr, " %s=%s", stageName(s), !e.slots[s].available ? "n/a" : e.slots[s].on.load() ? "on" : "off");
  std::fprintf(stderr, ", added latency %.2f ms\n", 1e3 * algorithmicLatency(e) / rate);
}

// ---------------------------------------------------------------- commands ----

int findStage(const char* name) {
  for (int s = 0; s < kStages; ++s)
    if (std::strcmp(name, stageName(s)) == 0) return s;
  return -1;
}

// "low-gain" matches "Low gain": case-insensitive, '-' or '_' for a space.
bool sameName(const char* typed, const char* name) {
  for (; *typed && *name; ++typed, ++name) {
    const char t = *typed == '-' || *typed == '_' ? ' ' : *typed;
    if (std::tolower(static_cast<unsigned char>(t)) != std::tolower(static_cast<unsigned char>(*name))) return false;
  }
  return *typed == *name;
}

// A parameter change for stage s, by name. False when the stage has no such parameter.
bool setParam(Engine& e, int s, const char* param, float value) {
  for (const ParamDesc& d : e.slots[s].params)
    if (sameName(param, d.name)) return e.changes.push({static_cast<std::uint16_t>(s), d.id, value});
  return false;
}

// "on STAGE" / "off STAGE". False when the verb is neither.
bool switchStage(Engine& e, int s, const char* verb) {
  const bool on = std::strcmp(verb, "on") == 0;
  if (!on && std::strcmp(verb, "off") != 0) return false;
  return e.changes.push({static_cast<std::uint16_t>(s), kSwitch, on ? 1.0f : 0.0f});
}

// "setid STAGE ID VALUE": by the contract's stable id. The NAM amp here has none (hosts/jack/
// nam_amp.hpp numbers Input 0 and Output 1); the phone's contract gives them 1 and 2, so the
// same ids work for both.
bool setById(Engine& e, int s, std::uint32_t id, float value) {
  const auto& params = e.slots[s].params;
  if (s == Amp) return id >= 1 && id <= params.size() && e.changes.push({static_cast<std::uint16_t>(s), static_cast<std::uint16_t>(id - 1), value});
  const ParamDesc* d = squatch::dsp::findStable(params, id);
  return d && d->settable() && e.changes.push({static_cast<std::uint16_t>(s), d->id, value});
}

// One command line, split: "VERB ARG [PARAM VALUE]".
struct Command {
  char verb[16] = {}, arg[16] = {}, param[32] = {};
  float value = 0.0f;
  int got = 0;
  bool is(const char* v) const { return std::strcmp(verb, v) == 0; }
};

// "a4 HZ" (430..450) and "mute on|off": the rig's own settings. False for anything else.
bool rigCommand(Engine& e, const Command& c) {
  if (c.got < 2) return false;
  if (c.is("a4")) {
    gA4.store(std::fmin(450.0f, std::fmax(430.0f, std::strtof(c.arg, nullptr))));
    return true;
  }
  if (!c.is("mute")) return false;
  e.muted.store(std::strcmp(c.arg, "on") == 0);
  return true;
}

// "on|off STAGE", "set STAGE PARAM VALUE" (by name) and "setid STAGE ID VALUE" (by stable id).
bool stageCommand(Engine& e, int s, const Command& c) {
  if (c.got == 2) return switchStage(e, s, c.verb);
  if (c.got != 4) return false;
  if (c.is("set")) return setParam(e, s, c.param, c.value);
  return c.is("setid") && setById(e, s, static_cast<std::uint32_t>(std::strtoul(c.param, nullptr, 10)), c.value);
}

void command(Engine& e, const char* line) {
  Command c;
  c.got = std::sscanf(line, "%15s %15s %31s %f", c.verb, c.arg, c.param, &c.value);
  if (rigCommand(e, c)) return;
  const int s = c.got >= 2 ? findStage(c.arg) : -1;
  if (s < 0 || !e.slots[s].block || !stageCommand(e, s, c)) std::fprintf(stderr, "? %s\n", line);
}

// Raw read(2), not fgets: see tuner_live.cpp (a thread blocked in fgets would block exit()).
void readCommands(Engine& e) {
  char line[128];
  std::size_t len = 0;
  for (char c; read(STDIN_FILENO, &c, 1) == 1;) {
    if (c != '\n') {
      if (len < sizeof line - 1) line[len++] = c;
      continue;
    }
    line[len] = '\0';
    command(e, line);
    len = 0;
  }
}

// ---------------------------------------------------------------- stream ----

double dbfs(float x) { return 20.0 * std::log10(std::max(x, 1e-6f)); }

double portLatencyMs(const Engine& e, jack_port_t* p, jack_latency_callback_mode_t mode) {
  jack_latency_range_t r{};
  jack_port_get_latency_range(p, mode, &r);
  return 1000.0 * r.max / jack_get_sample_rate(e.client);
}

// The tuner's newest reading, as the phone's telemetry prints it (the fields the page draws).
void printReading(const squatch::dsp::TunerSnapshot& snap, float a4, float in) {
  const squatch::host::DisplayReading d = squatch::host::toDisplay(snap.reading, a4);
  std::printf("{\"stream\":1,\"valid\":%s,\"note\":\"%s\",\"oct\":%d,\"midi\":%d,\"cents\":%.9g,\"fast\":%.3f,"
              "\"hz\":%.4f,\"shz\":%.4f,\"unc\":%.9g,\"parts\":%d,\"B\":%.3g,\"a4\":%.2f,\"lvl\":%.1f,\"pk\":%.1f,",
              d.valid ? "true" : "false", d.note.name, d.note.octave, d.midi, d.cents, d.fastCents, d.hz, d.steadyHz,
              snap.reading.uncertainty, snap.reading.partials, snap.reading.inharmonicity, a4, dbfs(snap.reading.level), dbfs(in));
}

// The audio path: samples, rate, latency, load, overruns, monitor and the peaks since the last line.
void printAudio(const Engine& e, const Options& o, std::uint64_t samples, float in, float out) {
  std::printf("\"samples\":%llu,\"rate\":%.0f,\"period\":%d,\"input\":%d,\"in\":\"UMC204HD\",\"out\":\"UMC204HD\","
              "\"inMs\":%.3f,\"outMs\":%.3f,\"dsp\":%.2f,\"dspWorst\":0,\"cpu\":0,\"overruns\":%u,\"monitor\":%s,"
              "\"inDb\":%.1f,\"outDb\":%.1f,\"on\":{",
              static_cast<unsigned long long>(samples), static_cast<double>(jack_get_sample_rate(e.client)), e.maxBlock, o.input,
              portLatencyMs(e, e.in, JackCaptureLatency), portLatencyMs(e, e.out, JackPlaybackLatency), jack_cpu_load(e.client),
              e.xruns.load(), e.muted.load() ? "false" : "true", dbfs(in), dbfs(out));
}

void printSwitches(const Engine& e) {
  for (int s = 0; s < kStages; ++s)
    std::printf("%s\"%s\":%d", s ? "," : "", stageName(s), e.slots[s].available && e.slots[s].on.load() ? 1 : 0);
}

// Every available stage's meters. They are written by the audio thread and read here: a float
// store and load, as the phone's AU parameter tree reads them.
void printMeters(const Engine& e) {
  bool first = true;
  for (int s = 0; s < kStages; ++s) {
    if (!e.slots[s].available) continue;
    for (const ParamDesc& p : e.slots[s].params) {
      if (p.role != squatch::dsp::Role::Meter) continue;
      std::printf("%s\"%s.%u\":%.3f", first ? "" : ",", stageName(s), p.stableId, e.slots[s].block->get(p.id));
      first = false;
    }
  }
}

// One line for pedal-web: the reading, the audio path, the switches and the meters.
void streamLine(Engine& e, const Options& o) {
  auto& latest = e.tuner.readings();
  latest.update();
  const squatch::dsp::TunerSnapshot& snap = latest.front();
  const float in = e.inHold.exchange(0.0f), out = e.outHold.exchange(0.0f);
  printReading(snap, gA4.load(), in);
  printAudio(e, o, snap.samples, in, out);
  printSwitches(e);
  std::printf("},\"m\":{");
  printMeters(e);
  std::printf("}}\n");
}

// Once a second the telemetry line; with --stream, the stream lines in between. Until --seconds
// pass, the rig is stopped, or the reader goes.
void run(const Options& o) {
  Telemetry telemetry(gEngine);
  const std::uint64_t end = o.seconds > 0.0f ? squatch::host::nowNs() + static_cast<std::uint64_t>(o.seconds * 1e9) : UINT64_MAX;
  const int perSecond = o.streamHz > 0.0f ? static_cast<int>(o.streamHz) : 1;
  const auto step = static_cast<std::uint64_t>(1e9 / perSecond);
  std::uint64_t deadline = squatch::host::nowNs();
  for (int tick = 0; running().load() && squatch::host::nowNs() < end; ++tick) {
    squatch::host::sleepUntil(deadline, step);
    if (o.streamHz > 0.0f) streamLine(gEngine, o);
    if (tick % perSecond == perSecond - 1 && !telemetry.line()) break;
    if (std::fflush(stdout) != 0) break;  // the reader has gone
  }
}

}  // namespace

int main(int argc, char** argv) {
  Options o;
  if (argc > 1 && std::strcmp(argv[1], "--help") == 0) return printUsage(stdout), 0;
  if (!parseOptions(argc, argv, o)) return printUsage(stderr), 64;
  jack_client_t* c = squatch::host::openClient(o.name);
  if (!c) return 2;
  if (!setUp(gEngine, c, o)) {
    std::fprintf(stderr, "squatch-rig-live: could not set up, register or activate the JACK client\n");
    jack_client_close(c);
    return 2;
  }
  describe(gEngine);
  std::thread(readCommands, std::ref(gEngine)).detach();
  run(o);
  jack_client_close(c);
  std::fprintf(stderr, "squatch-rig-live: stopped, %u xruns in all\n", gEngine.xruns.load());
  return 0;
}
