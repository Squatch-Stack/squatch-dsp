// SPDX-License-Identifier: MIT
// Loading a cabinet impulse response for the Cabinet block, off the audio thread: a WAV file
// (PCM 16/24/32-bit or 32-bit float, any channel count: the first channel is used), resampled
// to the host's rate when it differs, its silent tail trimmed, cut to a maximum length, and
// normalised to unit energy so that swapping cabinets keeps roughly the same loudness.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace squatch::host {

struct Wav {
  std::vector<float> samples;  // the first channel
  double rate = 0.0;
  std::string error;           // empty when it loaded
};

namespace wav_detail {

struct Format {
  int tag = 0, channels = 0, bits = 0;
  double rate = 0.0;
};

inline std::uint32_t le(const unsigned char* p, int bytes) {
  std::uint32_t v = 0;
  for (int i = bytes - 1; i >= 0; --i) v = (v << 8) | p[i];
  return v;
}

inline float sample(const unsigned char* p, const Format& f) {
  if (f.tag == 3 && f.bits == 32) {
    float v;
    const std::uint32_t u = le(p, 4);
    static_assert(sizeof v == sizeof u, "IEEE single");
    std::memcpy(&v, &u, sizeof v);
    return v;
  }
  if (f.bits == 16) return static_cast<float>(static_cast<std::int16_t>(le(p, 2))) / 32768.0f;
  if (f.bits == 24) return static_cast<float>(static_cast<std::int32_t>(le(p, 3) << 8) / 256) / 8388608.0f;
  return static_cast<float>(static_cast<double>(static_cast<std::int32_t>(le(p, 4))) / 2147483648.0);
}

inline bool supported(const Format& f) {
  const bool pcm = f.tag == 1 && (f.bits == 16 || f.bits == 24 || f.bits == 32);
  return f.channels >= 1 && f.rate >= 8000.0 && (pcm || (f.tag == 3 && f.bits == 32));
}

inline Format parseFmt(const unsigned char* p, std::uint32_t size) {
  Format f;
  if (size < 16) return f;
  f.tag = static_cast<int>(le(p, 2));
  f.channels = static_cast<int>(le(p + 2, 2));
  f.rate = le(p + 4, 4);
  f.bits = static_cast<int>(le(p + 14, 2));
  if (f.tag == 0xFFFE && size >= 26) f.tag = static_cast<int>(le(p + 24, 2));  // WAVE_FORMAT_EXTENSIBLE
  return f;
}

inline void decode(const std::vector<unsigned char>& data, const Format& f, Wav& w) {
  const std::size_t frame = static_cast<std::size_t>(f.channels) * static_cast<std::size_t>(f.bits / 8);
  w.samples.resize(data.size() / frame);
  for (std::size_t i = 0; i < w.samples.size(); ++i) w.samples[i] = sample(&data[i * frame], f);
  w.rate = f.rate;
}

inline std::vector<unsigned char> readAll(const char* path) {
  std::vector<unsigned char> bytes;
  FILE* file = std::fopen(path, "rb");
  if (!file) return bytes;
  unsigned char buf[65536];
  for (std::size_t got; (got = std::fread(buf, 1, sizeof buf, file)) > 0;) bytes.insert(bytes.end(), buf, buf + got);
  std::fclose(file);
  return bytes;
}

}  // namespace wav_detail

/// Reads a whole WAV file. On failure, `error` says why and `samples` is empty.
inline Wav readWav(const char* path) {
  using namespace wav_detail;
  Wav w;
  const std::vector<unsigned char> b = readAll(path);
  if (b.size() < 12 || le(&b[0], 4) != 0x46464952u || le(&b[8], 4) != 0x45564157u) {  // "RIFF" "WAVE"
    w.error = b.empty() ? "cannot read the file" : "not a RIFF/WAVE file";
    return w;
  }
  Format f;
  for (std::size_t at = 12; at + 8 <= b.size();) {
    const std::uint32_t id = le(&b[at], 4), size = le(&b[at + 4], 4);
    const std::size_t body = at + 8, len = std::min<std::size_t>(size, b.size() - body);
    if (id == 0x20746d66u) f = parseFmt(&b[body], static_cast<std::uint32_t>(len));  // "fmt "
    if (id == 0x61746164u) {                                                           // "data"
      if (!supported(f)) break;
      decode(std::vector<unsigned char>(b.begin() + static_cast<long>(body), b.begin() + static_cast<long>(body + len)), f, w);
      return w;
    }
    at = body + size + (size & 1u);
  }
  w.error = supported(f) ? "no data chunk" : "unsupported format (PCM 16/24/32 or float 32 only)";
  return w;
}

