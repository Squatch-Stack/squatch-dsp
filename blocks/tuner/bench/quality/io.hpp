// SPDX-License-Identifier: MIT
// What the quality run writes. The frame format is the contract every platform emits (Mac, Pi,
// phone, S3), so two platforms' runs over the same file can be compared line by line:
//
//   t_ms,note,octave,cents,hz,valid,shown_cents
//
//   t_ms         audio position of the reading shown, ms from the start of the file
//                (TunerSnapshot.samples / rate * 1000), three decimals
//   note,octave  the note on screen ("C#", 3), live or held; both empty when none is shown
//   cents        the live reading's cents against A4 = 440 (DisplayReading.cents), empty when
//                the tuner is not reading
//   hz           the live steadyHz (the number's source), empty when not reading
//   valid        1 while the tuner reads, else 0
//   shown_cents  the number on screen with one decimal (what "+0.3" means), empty for "--.-"
//                or when no note is shown
//
// Bulk files put the case id first (case,t_ms,...); a per-file frame log has no case column.
#pragma once
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "cases.hpp"
#include "display_path.hpp"

namespace quality {

inline const char* kFrameHeader = "t_ms,note,octave,cents,hz,valid,shown_cents";

inline void appendFrame(std::string& out, const Frame& f) {
  char b[160];
  const int n = std::snprintf(b, sizeof b, "%.3f,", f.tMs);
  out.append(b, static_cast<size_t>(n));
  if (f.noteShown) out += std::string(f.name) + "," + std::to_string(f.octave) + ",";
  else out += ",,";
  if (f.live) {
    std::snprintf(b, sizeof b, "%.4f,%.4f,1,", static_cast<double>(f.cents), static_cast<double>(f.hz));
    out += b;
  } else {
    out += ",,0,";
  }
  if (f.noteShown && std::isfinite(f.shown)) {
    std::snprintf(b, sizeof b, "%.1f", static_cast<double>(f.shown));
    out += b;
  }
  out += "\n";
}

/// Frames as CSV rows, each prefixed with `prefix` (the case id and a comma, or nothing).
inline std::string framesCsv(const std::vector<Frame>& frames, const std::string& prefix) {
  std::string out;
  for (const Frame& f : frames) {
    out += prefix;
    appendFrame(out, f);
  }
  return out;
}

inline const char* kTruthHeader = "t_ms,truth_hz,truth_midi,steady,segment";

inline std::string truthCsv(const std::vector<TruthRow>& rows, const std::string& prefix) {
  std::string out;
  char b[128];
  for (const TruthRow& r : rows) {
    std::snprintf(b, sizeof b, "%.3f,%.6f,%d,%d,%d\n", r.tMs, r.hz, r.midi, r.steady ? 1 : 0, r.segment);
    out += prefix + b;
  }
  return out;
}

inline std::string jsonString(const std::string& s) {
  std::string o = "\"";
  for (char c : s) {
    if (c == '"' || c == '\\') o += '\\';
    o += c;
  }
  return o + "\"";
}

inline std::string segmentJson(const Segment& g) {
  char b[256];
  std::snprintf(b, sizeof b, "{\"onset\":%.4f,\"end\":%.4f,\"midi\":%d,\"truth\":\"%s\",\"ref_hz\":%.6f,\"ref_unc_cents\":%.4f,\"ref_drift_cents\":%.4f,\"ref_B\":%.3g}",
                g.onset, g.end, g.midi, g.truth.c_str(), g.refHz, g.refUncCents, g.refDriftCents, g.refB);
  return b;
}

/// One line of cases.jsonl. `file` is written relative to the corpus root, never as a home path.
inline std::string caseJson(const Case& c, const std::string& corpusRoot) {
  std::string file = c.file;
  if (!corpusRoot.empty() && file.rfind(corpusRoot, 0) == 0) file = file.substr(corpusRoot.size() + 1);
  std::string o = "{\"id\":" + jsonString(c.id) + ",\"dataset\":" + jsonString(c.dataset) + ",\"family\":" + jsonString(c.family) +
                  ",\"value\":" + jsonString(c.value) + ",\"file\":" + jsonString(file) + ",\"source\":" + jsonString(c.source);
  char b[96];
  std::snprintf(b, sizeof b, ",\"fs\":%.0f,\"seconds\":%.3f,\"segments\":[", c.fs, static_cast<double>(c.x.size()) / c.fs);
  o += b;
  for (size_t i = 0; i < c.segments.size(); ++i) o += (i ? "," : "") + segmentJson(c.segments[i]);
  return o + "]}";
}

/// A mono 32-bit float WAV (WAVE_FORMAT_IEEE_FLOAT), as the phone's file input reads it.
inline bool writeFloatWav(const std::string& path, const std::vector<float>& x, uint32_t rate) {
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return false;
  const uint32_t data = static_cast<uint32_t>(x.size() * 4), riff = 36 + data, fmtLen = 16, byteRate = rate * 4;
  const uint16_t tag = 3, ch = 1, align = 4, bits = 32;
  std::fwrite("RIFF", 1, 4, f);
  std::fwrite(&riff, 4, 1, f);
  std::fwrite("WAVEfmt ", 1, 8, f);
  std::fwrite(&fmtLen, 4, 1, f);
  std::fwrite(&tag, 2, 1, f);
  std::fwrite(&ch, 2, 1, f);
  std::fwrite(&rate, 4, 1, f);
  std::fwrite(&byteRate, 4, 1, f);
  std::fwrite(&align, 2, 1, f);
  std::fwrite(&bits, 2, 1, f);
  std::fwrite("data", 1, 4, f);
  std::fwrite(&data, 4, 1, f);
  const bool ok = std::fwrite(x.data(), 4, x.size(), f) == x.size();
  return std::fclose(f) == 0 && ok;
}

}  // namespace quality
