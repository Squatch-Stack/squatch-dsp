// SPDX-License-Identifier: MIT
// Plumbing shared by the CoreAudio programs here (macOS only): list and pick devices, run one IOProc
// on a device, and turn the HAL's buffers into audio::Chunk. Links CoreAudio and CoreFoundation only.
//
// The device queries and the IOProc set-up follow squatch-rig-live's src/audio.cpp (CoreAudio HAL
// access, same author, Squatch Stack); this file differs in picking the system's default devices or a
// name, and in taking no part in any rig. Nothing here changes the Mac's default devices, the buffer
// size or the sample rate of any device: it uses them as they are.
#pragma once
#include <CoreAudio/CoreAudio.h>
#include <mach/mach_time.h>

#include <cctype>
#include <cstdio>
#include <string>
#include <vector>

#include "../common/host_clock.hpp"
#include "audio_chunks.hpp"

namespace squatch::host::ca {

struct DeviceInfo {
  AudioObjectID id = 0;
  std::string name;
  uint32_t inChannels = 0, outChannels = 0;
  uint32_t inLatency = 0, outLatency = 0;  // frames: device + safety offset + stream
  uint32_t bufferFrames = 0;
  double rate = 0.0;
  bool floatFormat = true;                 // every stream's virtual format is 32-bit float PCM
  bool defaultIn = false, defaultOut = false;
};

namespace detail {

template <typename T>
bool getProp(AudioObjectID id, AudioObjectPropertySelector sel, AudioObjectPropertyScope scope, T& out) {
  const AudioObjectPropertyAddress a{sel, scope, kAudioObjectPropertyElementMain};
  UInt32 sz = sizeof(T);
  return AudioObjectGetPropertyData(id, &a, 0, nullptr, &sz, &out) == noErr;
}

inline std::string nameOf(AudioObjectID id) {
  CFStringRef s = nullptr;
  if (!getProp(id, kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal, s) || !s) return "";
  char buf[256] = {0};
  CFStringGetCString(s, buf, sizeof buf, kCFStringEncodingUTF8);
  CFRelease(s);
  return buf;
}

inline std::vector<AudioObjectID> streamsOf(AudioObjectID dev, AudioObjectPropertyScope scope) {
  const AudioObjectPropertyAddress a{kAudioDevicePropertyStreams, scope, kAudioObjectPropertyElementMain};
  UInt32 sz = 0;
  std::vector<AudioObjectID> v;
  if (AudioObjectGetPropertyDataSize(dev, &a, 0, nullptr, &sz) != noErr || !sz) return v;
  v.resize(sz / sizeof(AudioObjectID));
  if (AudioObjectGetPropertyData(dev, &a, 0, nullptr, &sz, v.data()) != noErr) v.clear();
  return v;
}

// Channels and latency of one direction of a device; clears `floatOk` if a stream is not float32 PCM.
inline void queryScope(AudioObjectID dev, AudioObjectPropertyScope scope, uint32_t& channels, uint32_t& latency, bool& floatOk) {
  UInt32 device = 0, safety = 0, stream = 0;
  getProp(dev, kAudioDevicePropertyLatency, scope, device);
  getProp(dev, kAudioDevicePropertySafetyOffset, scope, safety);
  for (AudioObjectID st : streamsOf(dev, scope)) {
    UInt32 l = 0;
    getProp(st, kAudioStreamPropertyLatency, kAudioObjectPropertyScopeGlobal, l);
    stream = l > stream ? l : stream;
    AudioStreamBasicDescription f{};
    if (!getProp(st, kAudioStreamPropertyVirtualFormat, kAudioObjectPropertyScopeGlobal, f)) continue;
    channels += f.mChannelsPerFrame;
    if (f.mFormatID != kAudioFormatLinearPCM || !(f.mFormatFlags & kAudioFormatFlagIsFloat) || f.mBitsPerChannel != 32) floatOk = false;
  }
  latency = device + safety + stream;
}

inline AudioObjectID defaultId(AudioObjectPropertySelector sel) {
  AudioObjectID id = 0;
  getProp(kAudioObjectSystemObject, sel, kAudioObjectPropertyScopeGlobal, id);
  return id;
}

inline bool containsNoCase(const std::string& hay, const std::string& needle) {
  const auto lower = [](std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
  };
  return lower(hay).find(lower(needle)) != std::string::npos;
}

}  // namespace detail

inline uint32_t bufferFramesOf(AudioObjectID id) {
  UInt32 frames = 0;
  detail::getProp(id, kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeGlobal, frames);
  return frames;
}

inline DeviceInfo describeDevice(AudioObjectID id, AudioObjectID defIn, AudioObjectID defOut) {
  DeviceInfo d;
  d.id = id;
  d.name = detail::nameOf(id);
  d.defaultIn = id == defIn;
  d.defaultOut = id == defOut;
  detail::getProp(id, kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, d.rate);
  detail::queryScope(id, kAudioObjectPropertyScopeInput, d.inChannels, d.inLatency, d.floatFormat);
  detail::queryScope(id, kAudioObjectPropertyScopeOutput, d.outChannels, d.outLatency, d.floatFormat);
  d.bufferFrames = bufferFramesOf(id);
  return d;
}

/// Every audio device, in the HAL's order.
inline std::vector<DeviceInfo> listDevices() {
  const AudioObjectPropertyAddress a{kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
  UInt32 sz = 0;
  std::vector<DeviceInfo> out;
  if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &a, 0, nullptr, &sz) != noErr) return out;
  std::vector<AudioObjectID> ids(sz / sizeof(AudioObjectID));
  if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, nullptr, &sz, ids.data()) != noErr) return out;
  const AudioObjectID defIn = detail::defaultId(kAudioHardwarePropertyDefaultInputDevice);
  const AudioObjectID defOut = detail::defaultId(kAudioHardwarePropertyDefaultOutputDevice);
  for (AudioObjectID id : ids) out.push_back(describeDevice(id, defIn, defOut));
  return out;
}

