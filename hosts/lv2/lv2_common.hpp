// SPDX-License-Identifier: MIT
// What every Squatch LV2 plugin shares: port pointers checked against the generated port table,
// arena memory taken once at instantiate(), and a run() that feeds a block in chunks no longer
// than the length it was prepared for, whatever length the host passes.
//
// Real-time rules (as for the blocks): instantiate() allocates; run() and the port writes never
// allocate, lock or touch files.
#pragma once
#include <lv2/core/lv2.h>

#include <cmath>
#include <cstddef>
#include <cstring>
#include <memory>
#include <new>
#include <squatch/dsp/arena.hpp>
#include <squatch/dsp/block.hpp>
#include <squatch/dsp/span.hpp>

namespace squatch::lv2 {

constexpr int kChunk = 256;  // the block size every block is prepared for; run() splits longer calls

/// Port buffers by index, as the host connects them. Control inputs read as their default until
/// connected, so a host that leaves one unconnected still gets sane values.
template <unsigned N>
struct Ports {
  void* at[N] = {};

  float control(unsigned i, float def) const {
    const float* p = static_cast<const float*>(at[i]);
    return p && *p == *p ? *p : def;
  }
  void meter(unsigned i, float v) const {
    if (float* p = static_cast<float*>(at[i])) *p = v;
  }
  const float* audioIn(unsigned i) const { return static_cast<const float*>(at[i]); }
  float* audioOut(unsigned i) const { return static_cast<float*>(at[i]); }
};

/// Arena memory for one block, measured first so it takes exactly what the block asks for.
struct BlockMemory {
  std::unique_ptr<unsigned char[]> bytes;

  bool prepare(dsp::Block& b, double rate) {
    const dsp::Setup setup{rate, kChunk};
    dsp::Arena measure;
    b.prepare(setup, measure);
    const std::size_t size = measure.required() + 4 * dsp::Arena::kAlign;
    bytes.reset(new (std::nothrow) unsigned char[size]);
    if (!bytes) return false;
    dsp::Arena mem(bytes.get(), size);
    b.prepare(setup, mem);
    return !mem.failed();
  }
};

/// The block over the host's buffers, kChunk samples at a time. Missing buffers: silence out.
inline void runChunked(dsp::Block& b, const float* in, float* out, std::uint32_t n) {
  if (!out) return;
  if (!in) return dsp::clear(out, static_cast<int>(n));
  for (std::uint32_t done = 0; done < n;) {
    const int len = static_cast<int>(n - done < kChunk ? n - done : kChunk);
    b.process(in + done, out + done, len);
    done += static_cast<std::uint32_t>(len);
  }
}

inline float toDb(float linear, float floorDb) {
  return linear > 0.0f ? std::fmax(20.0f * std::log10(linear), floorDb) : floorDb;
}

/// The feature with this URI from the host's null-terminated list, or nullptr.
inline void* feature(const LV2_Feature* const* features, const char* uri) {
  for (; features && *features; ++features)
    if (std::strcmp((*features)->URI, uri) == 0) return (*features)->data;
  return nullptr;
}

}  // namespace squatch::lv2
