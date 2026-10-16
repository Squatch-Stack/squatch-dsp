// SPDX-License-Identifier: MIT
// The buffer shuffling of the CoreAudio host, with no CoreAudio types so that any machine can test it.
// A Chunk is one interleaved float32 buffer, laid out as CoreAudio's AudioBuffer (channels per frame,
// byte size, data); an AudioBufferList is a short array of them, one per stream. Everything here runs
// in the audio callback: no allocation, no locks, no system calls.
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace squatch::host::audio {

struct Chunk {
  uint32_t channels = 0;
  uint32_t bytes = 0;
  void* data = nullptr;
};

/// Frames in the first usable chunk (all chunks of one callback hold the same count).
/// A callback's buffers: what an AudioBufferList holds, one Chunk per stream. May be empty.
struct Chunks {
  Chunk* list = nullptr;
  uint32_t count = 0;
};

inline uint32_t framesOf(Chunks c) {
  for (uint32_t i = 0; c.list && i < c.count; ++i)
    if (c.list[i].channels && c.list[i].data) return c.list[i].bytes / (c.list[i].channels * static_cast<uint32_t>(sizeof(float)));
  return 0;
}

inline uint32_t channelsOf(Chunks c) {
  uint32_t total = 0;
  for (uint32_t i = 0; c.list && i < c.count; ++i) total += c.list[i].channels;
  return total;
}

/// Copies channel `channel` (0-based across all chunks) for frames [offset, offset + cap) into `dst`;
/// returns how many it wrote. A channel the device does not have reads as silence.
inline uint32_t gatherChannel(Chunks in, uint32_t channel, uint32_t offset, float* dst, uint32_t cap) {
  const uint32_t frames = framesOf(in);
  const uint32_t count = offset < frames ? std::min(cap, frames - offset) : 0;
  uint32_t first = 0;
  for (uint32_t i = 0; in.list && i < in.count; first += in.list[i].channels, ++i) {
    const Chunk& c = in.list[i];
    if (channel >= first + c.channels || !c.data) continue;
    const auto* src = static_cast<const float*>(c.data) + (channel - first);
    for (uint32_t k = 0; k < count; ++k) dst[k] = src[static_cast<size_t>(offset + k) * c.channels];
    return count;
  }
  std::fill(dst, dst + count, 0.0f);
  return count;
}

inline void clearAll(Chunks out) {
  for (uint32_t i = 0; out.list && i < out.count; ++i)
    if (out.list[i].data) std::memset(out.list[i].data, 0, out.list[i].bytes);
}

/// Writes `mono` (frames [offset, offset + count)) to output channels [0, channelCount).
inline void scatterMono(Chunks out, uint32_t channelCount, uint32_t offset, const float* mono, uint32_t count) {
  uint32_t first = 0;
  for (uint32_t i = 0; out.list && i < out.count; first += out.list[i].channels, ++i) {
    const Chunk& chunk = out.list[i];
    if (!chunk.data || first >= channelCount) continue;
    auto* d = static_cast<float*>(chunk.data);
    const uint32_t use = std::min(chunk.channels, channelCount - first);
    for (uint32_t c = 0; c < use; ++c)
      for (uint32_t k = 0; k < count; ++k) d[static_cast<size_t>(offset + k) * chunk.channels + c] = mono[k];
  }
}

}  // namespace squatch::host::audio