inline void printDevices(const std::vector<DeviceInfo>& devices, std::FILE* to) {
  std::fprintf(to, "%-4s %-40s %4s %4s %8s  %s\n", "id", "name", "in", "out", "rate Hz", "default");
  for (const DeviceInfo& d : devices)
    std::fprintf(to, "%-4u %-40s %4u %4u %8.0f  %s%s\n", d.id, d.name.c_str(), d.inChannels, d.outChannels, d.rate, d.defaultIn ? "input " : "",
                 d.defaultOut ? "output" : "");
}

/// The device to use for one direction: the first whose name contains `name` (any case) and that has
/// channels that way; with no name, the system's default device for that direction. Says why on failure.
inline bool pickDevice(const std::vector<DeviceInfo>& devices, const char* name, bool input, DeviceInfo& pick, std::string& why) {
  const char* dir = input ? "input" : "output";
  for (const DeviceInfo& d : devices) {
    const bool has = (input ? d.inChannels : d.outChannels) > 0;
    const bool wanted = name ? detail::containsNoCase(d.name, name) : (input ? d.defaultIn : d.defaultOut);
    if (has && wanted) return pick = d, true;
  }
  why = name ? std::string("no audio device with ") + dir + "s matches \"" + name + "\" (see --list-devices)"
             : std::string("this Mac has no default audio ") + dir + " device (see --list-devices, then --device NAME)";
  return false;
}

/// mach host time -> nanoseconds. AudioTimeStamp.mHostTime is in mach_absolute_time ticks.
class HostClock {
 public:
  HostClock() { mach_timebase_info(&tb_); }
  uint64_t ns(uint64_t ticks) const { return (ticks / tb_.denom) * tb_.numer + (ticks % tb_.denom) * tb_.numer / tb_.denom; }
  uint64_t nowNs() const { return ns(mach_absolute_time()); }

 private:
  mach_timebase_info_data_t tb_{};
};

/// The HAL's buffers as Chunks, without allocating: at most `cap` of them.
inline uint32_t chunksOf(const AudioBufferList* list, audio::Chunk* dst, uint32_t cap) {
  if (!list) return 0;
  const uint32_t n = list->mNumberBuffers < cap ? list->mNumberBuffers : cap;
  for (uint32_t i = 0; i < n; ++i) dst[i] = {list->mBuffers[i].mNumberChannels, list->mBuffers[i].mDataByteSize, list->mBuffers[i].mData};
  return n;
}

/// One IOProc on one device. The callback runs on the HAL's real-time thread.
class Stream {
 public:
  using Fn = void (*)(void* ctx, const AudioBufferList* in, const AudioTimeStamp* inTime, AudioBufferList* out, const AudioTimeStamp* outTime);
  Stream() = default;
  Stream(const Stream&) = delete;
  Stream& operator=(const Stream&) = delete;
  ~Stream() { stop(); }

  bool start(AudioObjectID device, Fn fn, void* ctx, std::string& err) {
    bind_ = {fn, ctx};
    OSStatus s = AudioDeviceCreateIOProcID(device, &Stream::ioproc, &bind_, &proc_);
    if (s != noErr) return err = "AudioDeviceCreateIOProcID failed: " + std::to_string(s), false;
    device_ = device;
    s = AudioDeviceStart(device, proc_);
    if (s != noErr) {
      err = "AudioDeviceStart failed: " + std::to_string(s);
      AudioDeviceDestroyIOProcID(device, proc_);
      proc_ = nullptr;
      return false;
    }
    return true;
  }

  void stop() {
    if (!proc_) return;
    AudioDeviceStop(device_, proc_);
    AudioDeviceDestroyIOProcID(device_, proc_);
    proc_ = nullptr;
  }

 private:
  struct Binding {
    Fn fn = nullptr;
    void* ctx = nullptr;
  };
  // The parameter list is CoreAudio's AudioDeviceIOProc, fixed by the HAL.
  static OSStatus ioproc(AudioObjectID, const AudioTimeStamp*, const AudioBufferList* in, const AudioTimeStamp* inTime, AudioBufferList* out,
                         const AudioTimeStamp* outTime, void* self) {
    // #lizard forgives
    const auto* b = static_cast<const Binding*>(self);
    b->fn(b->ctx, in, inTime, out, outTime);
    return noErr;
  }
  Binding bind_;
  AudioObjectID device_ = 0;
  AudioDeviceIOProcID proc_ = nullptr;
};

}  // namespace squatch::host::ca
