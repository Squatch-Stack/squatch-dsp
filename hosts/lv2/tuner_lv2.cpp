// SPDX-License-Identifier: MIT
// The Squatch Tuner as an LV2 plugin (urn:squatch-stack:lv2:tuner). Audio passes through, or is
// muted; the reading goes to output control ports that PiPedal draws without a custom UI:
//   pitch  units:midiNote, fractional, against Reference A4 (-1: nothing ringing) -> its tuner dial
//   note   pitch class as an enumeration -> text;  cents -> text;  level dB -> a VU;  valid -> a lamp
//   octave and freq are there for other hosts and hidden from PiPedal (notOnGUI).
// The note is named here, against the A4 control, exactly as hosts/common/display.hpp names it.
#include <squatch/dsp/blocks/tuner.hpp>

#include "../common/display.hpp"
#include "lv2_common.hpp"
#include "squatch-tuner_ports.hpp"

namespace {

using namespace squatch::lv2::tuner;
using squatch::lv2::kChunk;

struct Plugin {
  squatch::dsp::TunerBlock block;  // ~128 kB of fixed buffers: heap-allocated with the plugin
  squatch::lv2::BlockMemory memory;
  squatch::lv2::Ports<kPortCount> ports;
  float mute = -1.0f;  // the value last passed to the block; -1 forces the first set()
};

LV2_Handle instantiate(const LV2_Descriptor*, double rate, const char*, const LV2_Feature* const*) {
  auto* p = new (std::nothrow) Plugin;
  if (!p) return nullptr;
  if (!p->memory.prepare(p->block, rate)) {
    delete p;
    return nullptr;
  }
  return p;
}

void connect(LV2_Handle h, uint32_t port, void* data) {
  if (port < kPortCount) static_cast<Plugin*>(h)->ports.at[port] = data;
}

void activate(LV2_Handle h) { static_cast<Plugin*>(h)->block.reset(); }

void showNothing(const squatch::lv2::Ports<kPortCount>& ports, float levelDb) {
  ports.meter(kPitch, -1.0f);
  ports.meter(kNote, -1.0f);
  ports.meter(kOctave, -1.0f);
  ports.meter(kCents, 0.0f);
  ports.meter(kValid, 0.0f);
  ports.meter(kFreq, 0.0f);
  ports.meter(kLevel, levelDb);
}

// The newest reading, named against A4, onto the meter ports. No fresh reading: the ports keep
// what they show.
void showReading(Plugin& p) {
  if (!p.block.readings().update()) return;
  const squatch::tuner::TunerReading& r = p.block.readings().front().reading;
  const float a4 = p.ports.control(kA4, kPorts[kA4].def);
  const float levelDb = squatch::lv2::toDb(r.level, kPorts[kLevel].min);
  const squatch::host::DisplayReading d = squatch::host::toDisplay(r, a4);
  if (!d.valid) return showNothing(p.ports, levelDb);
  const int pc = ((d.midi % 12) + 12) % 12;
  p.ports.meter(kPitch, squatch::tuner::hzToMidi(d.steadyHz, a4));
  p.ports.meter(kNote, static_cast<float>(pc));
  p.ports.meter(kOctave, static_cast<float>(d.note.octave));
  p.ports.meter(kCents, std::fmin(std::fmax(d.cents, -50.0f), 50.0f));
  p.ports.meter(kValid, 1.0f);
  p.ports.meter(kFreq, d.steadyHz);
  p.ports.meter(kLevel, levelDb);
}

void run(LV2_Handle h, uint32_t n) {
  Plugin& p = *static_cast<Plugin*>(h);
  const float mute = p.ports.control(kMute, kPorts[kMute].def) >= 0.5f ? 1.0f : 0.0f;
  if (mute != p.mute) p.block.set(squatch::dsp::TunerBlock::Mute, p.mute = mute);
  squatch::lv2::runChunked(p.block, p.ports.audioIn(kAudioIn), p.ports.audioOut(kAudioOut), n);
  showReading(p);
}

void cleanup(LV2_Handle h) { delete static_cast<Plugin*>(h); }

const void* extensionData(const char*) { return nullptr; }

const LV2_Descriptor kDescriptor = {kUri, instantiate, connect, activate, run, nullptr, cleanup, extensionData};

}  // namespace

LV2_SYMBOL_EXPORT const LV2_Descriptor* lv2_descriptor(uint32_t index) { return index == 0 ? &kDescriptor : nullptr; }