/// Band-limited resampling (windowed sinc, 32 zero crossings a side, Blackman window), for
/// short signals such as impulse responses. The cutoff is the lower of the two Nyquists.
inline std::vector<float> resample(const std::vector<float>& x, double from, double to) {
  constexpr int kHalf = 32;
  constexpr double kPi = 3.14159265358979323846;
  const double ratio = to / from, cutoff = std::fmin(1.0, ratio);
  const auto n = static_cast<std::size_t>(std::floor(static_cast<double>(x.size()) * ratio));
  std::vector<float> y(n);
  const double reach = kHalf / cutoff;
  for (std::size_t m = 0; m < n; ++m) {
    const double centre = static_cast<double>(m) / ratio;
    const long lo = static_cast<long>(std::ceil(centre - reach)), hi = static_cast<long>(std::floor(centre + reach));
    double sum = 0.0;
    for (long k = std::max(lo, 0L); k <= hi && k < static_cast<long>(x.size()); ++k) {
      const double d = (static_cast<double>(k) - centre) * cutoff;
      const double sinc = d == 0.0 ? 1.0 : std::sin(kPi * d) / (kPi * d);
      const double w = 0.42 + 0.5 * std::cos(kPi * d / kHalf) + 0.08 * std::cos(2.0 * kPi * d / kHalf);
      sum += x[static_cast<std::size_t>(k)] * sinc * w;
    }
    y[m] = static_cast<float>(sum * cutoff);
  }
  return y;
}

/// Drops the tail below `floorDb` relative to the peak, cuts to `maxSamples`, and scales to
/// unit energy (sum of squares 1). Empty input stays empty.
inline void shapeResponse(std::vector<float>& h, double floorDb, std::size_t maxSamples) {
  float peak = 0.0f;
  for (float v : h) peak = std::fmax(peak, std::fabs(v));
  const float floor = peak * static_cast<float>(std::pow(10.0, floorDb / 20.0));
  std::size_t keep = h.size();
  while (keep > 1 && std::fabs(h[keep - 1]) < floor) --keep;
  h.resize(std::min(keep, maxSamples));
  double energy = 0.0;
  for (float v : h) energy += double(v) * v;
  if (energy > 0.0)
    for (float& v : h) v = static_cast<float>(v / std::sqrt(energy));
}

struct Response {
  std::vector<float> taps;
  double fileRate = 0.0;
  std::size_t fileSamples = 0;
  bool resampled = false;
  std::string error;
};

/// The whole pipeline: read, resample to `rate` (only when `allowResample`; otherwise a rate
/// mismatch is an error), shape. maxMs bounds the length, and with it the CPU cost.
inline Response loadResponse(const char* path, double rate, double maxMs, bool allowResample) {
  Response r;
  Wav w = readWav(path);
  r.error = w.error;
  r.fileRate = w.rate;
  r.fileSamples = w.samples.size();
  if (!r.error.empty()) return r;
  if (std::fabs(w.rate - rate) > 0.5) {
    if (!allowResample) {
      r.error = "the response is at " + std::to_string(static_cast<int>(w.rate)) + " Hz and JACK runs at " +
                std::to_string(static_cast<int>(rate)) + " Hz (allow resampling, or use a file at the JACK rate)";
      return r;
    }
    w.samples = resample(w.samples, w.rate, rate);
    r.resampled = true;
  }
  shapeResponse(w.samples, -80.0, static_cast<std::size_t>(maxMs * 1e-3 * rate));
  r.taps = std::move(w.samples);
  if (r.taps.empty()) r.error = "the response is empty";
  return r;
}

}  // namespace squatch::host
